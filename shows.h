#pragma once
// Hourly shows: short animations layered over the clock. Each draws through a full-screen overlay object above
// the clock widgets (hidden, so free, between shows) and invalidates only the areas it touches. While a show
// runs, fast_cb is called every SHOW_FRAME_MS; the frame rate is then limited by rendering + flushing.

static constexpr int SHOW_FRAME_MS = 30;
static constexpr int SHOW_FIRST_HOUR = 7, SHOW_LAST_HOUR = 22;   // quiet hours outside this range

struct ShowDef {
  const char* name;
  int duration_ms;
  void (*begin)();
  void (*tick)(float t);            // t: seconds since the start. Invalidate whatever changes.
  void (*draw)(lv_layer_t* L);      // called for each area LVGL redraws while the show runs
  void (*end)();
};

static lv_obj_t* show_obj = nullptr;
static int g_show = -1;
static int64_t g_show_t0 = 0, g_show_busy0 = 0;
static uint32_t g_show_frames0 = 0;
static int g_show_last_hour = -1;
static bool g_shows_enabled = true;
static float g_show_freeze = -1;
static int64_t g_show_tick_us = 0, g_show_tick_max = 0;   // time spent in the show's own tick (CPU-side work)
static uint32_t g_show_ticks = 0;   // >= 0: hold the running show at this time (for screenshots)

// ----------------------------------------------------------------------------------------------
// Benchmark: redraw the whole dial every frame without drawing anything extra
// ----------------------------------------------------------------------------------------------
static void bench_tick(float) { inv_box(CX - 345, CY - 345, CX + 345, CY + 345); }

// ----------------------------------------------------------------------------------------------
// Shared helpers
// ----------------------------------------------------------------------------------------------
static inline float ease(float t) { t = clamp01(t); return t * t * (3 - 2 * t); }
static inline float frand() { return (esp_random() >> 8) / 16777216.0f; }
static inline bool area_clip(lv_area_t& out, const lv_area_t& a, const lv_area_t& b) {
  out.x1 = max(a.x1, b.x1); out.y1 = max(a.y1, b.y1); out.x2 = min(a.x2, b.x2); out.y2 = min(a.y2, b.y2);
  return out.x1 <= out.x2 && out.y1 <= out.y2;
}
static inline bool clip_hit_xy(lv_layer_t* L, float x1, float y1, float x2, float y2) {
  const lv_area_t& c = L->_clip_area;
  return x2 >= c.x1 && x1 <= c.x2 && y2 >= c.y1 && y1 <= c.y2;
}
static void show_image(lv_layer_t* L, const lv_draw_buf_t* b, float x, float y, lv_opa_t opa,
                       lv_blend_mode_t mode = LV_BLEND_MODE_NORMAL, int sx = LV_SCALE_NONE, int sy = LV_SCALE_NONE) {
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = b;
  d.opa = opa;
  d.blend_mode = mode;
  d.scale_x = sx;
  d.scale_y = sy;
  d.pivot.x = b->header.w / 2;
  d.pivot.y = b->header.h / 2;
  const lv_area_t a = {(int32_t)lroundf(x), (int32_t)lroundf(y), (int32_t)lroundf(x) + (int32_t)b->header.w - 1,
                       (int32_t)lroundf(y) + (int32_t)b->header.h - 1};
  lv_draw_image(L, &d, &a);
}
// A soft round glow (white core, coloured halo), premultiplied for additive blending.
static void make_glow(lv_draw_buf_t*& b, int size, uint32_t col, float core) {
  sprite_begin(b, size);
  const float c = (size - 1) / 2.0f, sig = size / 6.0f;
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      const float dx = x - c, dy = y - c, d2 = dx * dx + dy * dy;
      Rgba p = rgba(col, expf(-d2 / (2 * sig * sig)));
      over(p, rgba(0xFFFFFF, core * expf(-d2 / (2 * 0.3f * sig * 0.3f * sig))));
      sprite_put(b, x, y, p);
    }
}

// Core 0 is nearly idle, so heavy per-frame pixel work runs there, one frame ahead of what LVGL draws on core 1
// (double-buffered by the show): show_work_wait() collects the frame prepared for now, show_work_kick() starts the next.
static TaskHandle_t g_work_task = nullptr;
static SemaphoreHandle_t g_work_go = nullptr, g_work_done = nullptr;
static void (*g_work_fn)(float) = nullptr;
static volatile float g_work_t = 0;
static bool g_work_busy = false;

static void show_work_task(void*) {
  for (;;) {
    xSemaphoreTake(g_work_go, portMAX_DELAY);
    g_work_fn(g_work_t);
    xSemaphoreGive(g_work_done);
  }
}
static void show_work_kick(void (*fn)(float), float t) {
  if (!g_work_task) {
    g_work_go = xSemaphoreCreateBinary();
    g_work_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(show_work_task, "showwork", 4096, nullptr, 2, &g_work_task, 0);
  }
  g_work_fn = fn;
  g_work_t = t;
  g_work_busy = true;
  xSemaphoreGive(g_work_go);
}
static void show_work_wait() {
  if (!g_work_busy) return;
  if (xSemaphoreTake(g_work_done, pdMS_TO_TICKS(500)) != pdTRUE) Serial.println("WARNING: show worker timeout");
  g_work_busy = false;
}

// ----------------------------------------------------------------------------------------------
// 1. Light sweep: a slanted sheen across the calendar card, a glint with a tail around the dial's bezel
// ----------------------------------------------------------------------------------------------
static constexpr int CARD_X = PX, CARD_Y = 392, CARD_W = PW, CARD_H = 296;
static lv_draw_buf_t* g_sheen = nullptr;
static lv_draw_buf_t* g_glint = nullptr;
static lv_draw_buf_t* g_glint_s = nullptr;
static float g_sheen_x = -1e9f, g_glint_a = -1e9f;
static constexpr float SHEEN_SLANT = 0.45f, GLINT_TAIL = 80;   // tail length in degrees

static void sweep_begin() {
  const Theme& th = THEMES[g_theme];
  const int w = 120 + (int)(CARD_H * SHEEN_SLANT), h = CARD_H;
  if (!g_sheen) g_sheen = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
  else lv_image_cache_drop(g_sheen);
  const Rgba ink = rgba(th.ink, 1);
  for (int y = 0; y < h; y++) {
    const float edge = smooth(0, 34, y) * smooth(0, 34, h - 1 - y);   // fade out where the card's corners round off
    for (int x = 0; x < w; x++) {
      const float u = x - 60 - (h - 1 - y) * SHEEN_SLANT;              // distance from the band's centre line
      const float a = edge * (0.30f * expf(-u * u / (2 * 22.0f * 22.0f)) + 0.10f * expf(-u * u / (2 * 55.0f * 55.0f)));
      sprite_put(g_sheen, x, y, {ink.r, ink.g, ink.b, a});
    }
  }
  make_glow(g_glint, 72, th.acc1, 1.0f);
  make_glow(g_glint_s, 28, th.acc1, 0.8f);
  g_sheen_x = g_glint_a = -1e9f;
}

static float sweep_glint_angle(float t) { return t < 0.9f || t > 3.9f ? -1e9f : -40 + 400 * ease((t - 0.9f) / 3.0f); }

static void inv_glint(float a) {
  if (a < -1e8f) return;
  float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
  for (float d = a - GLINT_TAIL; d <= a; d += 10) {
    const float r = d * (float)M_PI / 180, x = CX + sinf(r) * 301.5f, y = CY - cosf(r) * 301.5f;
    x1 = fminf(x1, x); x2 = fmaxf(x2, x); y1 = fminf(y1, y); y2 = fmaxf(y2, y);
  }
  inv_box(x1 - 38, y1 - 38, x2 + 38, y2 + 38);
}

static void sweep_tick(float t) {
  const float sx = t < 0.2f || t > 2.0f ? -1e9f : CARD_X - g_sheen->header.w + (CARD_W + g_sheen->header.w) * ease((t - 0.2f) / 1.8f);
  if (sx != g_sheen_x) inv_box(CARD_X, CARD_Y, CARD_X + CARD_W - 1, CARD_Y + CARD_H - 1);
  g_sheen_x = sx;
  const float a = sweep_glint_angle(t);
  inv_glint(g_glint_a);
  inv_glint(a);
  g_glint_a = a;
}

static void sweep_draw(lv_layer_t* L) {
  if (g_sheen_x > -1e8f && clip_hit_xy(L, CARD_X, CARD_Y, CARD_X + CARD_W - 1, CARD_Y + CARD_H - 1)) {
    const lv_area_t saved = L->_clip_area;
    lv_area_t card = {CARD_X, CARD_Y, CARD_X + CARD_W - 1, CARD_Y + CARD_H - 1};
    if (area_clip(L->_clip_area, saved, card)) show_image(L, g_sheen, g_sheen_x, CARD_Y, LV_OPA_COVER, LV_BLEND_MODE_ADDITIVE);
    L->_clip_area = saved;
  }
  if (g_glint_a > -1e8f) {
    for (int i = 20; i >= 0; i--) {          // the tail: glow beads fading and shrinking towards the back
      const float d = (g_glint_a - i * (GLINT_TAIL / 20)) * (float)M_PI / 180, f = 1 - i / 21.0f;
      const lv_draw_buf_t* b = i == 0 ? g_glint : g_glint_s;
      const float h = b->header.w / 2.0f, x = CX + sinf(d) * 301.5f, y = CY - cosf(d) * 301.5f;
      if (clip_hit_xy(L, x - h, y - h, x + h, y + h))
        show_image(L, b, x - h, y - h, (lv_opa_t)(255 * f * f), LV_BLEND_MODE_ADDITIVE);
    }
  }
}

// ----------------------------------------------------------------------------------------------
// 2. Fireworks over the dial: rockets rise and burst into sparks with streaking trails. All sparks are splatted
// (saturating add of small glow kernels) into one off-screen buffer, which LVGL then blends additively in a single
// draw: the cost no longer depends on how many sparks overlap.
// ----------------------------------------------------------------------------------------------
struct Spark { float x, y, vx, vy, age, life; uint8_t r, g, b, big, burst; };
static constexpr int MAX_SPARKS = 900, N_BURSTS = 8;
static constexpr int FW_W = 700, FW_H = SCR_H;   // the area sparks may cover: the dial side of the screen
static Spark* g_sparks = nullptr;                 // MAX_SPARKS, in PSRAM while the show runs (internal RAM is WiFi's)
static int g_spark_n = 0;
struct Burst { float t, x, y; uint32_t c1, c2; bool fired; };
static Burst g_bursts[N_BURSTS];
static float g_fw_last_t = 0;
// Each burst's sparks get their own box; overlapping boxes are merged, so separate bursts redraw separate regions
// and no pixel is blended twice. Two buffers: core 0 renders the next frame into one while LVGL shows the other.
static constexpr int FW_MAX_BOX = N_BURSTS;
struct FwFrame {
  lv_draw_buf_t* buf = nullptr;                  // XRGB intensities (alpha 255), valid inside the boxes only
  lv_area_t boxes[FW_MAX_BOX];
  lv_draw_buf_t views[FW_MAX_BOX];
  int nbox = 0;
};
static FwFrame g_fw[2];
static int g_fw_front = 0;                         // the frame LVGL is drawing
static lv_area_t g_fw_old[FW_MAX_BOX];
static int g_fw_nold = 0;
static int8_t g_fw_owner[N_BURSTS];               // burst -> merged box
static uint32_t g_fw_rng = 1;
static inline uint32_t fw_rand() { g_fw_rng = g_fw_rng * 1664525u + 1013904223u; return g_fw_rng >> 8; }
static constexpr int K_SMALL = 5, K_BIG = 9;      // kernel radii
static uint8_t g_k_small[(2 * K_SMALL + 1) * (2 * K_SMALL + 1)], g_k_big[(2 * K_BIG + 1) * (2 * K_BIG + 1)];

static void make_kernel(uint8_t* k, int r) {
  const float sig = r / 2.6f;
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      const float d2 = x * x + y * y;
      const float v = 0.55f * expf(-d2 / (2 * sig * sig)) + 0.45f * expf(-d2 / (2 * 0.8f * 0.8f));   // halo + hot core
      k[(y + r) * (2 * r + 1) + x + r] = (uint8_t)fminf(255, v * 255);
    }
}

static void fw_prepare(float t);
static float g_fw_prev_t = 0;
static bool g_fw_ok = false;

static void fireworks_begin() {
  const Theme& th = THEMES[g_theme];
  for (FwFrame& f : g_fw) { if (!f.buf) f.buf = lv_draw_buf_create(FW_W, FW_H, LV_COLOR_FORMAT_ARGB8888, 0); f.nbox = 0; }
  if (!g_sparks) g_sparks = (Spark*)heap_caps_malloc(sizeof(Spark) * MAX_SPARKS, MALLOC_CAP_SPIRAM);
  g_fw_ok = g_sparks && g_fw[0].buf && g_fw[1].buf;
  g_spark_n = 0;
  if (!g_fw_ok) return;
  if (!g_k_small[0]) { make_kernel(g_k_small, K_SMALL); make_kernel(g_k_big, K_BIG); }
  const uint32_t pal[5] = {th.acc1, th.acc2, th.acc3, 0xFFD27A, 0xFF7AB8};
  g_fw_rng = esp_random() ^ (uint32_t)esp_timer_get_time();
  auto r01 = []() { return fw_rand() / 16777216.0f; };
  g_spark_n = 0;
  for (int i = 0; i < N_BURSTS; i++) {
    const float a = r01() * 2 * (float)M_PI, r = 40 + r01() * 190;
    g_bursts[i] = {0.4f + i * 1.25f + r01() * 0.35f, CX + sinf(a) * r, CY - 30 - cosf(a) * r * 0.75f,
                   pal[fw_rand() % 5], pal[fw_rand() % 5], false};
  }
  g_fw_last_t = 0;
  g_fw_nold = 0;
  g_fw_prev_t = 0;
  g_fw_front = 1;
  fw_prepare(0);                    // frame 0 into g_fw[0]; fireworks_tick swaps it in
}

static void spark_add(float x, float y, float vx, float vy, float life, uint32_t c, bool big, int burst) {
  if (g_spark_n >= MAX_SPARKS) return;
  g_sparks[g_spark_n++] = {x, y, vx, vy, 0, life, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)big, (uint8_t)burst};
}

static HOT void splat(lv_draw_buf_t* buf, int cx, int cy, const Spark& p, int bright, const lv_area_t& box) {   // bright: 0..256
  const int r = p.big ? K_BIG : K_SMALL, n = 2 * r + 1;
  const uint8_t* k = p.big ? g_k_big : g_k_small;
  const int x0 = max(cx - r, (int)box.x1), x1 = min(cx + r, (int)box.x2);
  const int y0 = max(cy - r, (int)box.y1), y1 = min(cy + r, (int)box.y2);
  // a little white in the colour makes the cores burn hot when sparks overlap
  const int cr = (p.r * 3 + 255) >> 2, cg = (p.g * 3 + 255) >> 2, cb = (p.b * 3 + 255) >> 2;
  for (int y = y0; y <= y1; y++) {
    uint8_t* row = (uint8_t*)buf->data + y * buf->header.stride;
    const uint8_t* kr = k + (y - cy + r) * n - cx + r;
    for (int x = x0; x <= x1; x++) {
      const int w = kr[x] * bright;   // 0..65280
      if (w < 256) continue;
      uint8_t* q = row + x * 4;
      q[0] = (uint8_t)min(255, q[0] + ((cb * w) >> 16));
      q[1] = (uint8_t)min(255, q[1] + ((cg * w) >> 16));
      q[2] = (uint8_t)min(255, q[2] + ((cr * w) >> 16));
    }
  }
}

// Runs on core 0 (show worker): advance the simulation to t and render it into the back frame.
static HOT void fw_prepare(float t) {
  FwFrame& F = g_fw[g_fw_front ^ 1];
  const float dt = fmaxf(0, fminf(0.1f, t - g_fw_last_t));
  g_fw_last_t = t;
  for (int bi = 0; bi < N_BURSTS; bi++) {
    Burst& b = g_bursts[bi];
    if (b.fired || t < b.t - 0.7f) continue;
    if (t < b.t) {   // the rocket climbing, shedding embers
      const float k = (t - (b.t - 0.7f)) / 0.7f, y = CY + 320 + (b.y - CY - 320) * ease(k);
      for (int i = 0; i < 2; i++) {
        const float r1 = fw_rand() / 16777216.0f, r2 = fw_rand() / 16777216.0f;
        spark_add(b.x + (r1 - 0.5f) * 4, y, (r2 - 0.5f) * 30, 40 + r1 * 40, 0.4f, 0xFFD27A, false, bi);
      }
      continue;
    }
    b.fired = true;
    const int n = 110 + (int)(fw_rand() % 50);
    const float vmax = 170 + fw_rand() / 16777216.0f * 90;
    for (int i = 0; i < n; i++) {
      const float a = fw_rand() / 16777216.0f * 2 * (float)M_PI, v = vmax * (0.35f + 0.65f * sqrtf(fw_rand() / 16777216.0f));
      const float sx = sinf(a), sy = cosf(a);   // start a little out along the direction: a ring, not a blob
      spark_add(b.x + sx * v * 0.03f, b.y + sy * v * 0.03f, sx * v, sy * v, 1.6f + fw_rand() / 16777216.0f * 1.2f,
                i & 1 ? b.c1 : b.c2, (fw_rand() & 3) == 0, bi);
    }
    spark_add(b.x, b.y, 0, 0, 0.12f, 0xFFFFFF, true, bi);   // the flash
  }
  const float drag = expf(-1.5f * dt);
  float bx1[N_BURSTS], by1[N_BURSTS], bx2[N_BURSTS], by2[N_BURSTS];
  for (int i = 0; i < N_BURSTS; i++) { bx1[i] = by1[i] = 1e9f; bx2[i] = by2[i] = -1e9f; }
  int n = 0;
  for (int i = 0; i < g_spark_n; i++) {
    Spark p = g_sparks[i];
    p.age += dt;
    if (p.age >= p.life || p.y > SCR_H + 20) continue;
    p.vx *= drag; p.vy = p.vy * drag + 60 * dt;
    p.x += p.vx * dt; p.y += p.vy * dt;
    g_sparks[n++] = p;
    const int b = p.burst;
    bx1[b] = fminf(bx1[b], fminf(p.x, p.x - p.vx * 0.09f)); bx2[b] = fmaxf(bx2[b], fmaxf(p.x, p.x - p.vx * 0.09f));
    by1[b] = fminf(by1[b], fminf(p.y, p.y - p.vy * 0.09f)); by2[b] = fmaxf(by2[b], fmaxf(p.y, p.y - p.vy * 0.09f));
  }
  g_spark_n = n;
  // one box per burst with live sparks, then merge overlapping boxes until all are disjoint
  lv_area_t* g_fw_boxes = F.boxes;
  int& g_fw_nbox = F.nbox;
  g_fw_nbox = 0;
  for (int b = 0; b < N_BURSTS; b++) {
    g_fw_owner[b] = -1;
    if (bx2[b] < bx1[b]) continue;
    lv_area_t a;
    a.x1 = max(0, (int)bx1[b] - K_BIG); a.y1 = max(0, (int)by1[b] - K_BIG);
    a.x2 = min(FW_W - 1, (int)bx2[b] + K_BIG); a.y2 = min(FW_H - 1, (int)by2[b] + K_BIG);
    if (a.x2 < a.x1 || a.y2 < a.y1) continue;
    g_fw_owner[b] = (int8_t)g_fw_nbox;
    g_fw_boxes[g_fw_nbox++] = a;
  }
  for (bool merged = true; merged;) {
    merged = false;
    for (int i = 0; i < g_fw_nbox && !merged; i++)
      for (int j = i + 1; j < g_fw_nbox && !merged; j++) {
        lv_area_t& A = g_fw_boxes[i];
        const lv_area_t& B = g_fw_boxes[j];
        if (B.x1 > A.x2 || B.x2 < A.x1 || B.y1 > A.y2 || B.y2 < A.y1) continue;
        A.x1 = min(A.x1, B.x1); A.y1 = min(A.y1, B.y1); A.x2 = max(A.x2, B.x2); A.y2 = max(A.y2, B.y2);
        for (int b = 0; b < N_BURSTS; b++) {
          if (g_fw_owner[b] == j) g_fw_owner[b] = (int8_t)i;
          else if (g_fw_owner[b] == g_fw_nbox - 1) g_fw_owner[b] = (int8_t)j;
        }
        g_fw_boxes[j] = g_fw_boxes[--g_fw_nbox];
        merged = true;
      }
  }
  for (int i = 0; i < g_fw_nbox; i++) {
    const lv_area_t& a = g_fw_boxes[i];
    for (int y = a.y1; y <= a.y2; y++) {
      uint32_t* row = (uint32_t*)((uint8_t*)F.buf->data + y * F.buf->header.stride) + a.x1;
      for (int x = 0; x <= a.x2 - a.x1; x++) row[x] = 0xFF000000u;
    }
  }
  for (int i = 0; i < n; i++) {
    const Spark& p = g_sparks[i];
    const lv_area_t& box = g_fw_boxes[g_fw_owner[p.burst]];
    const float k = p.age / p.life;
    float b = (1 - k * k) * fminf(1, p.age * 12 + 0.15f);           // ramp up over the first ~80 ms
    if (k > 0.6f && (fw_rand() & 3) == 0) b *= 0.35f;                 // twinkle as they burn out
    for (int j = 0; j < 3; j++)                                        // the spark and a streak behind it
      splat(F.buf, (int)(p.x - p.vx * 0.03f * j), (int)(p.y - p.vy * 0.03f * j), p, (int)(256 * b / (1 + j * j * 0.6f)), box);
  }
  for (int i = 0; i < g_fw_nbox; i++) {
    const lv_area_t& a = g_fw_boxes[i];
    const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    lv_draw_buf_init(&F.views[i], w, h, LV_COLOR_FORMAT_ARGB8888, F.buf->header.stride,
                     (uint8_t*)F.buf->data + a.y1 * F.buf->header.stride + a.x1 * 4, F.buf->header.stride * h);
  }
}

// UI thread: show the frame core 0 prepared, then have it prepare the next one.
static void fireworks_tick(float t) {
  if (!g_fw_ok) return;
  show_work_wait();
  const FwFrame& old = g_fw[g_fw_front];
  memcpy(g_fw_old, old.boxes, sizeof(g_fw_old));
  g_fw_nold = old.nbox;
  g_fw_front ^= 1;
  const FwFrame& F = g_fw[g_fw_front];
  for (int i = 0; i < F.nbox; i++) lv_inv_area(g_disp, &F.boxes[i]);
  for (int i = 0; i < g_fw_nold; i++) lv_inv_area(g_disp, &g_fw_old[i]);
  const float step = g_show_freeze >= 0 ? 0 : fminf(0.1f, fmaxf(0.03f, t - g_fw_prev_t));
  g_fw_prev_t = t;
  show_work_kick(fw_prepare, t + step);
}

static void fireworks_draw(lv_layer_t* L) {
  if (!g_fw_ok) return;
  const FwFrame& F = g_fw[g_fw_front];
  for (int i = 0; i < F.nbox; i++) {
    const lv_area_t& a = F.boxes[i];
    if (clip_hit_xy(L, a.x1, a.y1, a.x2, a.y2)) show_image(L, &F.views[i], a.x1, a.y1, LV_OPA_COVER, LV_BLEND_MODE_ADDITIVE);
  }
}
static void fireworks_end() {
  show_work_wait();
  for (FwFrame& f : g_fw) if (f.buf) { lv_draw_buf_destroy(f.buf); f.buf = nullptr; }   // 2 x 2 MB of PSRAM
  heap_caps_free(g_sparks);
  g_sparks = nullptr;
  g_fw_ok = false;
}

// ----------------------------------------------------------------------------------------------
// 3. Numerals take flight: the 12 numerals lift off the dial (the artwork behind them is painted in from per-theme
// patches), orbit on a tilted ring in perspective - scaled and faded by depth, drawn back to front, each with a soft
// glow - and land back in place.
// ----------------------------------------------------------------------------------------------
static lv_draw_buf_t* g_num_spr[12];               // at 1.0, as on the dial
static lv_draw_buf_t* g_num_hi[12];                // rendered with a 1.5x font, the source for the scaled copies
static constexpr int NUM_LV = 17;                  // scales 0.70 .. 1.50 in 0.05 steps
static constexpr float NUM_K0 = 0.70f, NUM_DK = 0.05f, NUM_HI = 1.5f;
static lv_draw_buf_t* g_num_lv[12][NUM_LV];
static volatile bool g_num_lv_ready = false;
static lv_draw_buf_t* g_num_glow = nullptr;
struct NumPos { float x, y, k, opa; };
static NumPos g_num_pos[12];
static lv_area_t g_num_inv[12];
static int g_num_order[12];
static bool g_num_ok = false;
static constexpr float NUM_T = 9.0f;
static lv_opa_t g_num_glow_opa = 0;

// Bilinear resample of a straight-alpha ARGB8888 sprite (premultiplied while filtering, so edges don't go dark).
static HOT lv_draw_buf_t* resample(const lv_draw_buf_t* src, float k) {
  const int sw = src->header.w, sh = src->header.h;
  const int dw = max(1, (int)lroundf(sw * k)), dh = max(1, (int)lroundf(sh * k));
  lv_draw_buf_t* dst = lv_draw_buf_create(dw, dh, LV_COLOR_FORMAT_ARGB8888, 0);
  if (!dst) return nullptr;
  const float inv = 1 / k;
  for (int y = 0; y < dh; y++) {
    const float fy = fmaxf(0, fminf(sh - 1.001f, (y + 0.5f) * inv - 0.5f));
    const int y0 = (int)fy; const float ty = fy - y0;
    const uint8_t* r0 = (const uint8_t*)src->data + y0 * src->header.stride;
    const uint8_t* r1 = r0 + src->header.stride;
    uint8_t* out = (uint8_t*)dst->data + y * dst->header.stride;
    for (int x = 0; x < dw; x++) {
      const float fx = fmaxf(0, fminf(sw - 1.001f, (x + 0.5f) * inv - 0.5f));
      const int x0 = (int)fx; const float tx = fx - x0;
      const uint8_t* p[4] = {r0 + x0 * 4, r0 + x0 * 4 + 4, r1 + x0 * 4, r1 + x0 * 4 + 4};
      const float w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
      float a = 0, c[3] = {0, 0, 0};
      for (int j = 0; j < 4; j++) {
        const float aw = p[j][3] * w[j];
        a += aw;
        for (int ch = 0; ch < 3; ch++) c[ch] += p[j][ch] * aw;
      }
      uint8_t* q = out + x * 4;
      if (a < 0.5f) { q[0] = q[1] = q[2] = q[3] = 0; continue; }
      for (int ch = 0; ch < 3; ch++) q[ch] = (uint8_t)fminf(255, c[ch] / a + 0.5f);
      q[3] = (uint8_t)fminf(255, a + 0.5f);
    }
  }
  return dst;
}

static void num_free_levels() {
  for (auto& row : g_num_lv) for (auto& b : row) if (b) { lv_draw_buf_destroy(b); b = nullptr; }
}
static void num_scale_all(float) {   // show worker (core 0)
  for (int i = 0; i < 12; i++)
    for (int l = 0; l < NUM_LV; l++) g_num_lv[i][l] = resample(g_num_hi[i], (NUM_K0 + l * NUM_DK) / NUM_HI);
  g_num_lv_ready = true;
}

static void numerals_begin() {
  const Theme& th = THEMES[g_theme];
  g_num_ok = true;
  show_work_wait();
  num_free_levels();
  for (int i = 0; i < 12; i++) if (!g_num_patch[g_theme][i]) g_num_ok = false;
  if (!g_num_ok) return;
  // Sprites: the same shadow + label pair as in the artwork, rendered onto transparency
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_remove_style_all(scr);
  for (int i = 0; i < 12; i++) {
    const lv_area_t& r = g_num_rect[i];
    lv_obj_t* box = mk(scr, 0, 0, r.x2 - r.x1 + 1, r.y2 - r.y1 + 1);
    char t[4];
    snprintf(t, sizeof(t), "%d", i + 1);
    const bool card = (i + 1) % 3 == 0;
    mk_label(box, f_num, 0x000000, LV_OPA_50, t, 1, 3, 100, LV_TEXT_ALIGN_CENTER);
    mk_label(box, f_num, card ? th.acc1 : th.ink, card ? LV_OPA_COVER : 225, t, 0, 0, 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_update_layout(box);
    if (g_num_spr[i]) lv_draw_buf_destroy(g_num_spr[i]);
    g_num_spr[i] = lv_snapshot_take(box, LV_COLOR_FORMAT_ARGB8888);
    if (!g_num_spr[i]) g_num_ok = false;
  }
  // the same at 1.5x size: core 0 downsamples these to every scale step while the numerals are still at home
  lv_font_t* big = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, (int)(46 * NUM_HI));
  for (int i = 0; i < 12 && big; i++) {
    const lv_area_t& r = g_num_rect[i];
    const int w = (int)((r.x2 - r.x1 + 1) * NUM_HI), h = (int)((r.y2 - r.y1 + 1) * NUM_HI);
    lv_obj_t* box = mk(scr, 0, 0, w, h);
    char t[4];
    snprintf(t, sizeof(t), "%d", i + 1);
    const bool card = (i + 1) % 3 == 0;
    lv_obj_t* l1 = mk_label(box, big, 0x000000, LV_OPA_50, t, 2, 4, w - 2, LV_TEXT_ALIGN_CENTER);
    lv_obj_t* l2 = mk_label(box, big, card ? th.acc1 : th.ink, card ? LV_OPA_COVER : 225, t, 0, 0, w - 2, LV_TEXT_ALIGN_CENTER);
    (void)l1; (void)l2;
    lv_obj_update_layout(box);
    if (g_num_hi[i]) lv_draw_buf_destroy(g_num_hi[i]);
    g_num_hi[i] = lv_snapshot_take(box, LV_COLOR_FORMAT_ARGB8888);
    if (!g_num_hi[i]) g_num_ok = false;
  }
  lv_obj_delete(scr);
  if (big) lv_tiny_ttf_destroy(big); else g_num_ok = false;
  make_glow(g_num_glow, 110, th.acc1, 0.0f);
  if (!g_num_ok) return;
  g_num_lv_ready = false;
  show_work_kick(num_scale_all, 0);
  g_hide_numerals = true;
  for (const lv_area_t& r : g_num_rect) comp_box(r.x1, r.y1, r.x2, r.y2);
  for (int i = 0; i < 12; i++) {
    const lv_area_t& r = g_num_rect[i];
    g_num_pos[i] = {(r.x1 + r.x2) / 2.0f, (r.y1 + r.y2) / 2.0f, 1, 255};
    g_num_inv[i] = r;
    g_num_order[i] = i;
  }
}

static void numerals_tick(float t) {
  if (!g_num_ok) return;
  // envelope: up in the first ~1.2 s, down over the last ~2 s; the ring turns once, so every numeral lands home
  const float env = ease((t - 0.3f) / 1.2f) * (1 - ease((t - (NUM_T - 2.6f)) / 1.8f));
  const float spin = 2 * (float)M_PI * ease((t - 0.5f) / (NUM_T - 1.4f));
  const float tilt = 1.10f * env, ct = cosf(tilt), st = sinf(tilt);
  const float breathe = 1 + 0.16f * env * sinf(2 * (float)M_PI * (t - 0.5f) / 3.2f);
  lv_area_t boxes[24];
  int nb = 0;
  for (int i = 0; i < 12; i++) {
    const lv_area_t& r = g_num_rect[i];
    const float hx = (r.x1 + r.x2) / 2.0f - CX, hy = (r.y1 + r.y2) / 2.0f - CY;   // home, relative to the centre
    const float a = atan2f(hx, -hy) + spin, rad = sqrtf(hx * hx + hy * hy) * breathe;
    const float x = sinf(a) * rad, y = -cosf(a) * rad;
    const float z = 46 * env * sinf(3 * a + 2.4f * t);                                 // a ripple around the ring
    const float y2 = y * ct - z * st, z2 = y * st + z * ct;
    const float persp = 900 / (900 + z2);
    const float k = persp * (1 + 0.22f * env);
    NumPos& p = g_num_pos[i];
    p = {CX + x * persp, CY + y2 * persp - 24 * env, k, 255 * clamp01(0.45f + 0.55f * (persp - 0.75f) / 0.45f)};
    boxes[nb++] = g_num_inv[i];                                                        // where it was
    const float hw = 52 * k + 4, hh = 50 * k + 4;                                      // glow is wider than the digits
    lv_area_t& b = g_num_inv[i];
    b.x1 = max(0, (int)(p.x - hw)); b.y1 = max(0, (int)(p.y - hh));
    b.x2 = min(SCR_W - 1, (int)(p.x + hw)); b.y2 = min(SCR_H - 1, (int)(p.y + hh));
    boxes[nb++] = b;
  }
  merge_dirty_regions(boxes, nb);
  if (nb > 10) {   // keep LVGL's dirty-area list from overflowing into a full-screen redraw
    for (int i = 1; i < nb; i++) {
      boxes[0].x1 = min(boxes[0].x1, boxes[i].x1); boxes[0].y1 = min(boxes[0].y1, boxes[i].y1);
      boxes[0].x2 = max(boxes[0].x2, boxes[i].x2); boxes[0].y2 = max(boxes[0].y2, boxes[i].y2);
    }
    nb = 1;
  }
  for (int i = 0; i < nb; i++) lv_inv_area(g_disp, &boxes[i]);
  for (int i = 0; i < 12; i++) g_num_order[i] = i;                                     // back to front
  for (int a = 1; a < 12; a++)
    for (int b = a; b > 0 && g_num_pos[g_num_order[b]].k < g_num_pos[g_num_order[b - 1]].k; b--) {
      const int x = g_num_order[b]; g_num_order[b] = g_num_order[b - 1]; g_num_order[b - 1] = x;
    }
  g_num_glow_opa = (lv_opa_t)(70 * env);
}

static void numerals_draw(lv_layer_t* L) {
  if (!g_num_ok) return;
  for (int j = 0; j < 12; j++) {
    const int i = g_num_order[j];
    const NumPos& p = g_num_pos[i];
    if (!clip_hit_xy(L, g_num_inv[i].x1, g_num_inv[i].y1, g_num_inv[i].x2, g_num_inv[i].y2)) continue;
    if (g_num_glow_opa > 4)
      show_image(L, g_num_glow, p.x - 55, p.y - 55, (lv_opa_t)(g_num_glow_opa * p.opa / 255), LV_BLEND_MODE_ADDITIVE);
    const lv_draw_buf_t* b = g_num_spr[i];
    if (g_num_lv_ready) {   // nearest pre-scaled copy: a plain blit, no per-frame transform
      const int l = max(0, min(NUM_LV - 1, (int)lroundf((p.k - NUM_K0) / NUM_DK)));
      if (g_num_lv[i][l]) b = g_num_lv[i][l];
    }
    show_image(L, b, p.x - b->header.w / 2.0f, p.y - b->header.h / 2.0f, (lv_opa_t)p.opa);
  }
}

static void numerals_end() {
  show_work_wait();
  num_free_levels();
  if (!g_hide_numerals) return;
  g_hide_numerals = false;
  for (const lv_area_t& r : g_num_rect) comp_box(r.x1, r.y1, r.x2, r.y2);
}

// ----------------------------------------------------------------------------------------------
// 4. Split-flap: the big digits become a departure board. Cards fade in under the clock's fixed digit slots (rounded,
// shaded upper and lower flaps, a lit top edge, a hinge with side pins, a soft shadow), every digit flips through a
// few random digits before landing on the time - later columns flip longer, so it settles left to right.
// Each flip folds the old upper flap down and unfolds the new lower flap around the hinge, darkening the moving leaf.
// ----------------------------------------------------------------------------------------------
static lv_draw_buf_t* g_glyph[11];                 // '0'..'9', ':'
static char g_flap_text[8];
static int g_flap_n = 0;
struct FlapCol { float cx; int x1, x2; bool card; int flips; char seq[12]; float t0; };   // slot centre, card extent
static FlapCol g_flap[8];
static int g_flap_y = 0, g_flap_mid = 0, g_flap_h = 0;
static int g_flap_top = 0, g_flap_bot = 0;         // digit ink extent within a glyph sprite
static int g_card_w = 0;                           // every card has the same size
static float g_flap_m = 0;                         // 0 = digits at their natural places, 1 = on the cards
static constexpr float FLAP_T = 4.6f;
static lv_area_t g_flap_box;
static constexpr float FLAP_DT = 0.20f;            // one flip
static constexpr int CARD_R = 14, CARD_PAD = 15;

static inline int glyph_idx(char c) { return c == ':' ? 10 : c >= '0' && c <= '9' ? c - '0' : -1; }
static bool g_flap_ok = false;

static void flap_begin() {
  const Theme& th = THEMES[g_theme];
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_remove_style_all(scr);
  const char* chars = "0123456789:";
  int ext = 0;                                     // lv_snapshot_take includes the label's extra draw margin
  for (int i = 0; i < 11; i++) {
    char t[2] = {chars[i], 0};
    lv_obj_t* l = mk_label(scr, f_digits, th.ink, LV_OPA_COVER, t, 0, 0);   // styled exactly like lbl_time
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_update_layout(l);
    if (g_glyph[i]) lv_draw_buf_destroy(g_glyph[i]);
    g_glyph[i] = lv_snapshot_take(l, LV_COLOR_FORMAT_ARGB8888);
    if (g_glyph[i]) ext = ((int)g_glyph[i]->header.w - (int)lv_obj_get_width(l)) / 2;
  }
  lv_obj_delete(scr);
  g_flap_ok = true;
  for (int i = 0; i < 11; i++) if (!g_glyph[i]) g_flap_ok = false;
  if (!g_flap_ok) { Serial.println("flap: glyph snapshot failed"); return; }
  // the hinge sits at the middle of the digits' ink (from '0')
  int top = -1, bot = -1;
  const lv_draw_buf_t* z = g_glyph[0];
  auto row_inked = [z](int y) {
    for (int x = 0; x < (int)z->header.w; x++)
      if (((const uint8_t*)z->data)[y * z->header.stride + x * 4 + 3] > 96) return true;
    return false;
  };
  for (int y = 0; y < (int)z->header.h; y++) if (row_inked(y)) { top = y; break; }
  for (int y = (int)z->header.h - 1; y >= 0; y--) if (row_inked(y)) { bot = y; break; }
  g_flap_h = z->header.h;
  g_flap_mid = top >= 0 && bot >= 0 ? (top + bot) / 2 : g_flap_h / 2;
  g_flap_top = top >= 0 ? top : 0;
  g_flap_bot = bot >= 0 ? bot : g_flap_h - 1;
  // the board is the clock's own time layout: one card per digit slot (blank ones too), the colon slot bare.
  // A glyph sprite centred on its slot lands exactly where the slot label draws it.
  g_flap_y = TIME_Y - ext;
  g_flap_n = 5;
  float t0 = 0.30f;
  int k = 0;
  for (int i = 0; i < 5; i++) {
    FlapCol& col = g_flap[i];
    const char c = lv_label_get_text(lbl_slot[i])[0];
    col.cx = g_slot_x[i] + g_slot_w[i] / 2.0f;
    col.x1 = g_slot_x[i] + 4;
    col.x2 = g_slot_x[i] + g_slot_w[i] - 6;
    col.card = i != 2;
    const bool digit = c >= '0' && c <= '9';
    col.flips = digit ? 4 + 2 * k : 0;
    for (int f = 0; f < col.flips; f++) col.seq[f] = '0' + esp_random() % 10;
    col.seq[col.flips] = c ? c : ' ';
    col.t0 = t0;
    if (digit) { t0 += 0.10f; k++; }
  }
  g_card_w = g_slot_w[0] - 10;
  g_flap_box = {(int32_t)g_slot_x[0] - 12, (int32_t)(g_flap_y + g_flap_top - CARD_PAD - 4),
                (int32_t)(g_slot_x[4] + g_slot_w[4] + 12), (int32_t)(g_flap_y + g_flap_bot + CARD_PAD + 14)};
  g_flap_box.y1 = min(g_flap_box.y1, (int32_t)TIME_Y);
  g_flap_m = 0;
  for (lv_obj_t* l : lbl_slot) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}

static float g_flap_t = 0;
static void flap_tick(float t) {
  if (!g_flap_ok) return;
  g_flap_t = t;
  g_flap_m = ease(t / 0.3f) * (1 - ease((t - (FLAP_T - 0.5f)) / 0.45f));   // the cards fade in and out
  lv_inv_area(g_disp, &g_flap_box);
}

static lv_color_t shade_to(lv_color_t c, lv_opa_t shade) { return lv_color_mix(lv_color_black(), c, shade); }

// One flap of a card - its upper or lower half, or the moving leaf - with the glyph half on it. `s`: how far the
// leaf is open (1 = flat), squeezed towards the hinge; `shade` darkens it as it turns away.
static void flap_piece(lv_layer_t* L, char c, const FlapCol& col, bool upper, float s, lv_opa_t shade, lv_opa_t card) {
  const Theme& th = THEMES[g_theme];
  const int ym = g_flap_y + g_flap_mid;
  const int x1 = col.x1, x2 = col.x2;
  const int cy1 = g_flap_y + g_flap_top - CARD_PAD, cy2 = g_flap_y + g_flap_bot + CARD_PAD;
  const bool is_card = col.card && card;
  lv_area_t piece = upper ? lv_area_t{x1, (int32_t)lroundf(ym - (ym - cy1) * s), x2, ym - 2}
                          : lv_area_t{x1, ym + 2, x2, (int32_t)lroundf(ym + 2 + (cy2 - ym - 2) * s)};
  if (!is_card) { piece.x1 -= 40; piece.x2 += 40; }   // the colon and the glyphs while the cards are hidden
  if (piece.y2 < piece.y1) return;
  lv_area_t cl;
  if (!area_clip(cl, L->_clip_area, piece)) return;
  const lv_area_t saved = L->_clip_area;
  L->_clip_area = cl;
  if (is_card) {
    // body: a lighter upper flap, a darker lower one; the gradient follows a moving leaf as it squeezes
    const lv_color_t base = lv_color_mix(lv_color_black(), lv_color_mix(lv_color_hex(th.acc3), lv_color_hex(th.bg_top), 50), 100);
    const lv_color_t light = lv_color_mix(lv_color_hex(th.ink), base, 40);
    const lv_color_t dark = lv_color_mix(lv_color_black(), base, 75);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = CARD_R;
    r.bg_opa = card;
    r.bg_color = shade_to(upper ? light : base, shade);
    r.bg_grad.dir = LV_GRAD_DIR_VER;
    r.bg_grad.stops_count = 2;
    r.bg_grad.stops[0].color = shade_to(upper ? light : base, shade);
    r.bg_grad.stops[1].color = shade_to(upper ? base : dark, shade);
    r.bg_grad.stops[0].opa = r.bg_grad.stops[1].opa = card;
    r.bg_grad.stops[0].frac = 0;
    r.bg_grad.stops[1].frac = 255;
    r.border_width = 1;
    r.border_color = lv_color_black();
    r.border_opa = (lv_opa_t)(card * 140 / 255);
    // a whole rounded card per flap, clipped to the flap: rounded outer corners, square at the hinge
    const lv_area_t body = upper ? lv_area_t{x1, piece.y1, x2, ym + CARD_R} : lv_area_t{x1, ym - CARD_R, x2, piece.y2};
    lv_draw_rect(L, &r, &body);
    if (upper && s >= 0.995f) {        // the lit top edge
      lv_draw_rect_dsc_t e;
      lv_draw_rect_dsc_init(&e);
      e.bg_color = lv_color_hex(th.ink);
      e.bg_opa = (lv_opa_t)(card * 60 / 255);
      const lv_area_t edge = {x1 + CARD_R, cy1 + 1, x2 - CARD_R, cy1 + 1};
      lv_draw_rect(L, &e, &edge);
    }
  }
  const int gi = glyph_idx(c);
  if (const lv_draw_buf_t* b = gi >= 0 ? g_glyph[gi] : nullptr) {
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    d.scale_y = s >= 0.995f ? LV_SCALE_NONE : max(1, (int)(256 * s));
    d.pivot.x = b->header.w / 2;
    d.pivot.y = g_flap_mid;
    d.recolor = lv_color_black();
    d.recolor_opa = shade;
    const int x = (int)lroundf(col.cx - b->header.w / 2.0f);
    const lv_area_t a = {x, g_flap_y, x + (int32_t)b->header.w - 1, g_flap_y + (int32_t)b->header.h - 1};
    lv_draw_image(L, &d, &a);
  }
  L->_clip_area = saved;
}

static void flap_card_frame(lv_layer_t* L, const FlapCol& col, lv_opa_t card, bool shadow) {
  const int ym = g_flap_y + g_flap_mid;
  const int x1 = col.x1, x2 = col.x2;
  const int cy1 = g_flap_y + g_flap_top - CARD_PAD, cy2 = g_flap_y + g_flap_bot + CARD_PAD;
  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  if (shadow) {                        // a soft shadow under the card: stacked translucent rounded rects
    r.bg_color = lv_color_black();
    for (int k = 0; k < 3; k++) {
      r.radius = CARD_R + 2 + 2 * k;
      r.bg_opa = (lv_opa_t)(card * (36 - 10 * k) / 255);
      const lv_area_t a = {x1 - 1 - 2 * k, cy1 + 5 - k, x2 + 1 + 2 * k, cy2 + 7 + 2 * k};
      lv_draw_rect(L, &r, &a);
    }
    return;
  }
  // the hinge: a dark split, a lit bevel under it, a pin at each side
  r.bg_color = lv_color_black();
  r.bg_opa = (lv_opa_t)(card * 230 / 255);
  const lv_area_t split = {x1, ym - 1, x2, ym + 1};
  lv_draw_rect(L, &r, &split);
  r.bg_color = lv_color_hex(THEMES[g_theme].ink);
  r.bg_opa = (lv_opa_t)(card * 28 / 255);
  const lv_area_t bevel = {x1 + 2, ym + 2, x2 - 2, ym + 2};
  lv_draw_rect(L, &r, &bevel);
  r.bg_color = lv_color_mix(lv_color_hex(THEMES[g_theme].ink), lv_color_black(), 70);
  r.bg_opa = card;
  r.radius = 2;
  const lv_area_t pin_l = {x1 - 3, ym - 6, x1 + 2, ym + 6}, pin_r = {x2 - 2, ym - 6, x2 + 3, ym + 6};
  lv_draw_rect(L, &r, &pin_l);
  lv_draw_rect(L, &r, &pin_r);
}

static void flap_draw(lv_layer_t* L) {
  if (!g_flap_ok || !clip_hit_xy(L, g_flap_box.x1, g_flap_box.y1, g_flap_box.x2, g_flap_box.y2)) return;
  const lv_opa_t card = (lv_opa_t)(255 * g_flap_m);
  if (card) for (int i = 0; i < g_flap_n; i++) if (g_flap[i].card) flap_card_frame(L, g_flap[i], card, true);
  for (int i = 0; i < g_flap_n; i++) {
    const FlapCol& c = g_flap[i];
    const float u = (g_flap_t - c.t0) / FLAP_DT;
    const int k = u < 0 ? -1 : (int)u;                    // flip in progress: seq[k] -> seq[k+1]
    if (!c.flips || k < 0 || k >= c.flips) {
      const char ch = c.seq[k < 0 || !c.flips ? 0 : c.flips];
      flap_piece(L, ch, c, true, 1, 0, card);
      flap_piece(L, ch, c, false, 1, 0, card);
    } else {
      const char a = c.seq[k], b = c.seq[k + 1];
      const float p = u - k;
      flap_piece(L, b, c, true, 1, 0, card);              // behind the leaf: the next upper flap ...
      flap_piece(L, a, c, false, 1, 0, card);             // ... and the current lower one
      if (p < 0.5f) flap_piece(L, a, c, true, cosf(p * (float)M_PI), (lv_opa_t)(200 * p * 2), card);     // old upper falls
      else          flap_piece(L, b, c, false, -cosf(p * (float)M_PI), (lv_opa_t)(150 * (1 - p) * 2), card); // new lower lands
    }
    if (card && c.card) flap_card_frame(L, c, card, false);
  }
}

static void flap_end() {
  for (lv_obj_t* l : lbl_slot) lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
  for (auto& b : g_glyph) if (b) { lv_draw_buf_destroy(b); b = nullptr; }
}

// ----------------------------------------------------------------------------------------------
// 5. Dial flip: the whole dial turns about its vertical axis in perspective, shows a back face with the digital time
// and date for a few seconds, and turns back.
// LVGL's transforms are far too slow for a 600 px image, and routing a full-dial image through LVGL and the PPA every
// frame saturates PSRAM. So for the duration LVGL keeps out of the dial (g_show_owns_dial) and core 0 renders straight
// into the panel's frame buffer: the panel is portrait, so a screen column is one contiguous frame-buffer row, and the
// warp is column-separable (each screen column samples one face column, scaled by its depth). With the faces and
// the background stored column-major, every read and write is a sequential stream.
// ----------------------------------------------------------------------------------------------
static constexpr int DF_D = 640, DF_R = 306;                  // square region side, face radius
static constexpr int DF_X0 = CX - DF_D / 2, DF_Y0 = CY - DF_D / 2;
static constexpr float DF_ANIM = 7.0f, DF_F = 1400;           // animation length, perspective distance
static uint32_t* g_df_cm[2] = {nullptr, nullptr};            // front, back: ARGB words, column-major [x * h + y]
static int g_df_fw[2], g_df_fh[2];
static uint16_t* g_df_under = nullptr;                        // RGB565 behind the dial, column-major [x * D + y]
static lv_draw_buf_t *g_df_hands = nullptr, *g_df_back = nullptr;   // LVGL snapshots, handed to the worker
static bool g_df_ok = false;
static int g_df_wait = 0;                                     // ticks to wait for LVGL's own first refresh
static float g_df_t0 = -1;                                    // show time at which the animation starts
static float g_df_drawn = -1, g_df_pending = -1;              // angle on screen / being rendered
static int64_t g_df_us = 0;
static int g_df_frames = 0;

static float df_angle(float t) {   // degrees: 0 front, 180 back
  if (t < 0.2f) return 0;
  if (t < 1.6f) return 180 * ease((t - 0.2f) / 1.4f);
  if (t < 4.8f) return 180;
  if (t < 6.2f) return 180 + 180 * ease((t - 4.8f) / 1.4f);
  return 360;
}

// Row-major ARGB8888 -> column-major words, in 32x32 tiles so both sides stay in cache.
static uint32_t* df_columns(const lv_draw_buf_t* b, int& w, int& h) {
  w = b->header.w; h = b->header.h;
  uint32_t* cm = (uint32_t*)heap_caps_malloc((size_t)w * h * 4, MALLOC_CAP_SPIRAM);
  if (!cm) return nullptr;
  for (int ty = 0; ty < h; ty += 32)
    for (int tx = 0; tx < w; tx += 32)
      for (int y = ty; y < min(h, ty + 32); y++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)b->data + y * b->header.stride);   // BGRA = ARGB word
        for (int x = tx; x < min(w, tx + 32); x++) cm[x * h + y] = row[x];
      }
  return cm;
}

static HOT void df_render(float angle) {   // show worker (core 0): one frame, straight into the frame buffer
  const int64_t t0 = esp_timer_get_time();
  const float th = angle * (float)M_PI / 180, c = cosf(th), sn = sinf(th);
  const float zoom = 1 + 0.035f * fabsf(sn);
  const int f = c < 0 ? 1 : 0;
  const uint32_t* face = g_df_cm[f];
  const int fw = g_df_fw[f], fh = g_df_fh[f];
  const bool rot1 = g_rot == 1;
  for (int x = 0; x < DF_D; x++) {
    const int sx = DF_X0 + x;
    // screen (sx, sy) -> rotation 1: fb[sx][719 - sy] (walk backwards); rotation 3: fb[1279 - sx][sy]
    uint16_t* fbrow = g_fb + (rot1 ? sx : FB_H - 1 - sx) * FB_W;
    uint16_t* o = rot1 ? fbrow + (FB_W - 1 - DF_Y0) : fbrow + DF_Y0;
    const int step = rot1 ? -1 : 1;
    const uint16_t* under = g_df_under + x * DF_D;
    // which face column this screen column shows, if any
    const float X = x - DF_D / 2 + 0.5f;
    const float den = zoom * DF_F * c - X * sn;
    const uint32_t* col = nullptr;
    int32_t dv = 0;
    int shade = 256;
    if (fabsf(den) > 1e-3f && (den > 0) == (c > 0)) {
      const float u = X * DF_F / den;
      if (fabsf(u) <= DF_R + 1) {
        const float persp = DF_F / (DF_F + u * sn);
        const int su = (int)lroundf((f ? -u : u) + fw / 2.0f);
        if (su >= 0 && su < fw) {
          col = face + su * fh;
          dv = (int32_t)(65536 / (zoom * persp));
          // the face darkens as it turns away; a highlight crosses it mid-turn
          const float k = u / DF_R;
          shade = (int)fminf(320, 256 * (0.42f + 0.58f * fabsf(c) + 0.30f * fabsf(sn) * expf(-k * k * 6)));
        }
      }
    }
    if (!col) {
      for (int y = 0; y < DF_D; y++, o += step) *o = under[y];
      continue;
    }
    for (int y = 0; y < DF_D; y++, o += step) {
      const int sv = (((2 * (y - DF_D / 2) + 1) * dv) >> 17) + fh / 2;
      const uint32_t p = (unsigned)sv < (unsigned)fh ? col[sv] : 0;
      const int a = p >> 24;
      if (!a) { *o = under[y]; continue; }
      int r = min(255, (int)(((p >> 16) & 255) * shade) >> 8), g = min(255, (int)(((p >> 8) & 255) * shade) >> 8),
          b = min(255, (int)((p & 255) * shade) >> 8);
      if (a < 255) {
        const uint16_t u16 = under[y];
        const int ur = (u16 >> 8) & 0xF8, ug = (u16 >> 3) & 0xFC, ub = (u16 << 3) & 0xF8;
        r = ur + (((r - ur) * a) >> 8); g = ug + (((g - ug) * a) >> 8); b = ub + (((b - ub) * a) >> 8);
      }
      *o = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
  }
  // the panel's DMA reads the frame buffer from PSRAM: write the CPU cache back
  uint16_t* first = g_fb + (rot1 ? DF_X0 : FB_H - DF_X0 - DF_D) * FB_W;
  esp_cache_msync(first, (size_t)DF_D * FB_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  g_df_us += esp_timer_get_time() - t0;
  g_df_frames++;
}

static void df_free() {
  for (uint32_t*& cm : g_df_cm) { heap_caps_free(cm); cm = nullptr; }
  heap_caps_free(g_df_under);
  g_df_under = nullptr;
  if (g_df_hands) { lv_draw_buf_destroy(g_df_hands); g_df_hands = nullptr; }
  if (g_df_back) { lv_draw_buf_destroy(g_df_back); g_df_back = nullptr; }
}

static lv_area_t g_df_hands_at;
static volatile bool g_df_ready = false;
static int64_t g_df_prep_us = 0;

// Core 0: everything that is plain pixel work - the front face, the column-major copies, the sky behind the dial.
static HOT void df_prepare(float) {
  const int64_t t0 = esp_timer_get_time();
  const Theme& th = THEMES[g_theme];
  const lv_draw_buf_t* comp = g_comp;   // LVGL leaves it alone: g_show_owns_dial holds composite updates
  lv_draw_buf_t front;                  // the front face, row-major, in a temporary buffer
  uint8_t* fpx = (uint8_t*)heap_caps_malloc(DF_D * DF_D * 4, MALLOC_CAP_SPIRAM);
  uint32_t* sky = (uint32_t*)heap_caps_malloc(DF_D * DF_D * 4, MALLOC_CAP_SPIRAM);
  g_df_under = (uint16_t*)heap_caps_malloc(DF_D * DF_D * 2, MALLOC_CAP_SPIRAM);
  if (!fpx || !sky || !g_df_under) { heap_caps_free(fpx); heap_caps_free(sky); return; }
  lv_draw_buf_init(&front, DF_D, DF_D, LV_COLOR_FORMAT_ARGB8888, DF_D * 4, fpx, DF_D * DF_D * 4);
  // the composite (artwork + hour/minute hands) cut to the face; the anti-aliased rim only near the edge
  for (int y = 0; y < DF_D; y++) {
    const uint16_t* src = (const uint16_t*)((const uint8_t*)comp->data + (DF_Y0 + y) * comp->header.stride) + DF_X0;
    uint8_t* dst = fpx + y * DF_D * 4;
    const float dy = y - DF_D / 2 + 0.5f, dy2 = dy * dy;
    for (int x = 0; x < DF_D; x++) {
      const float dx = x - DF_D / 2 + 0.5f, d2 = dx * dx + dy2;
      const uint16_t v = src[x];
      uint8_t* q = dst + x * 4;
      q[0] = (v << 3) & 0xF8; q[1] = (v >> 3) & 0xFC; q[2] = (v >> 8) & 0xF8;
      q[3] = d2 < (DF_R - 1) * (DF_R - 1) ? 255 : d2 > (DF_R + 1) * (DF_R + 1) ? 0 : (uint8_t)(255 * clamp01(DF_R + 0.5f - sqrtf(d2)));
    }
  }
  if (g_df_hands) {                     // the second hand and hub on top
    const int ox = g_df_hands_at.x1 - DF_X0, oy = g_df_hands_at.y1 - DF_Y0;
    for (int y = 0; y < (int)g_df_hands->header.h; y++) {
      const int fy = y + oy;
      if (fy < 0 || fy >= DF_D) continue;
      const uint8_t* sp = (const uint8_t*)g_df_hands->data + y * g_df_hands->header.stride;
      uint8_t* d = fpx + fy * DF_D * 4;
      for (int x = 0; x < (int)g_df_hands->header.w; x++) {
        const int fx = x + ox;
        if (fx < 0 || fx >= DF_D || !sp[x * 4 + 3] || !d[fx * 4 + 3]) continue;
        const int a = sp[x * 4 + 3];
        for (int ch = 0; ch < 3; ch++) d[fx * 4 + ch] = d[fx * 4 + ch] + (((sp[x * 4 + ch] - d[fx * 4 + ch]) * a) >> 8);
      }
    }
  }
  g_df_cm[0] = df_columns(&front, g_df_fw[0], g_df_fh[0]);
  heap_caps_free(fpx);
  if (g_df_back) g_df_cm[1] = df_columns(g_df_back, g_df_fw[1], g_df_fh[1]);
  // the sky and the dial's shadow inside the face, the composite outside it (column-major)
  paint_region(sky, DF_D, th, DF_X0, DF_Y0, DF_D, DF_D, 1);
  static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int ty = 0; ty < DF_D; ty += 32)
    for (int tx = 0; tx < DF_D; tx += 32)
      for (int y = ty; y < ty + 32; y++) {
        const uint16_t* src = (const uint16_t*)((const uint8_t*)comp->data + (DF_Y0 + y) * comp->header.stride) + DF_X0;
        const float dy = y - DF_D / 2 + 0.5f;
        for (int x = tx; x < tx + 32; x++) {
          const float dx = x - DF_D / 2 + 0.5f;
          uint16_t v = src[x];
          if (dx * dx + dy * dy <= (DF_R + 2.0f) * (DF_R + 2.0f)) {
            const uint32_t p = sky[y * DF_D + x];
            const int t = bayer[(DF_Y0 + y) & 3][(DF_X0 + x) & 3];
            const int r = min(255, (int)((p >> 16) & 0xFF) + ((t * 8 + 4) >> 4)) >> 3;
            const int g = min(255, (int)((p >> 8) & 0xFF) + ((t * 4 + 2) >> 4)) >> 2;
            const int b = min(255, (int)(p & 0xFF) + ((t * 8 + 4) >> 4)) >> 3;
            v = (uint16_t)((r << 11) | (g << 5) | b);
          }
          g_df_under[x * DF_D + y] = v;
        }
      }
  heap_caps_free(sky);
  g_df_prep_us = esp_timer_get_time() - t0;
  g_df_ready = g_df_cm[0] && g_df_cm[1] && g_df_under;
}

static void dialflip_begin() {
  const int64_t t0 = esp_timer_get_time();
  const Theme& th = THEMES[g_theme];
  show_work_wait();
  df_free();
  g_df_ok = g_df_ready = false;
  if (!g_fb) return;
  comp_apply();
  g_show_owns_dial = true;              // from here the composite stays as it is: the worker reads it
  // LVGL work (UI thread): the second hand + hub, and the back face
  if (g_df_hands) lv_draw_buf_destroy(g_df_hands);
  g_df_hands = lv_snapshot_take(dial_obj, LV_COLOR_FORMAT_ARGB8888);
  if (g_df_hands) {
    lv_area_t dc;
    lv_obj_get_coords(dial_obj, &dc);
    g_df_hands_at.x1 = dc.x1 - ((int)g_df_hands->header.w - lv_area_get_width(&dc)) / 2;
    g_df_hands_at.y1 = dc.y1 - ((int)g_df_hands->header.h - lv_area_get_height(&dc)) / 2;
  }
  {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_t* disc = mk(scr, 0, 0, 2 * DF_R, 2 * DF_R);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(disc, lv_color_mix(lv_color_hex(th.acc3), lv_color_hex(th.bg_top), 90), 0);
    lv_obj_set_style_bg_grad_color(disc, lv_color_mix(lv_color_black(), lv_color_hex(th.bg_bot), 140), 0);
    lv_obj_set_style_bg_grad_dir(disc, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(disc, 4, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(th.acc1), 0);
    lv_obj_set_style_border_opa(disc, 170, 0);
    lv_obj_t* ring = mk(disc, 22, 22, 2 * DF_R - 44, 2 * DF_R - 44);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(th.ink), 0);
    lv_obj_set_style_border_opa(ring, 60, 0);
    lv_obj_t* brand = mk_label(disc, f_tiny, th.ink, 150, "TAB5", 0, 120, 2 * DF_R, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(brand, 10, 0);
    mk_label(disc, f_digits, th.ink, LV_OPA_COVER, lv_label_get_text(lbl_time), 0, 175, 2 * DF_R, LV_TEXT_ALIGN_CENTER);
    mk_label(disc, f_date, th.acc1, LV_OPA_COVER, lv_label_get_text(lbl_date), 0, 420, 2 * DF_R, LV_TEXT_ALIGN_CENTER);
    lv_obj_t* nm = mk_label(disc, f_tiny, th.acc2, 200, th.name, 0, 480, 2 * DF_R, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(nm, 6, 0);
    lv_obj_update_layout(scr);
    if (g_df_back) lv_draw_buf_destroy(g_df_back);
    g_df_back = lv_snapshot_take(disc, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_delete(scr);
  }
  g_df_ok = true;
  g_df_wait = 3;
  g_df_t0 = -1;
  g_df_drawn = g_df_pending = 0;
  g_df_us = 0; g_df_frames = 0;
  show_work_kick(df_prepare, 0);
  Serial.printf("dialflip: snapshots in %d ms; preparing on core 0\n", (int)((esp_timer_get_time() - t0) / 1000));
}

static bool show_work_poll() {   // non-blocking: has the worker finished?
  if (!g_work_busy) return true;
  if (xSemaphoreTake(g_work_done, 0) != pdTRUE) return false;
  g_work_busy = false;
  return true;
}
static void df_work(float angle) { df_render(angle); }

static void dialflip_tick(float t) {
  if (!g_df_ok) return;
  if (g_df_t0 < 0) {                                  // still preparing on core 0 (the clock keeps running)
    if (g_df_wait > 0) g_df_wait--;
    if (!show_work_poll() || g_df_wait > 0) return;
    if (!g_df_ready) { g_df_ok = false; return; }
    Serial.printf("dialflip: prepared on core 0 in %d ms\n", (int)(g_df_prep_us / 1000));
    flush_settle();                                   // LVGL's last strip is in the frame buffer
    g_df_t0 = t;
    g_frame_last_us = 0;                              // frame timing starts with the animation
  }
  if (g_work_busy) {
    if (!show_work_poll()) return;                    // still rendering
    g_df_drawn = g_df_pending;
    note_frame();
  }
  const float a = df_angle(g_show_freeze >= 0 ? g_show_freeze : t - g_df_t0);
  if (a == g_df_drawn) { g_frame_last_us = 0; return; }   // holding still: nothing to redraw (not a slow frame)
  g_df_pending = a;
  show_work_kick(df_work, a);
}

static void dialflip_draw(lv_layer_t*) {}

static void dialflip_end() {
  show_work_wait();
  if (g_df_frames)
    Serial.printf("  dialflip worker: %.1f ms per frame (%d frames)\n", g_df_us / 1000.0 / g_df_frames, g_df_frames);
  g_show_owns_dial = false;
  g_dirty_all = true;                                 // re-composite and redraw everything the show kept LVGL from
  const lv_area_t a = {DF_X0, DF_Y0, DF_X0 + DF_D - 1, DF_Y0 + DF_D - 1};
  lv_inv_area(g_disp, &a);
  g_df_ok = false;
  df_free();
}

// ----------------------------------------------------------------------------------------------
// 6. Aurora: a curtain of light ripples across the top of the screen - a wavy lower edge, a glow fading upwards,
// fine vertical rays, coloured from the theme's accents. Like the dial flip it bypasses LVGL: the band's current
// pixels are copied out of the frame buffer at the start, and core 0 adds the light to that copy straight into the
// frame buffer each frame. It stays above the seconds row, so the per-second updates are LVGL's as usual.
// ----------------------------------------------------------------------------------------------
// newlib's sinf/expf go through software double precision on the P4: a few thousand cycles each, far too slow for
// per-column use at 1280 columns a frame. A wrapped 5th-order polynomial is plenty for smooth curtains.
static inline float fsin(float x) {
  x *= 0.15915494f;                               // turns
  x -= floorf(x + 0.5f);                          // -0.5 .. 0.5
  const float y = x * 6.2831853f;
  float z = y;
  if (y > 1.5707963f) z = 3.1415927f - y; else if (y < -1.5707963f) z = -3.1415927f - y;
  const float z2 = z * z;
  return z * (1 - z2 * (0.16666667f - z2 * (0.0083333f - z2 * 0.000198f)));
}

static constexpr int AU_H = 300;                       // band height, from the top of the screen
static constexpr float AU_T = 10.0f;
static uint16_t* g_au_base = nullptr;                  // the band as it was, column-major [x * AU_H + y]
static uint8_t g_au_ramp[AU_H][3];                     // colour by distance above the curtain's edge: acc1 -> acc3
static bool g_au_ok = false;
static float g_au_t0 = -1;
static int g_au_wait = 0;
static int64_t g_au_us = 0, g_au_sync_us = 0;
static int g_au_frames = 0;

static HOT void au_render(float t) {   // show worker (core 0)
  const int64_t t0 = esp_timer_get_time();
  const Theme& th = THEMES[g_theme];
  const float env = ease(t / 1.5f) * (1 - ease((t - (AU_T - 2.0f)) / 1.8f));
  const bool rot1 = g_rot == 1;
  for (int x = 0; x < SCR_W; x++) {
    const float fx = x * (1.0f / SCR_W);
    // the curtain's lower edge drifts and folds; its brightness ripples along it
    const float yb = 190 + 46 * fsin(fx * 7.1f + t * 0.55f) + 22 * fsin(fx * 17.3f - t * 0.9f) + 9 * fsin(fx * 41.0f + t * 1.7f);
    float I = 0.55f + 0.30f * fsin(fx * 11.0f - t * 0.8f) + 0.15f * fsin(fx * 29.0f + t * 1.3f);
    I *= 0.78f + 0.22f * fsin(x * 0.37f + t * 2.1f) * fsin(x * 0.11f - t * 0.7f);   // fine rays
    I = fmaxf(0, I) * env;
    const float L = 70 + 30 * fsin(fx * 5.0f + t * 0.4f);                          // how far the glow reaches up
    uint16_t* fbrow = g_fb + (rot1 ? x : FB_H - 1 - x) * FB_W;
    uint16_t* o = rot1 ? fbrow + (FB_W - 1) : fbrow;   // screen y = 0
    const int step = rot1 ? -1 : 1;
    const uint16_t* base = g_au_base + x * AU_H;
    const int ybi = (int)yb;
    (void)th;
    const int32_t k_up = (int32_t)(65536 * (1 - 1 / L + 0.5f / (L * L)));          // ~exp(-1/L), 16.16
    const int edge = min(AU_H - 1, ybi + 12);
    for (int y = AU_H - 1; y > edge; y--) o[y * step] = base[y];
    // below the edge: a soft falloff (few rows, float is fine); above it: the glow decays by k_up per row
    int32_t w = (int32_t)(I * 220 * 65536);                                        // 16.16, 220 = full strength
    for (int y = edge; y >= 0; y--) {
      int wi;
      if (y > ybi) { const float d = (y - yb) / 6.0f; wi = (int)(I * 220 * gauss(2 * d * d)); }
      else { wi = w >> 16; w = (int32_t)(((int64_t)w * k_up) >> 16); }
      const uint16_t v = base[y];
      if (wi <= 0) { o[y * step] = v; continue; }
      const uint8_t* c = g_au_ramp[min(AU_H - 1, max(0, ybi - y))];
      const int r = min(255, ((v >> 8) & 0xF8) + ((c[0] * wi) >> 8));
      const int g = min(255, ((v >> 3) & 0xFC) + ((c[1] * wi) >> 8));
      const int b = min(255, ((v << 3) & 0xF8) + ((c[2] * wi) >> 8));
      o[y * step] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
  }
  const int64_t t1 = esp_timer_get_time();
  uint16_t* first = g_fb + (rot1 ? 0 : FB_H - SCR_W) * FB_W;
  esp_cache_msync(first, (size_t)SCR_W * FB_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  g_au_sync_us += esp_timer_get_time() - t1;
  g_au_us += esp_timer_get_time() - t0;
  g_au_frames++;
}

static void aurora_begin() {
  g_au_ok = false;
  if (!g_fb) return;
  if (!g_au_base) g_au_base = (uint16_t*)heap_caps_malloc(SCR_W * AU_H * 2, MALLOC_CAP_SPIRAM);
  if (!g_au_base) return;
  const Theme& th = THEMES[g_theme];
  for (int d = 0; d < AU_H; d++) {
    const int m = min(256, d * 2);
    for (int ch = 0; ch < 3; ch++) {
      const int a = (th.acc1 >> (16 - 8 * ch)) & 255, b = (th.acc3 >> (16 - 8 * ch)) & 255;
      g_au_ramp[d][ch] = (uint8_t)(a + (((b - a) * m) >> 8));
    }
  }
  g_au_ok = true;
  g_au_t0 = -1;
  g_au_wait = 3;          // let LVGL bring the screen up to date before the band is copied
  g_au_us = g_au_sync_us = 0; g_au_frames = 0;
}

static void aurora_tick(float t) {
  if (!g_au_ok) return;
  if (g_au_t0 < 0 && g_au_wait > 0) { g_au_wait--; return; }
  if (g_au_t0 < 0) {
    // copy the band out of the frame buffer (the panel is portrait: a screen column is a frame-buffer row),
    // after LVGL has drawn everything still pending
    lv_refr_now(g_disp);
    flush_settle();
    esp_cache_msync(g_fb, (size_t)FB_W * FB_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const bool rot1 = g_rot == 1;
    for (int x = 0; x < SCR_W; x++) {
      const uint16_t* fbrow = g_fb + (rot1 ? x : FB_H - 1 - x) * FB_W;
      for (int y = 0; y < AU_H; y++) g_au_base[x * AU_H + y] = rot1 ? fbrow[FB_W - 1 - y] : fbrow[y];
    }
    g_show_owns_dial = true;      // hold composite updates (the dial's top is in the band)
    g_au_t0 = t;
    g_frame_last_us = 0;
  }
  if (g_work_busy) {
    if (!show_work_poll()) return;
    note_frame();
  }
  show_work_kick(au_render, g_show_freeze >= 0 ? g_show_freeze : t - g_au_t0);
}

static void aurora_draw(lv_layer_t*) {}

static void aurora_end() {
  show_work_wait();
  if (g_au_frames)
    Serial.printf("  aurora worker: %.1f ms per frame, of which cache write-back %.1f ms (%d frames)\n",
                  g_au_us / 1000.0 / g_au_frames, g_au_sync_us / 1000.0 / g_au_frames, g_au_frames);
  g_show_owns_dial = false;
  g_dirty_all = true;
  heap_caps_free(g_au_base);
  g_au_base = nullptr;
  g_au_ok = false;
}

static const ShowDef SHOWS[] = {
  {"bench", 6000, nullptr, bench_tick, nullptr, nullptr},
  {"sweep", 4100, sweep_begin, sweep_tick, sweep_draw, nullptr},
  {"fireworks", 12500, fireworks_begin, fireworks_tick, fireworks_draw, fireworks_end},
  {"numerals", 9000, numerals_begin, numerals_tick, numerals_draw, numerals_end},
  {"flap", (int)(FLAP_T * 1000), flap_begin, flap_tick, flap_draw, flap_end},
  {"dialflip", (int)(DF_ANIM * 1000) + 1800, dialflip_begin, dialflip_tick, dialflip_draw, dialflip_end},
  {"aurora", (int)(AU_T * 1000) + 300, aurora_begin, aurora_tick, aurora_draw, aurora_end},
};
static constexpr int N_SHOWS = sizeof(SHOWS) / sizeof(SHOWS[0]);
static constexpr int FIRST_REAL_SHOW = 1;   // index 0 is the benchmark, never picked by the hourly schedule

static void show_draw_cb(lv_event_t* e) {
  if (g_show >= 0 && SHOWS[g_show].draw) SHOWS[g_show].draw(lv_event_get_layer(e));
}

static void shows_build() {
  show_obj = mk(lv_screen_active(), 0, 0, SCR_W, SCR_H);   // always "visible": un-hiding would redraw the screen
  lv_obj_add_event_cb(show_obj, show_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
}

static void show_stop() {
  if (g_show < 0) return;
  const ShowDef& s = SHOWS[g_show];
  const int64_t now = esp_timer_get_time();
  const float secs = (now - g_show_t0) / 1e6f;
  const uint32_t frames = g_frames - g_show_frames0;
  Serial.printf("show %s: %u frames in %.1f s = %.1f fps (%.1f while animating), worst %d ms, %u slower than 100 ms, loop busy %.0f%%\n",
                s.name, (unsigned)frames, secs, frames / secs, g_frame_active_us ? (frames - 1) * 1e6f / g_frame_active_us : 0.0f,
                (int)(g_frame_worst_us / 1000), (unsigned)g_frame_slow, 100.0 * (g_busy_us - g_show_busy0) / (now - g_show_t0));
  Serial.printf("  tick: avg %.1f ms, max %.1f ms over %u ticks; flush avg %u us/frame-strip\n",
                g_show_ticks ? g_show_tick_us / 1000.0 / g_show_ticks : 0.0, g_show_tick_max / 1000.0, (unsigned)g_show_ticks,
                g_flush_n ? (unsigned)(g_flush_us / g_flush_n) : 0u);
  if (g_frame_slow) {
    Serial.print("  slow frames (at ms: duration):");
    for (uint32_t i = 0; i < g_frame_slow && i < 12; i++) Serial.printf(" %u:%u", g_slow_at_ms[i], g_slow_dt_ms[i]);
    Serial.println();
  }
  if (s.end) s.end();
  g_show = -1;
  g_show_running = false;
  g_show_freeze = -1;
  g_frame_track = false;
  lv_timer_set_period(g_fast_timer, FRAME_MS);
  lv_obj_invalidate(lv_screen_active());
}

static void show_start(int idx) {
  if (idx < 0 || idx >= N_SHOWS || !g_disp_on || g_wifi_open) return;
  show_stop();
  if (g_batt_open) batt_close();
  if (SHOWS[idx].begin) SHOWS[idx].begin();   // before the show is visible to the draw callback
  g_show = idx;
  g_show_running = true;
  g_show_t0 = esp_timer_get_time();
  g_show_frames0 = g_frames;
  g_show_busy0 = g_busy_us;
  g_frame_worst_us = g_frame_slow = 0;
  g_frame_active_us = 0;
  g_show_tick_us = g_show_tick_max = 0; g_show_ticks = 0;
  g_frame_last_us = 0;
  g_frame_track_t0 = esp_timer_get_time();
  g_frame_track = true;
  lv_timer_set_period(g_fast_timer, SHOW_FRAME_MS);
  Serial.printf("show %s started\n", SHOWS[idx].name);
}

// Called from fast_cb every frame, before the composite is applied.
static void shows_update(const Now& n) {
  if (g_show >= 0) {
    if (!g_disp_on || g_wifi_open || g_batt_open) { show_stop(); return; }
    const float t = g_show_freeze >= 0 ? g_show_freeze : (esp_timer_get_time() - g_show_t0) / 1e6f;
    if (g_show_freeze < 0 && t * 1000 >= SHOWS[g_show].duration_ms) { show_stop(); return; }
    const int64_t t0 = esp_timer_get_time();
    SHOWS[g_show].tick(t);
    const int64_t d = esp_timer_get_time() - t0;
    g_show_tick_us += d; g_show_ticks++;
    if (d > g_show_tick_max) g_show_tick_max = d;
    return;
  }
  if (!g_shows_enabled || n.mi != 0 || n.s > 1 || n.h == g_show_last_hour) return;
  if (n.h < SHOW_FIRST_HOUR || n.h > SHOW_LAST_HOUR || N_SHOWS <= FIRST_REAL_SHOW) return;
  g_show_last_hour = n.h;
  if (g_batt_open || g_wifi_open) return;   // don't pull a card away from someone reading it: skip this hour
  show_start(FIRST_REAL_SHOW + (int)(esp_random() % (N_SHOWS - FIRST_REAL_SHOW)));
}
