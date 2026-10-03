// Fancy Clock for the M5Stack Tab5 (ESP32-P4, 1280x720 MIPI-DSI)
//
//  * LVGL 9 does all the drawing, M5GFX/M5Unified provide the display, touch, RTC and battery.
//  * The heavy, static artwork (gradients, glows, bokeh, guilloche dial, ticks, numerals, frosted calendar card) is
//    rendered once per theme into an image.  Per frame (~15 fps) only small rectangles hugging the moving hands are
//    redrawn; hands, hub and glows are drawn by a custom draw callback using pre-computed ARGB sprites instead of
//    LVGL's (expensive) blurred box shadows.
//  * The rotated copy into the panel's frame buffer is done by the P4's PPA (pixel processing accelerator), not the CPU.
//  * Between frames loop() sleeps until the next LVGL timer is due (touch is polled every 25 ms).
//  * Tap the clock face / background to cycle themes, tap the big digits to switch 12h/24h.
//  * Serial (115200, USB-C port): "T YYYY-MM-DD HH:MM:SS" sets the RTC, "P" sends a screenshot,
//    "C" next theme, "M" toggle 12/24h, "S" status.  See tools/*.py.

#include <M5Unified.h>
#include <lvgl.h>
#include <misc/cache/instance/lv_image_cache.h>
#include <core/lv_refr_private.h>                     // lv_inv_area(): invalidate raw screen areas
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>    // frame buffer address
#include <driver/ppa.h>
#include <esp_cache.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <atomic>
#include <freertos/queue.h>
#include <esp_log.h>
#include "fonts.h"
#include "types.h"
#include "dirty_regions.h"
#include "battery_state.h"
#include <esp_system.h>

// ----------------------------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------------------------
static constexpr int      ROTATION   = 1;    // start-up orientation if the IMU can't tell: 1 = landscape, 3 = upside-down
static constexpr int      ROT_HOLD_MS = 1200; // auto-rotate: a new orientation must be held this long before the screen turns
static constexpr uint8_t  BRIGHTNESS = 200;  // 0..255
static constexpr int      FRAME_MS   = 66;   // clock animation period (~15 fps)
static constexpr int      TOUCH_MS   = 25;   // touch polling period while the display is on

static constexpr int SCR_W = 1280, SCR_H = 720;
static constexpr int CX = 340, CY = 360;     // dial centre
static constexpr int PX = 724;               // left edge of the right-hand panel
static constexpr int PW = 516;               // width of the right-hand panel

// Saturated but not neon: deep, tinted backgrounds (never pure black), accents with high chroma and medium-high
// lightness, and a slightly tinted off-white "ink" instead of pure white for text and hands.
static const Theme THEMES[] = {
  //  name       bg_top    bg_bot    acc1      acc2      acc3      ink
  {"AURORA",   0x06182C, 0x1C0F40, 0x3DE3B9, 0xFF6FAE, 0x8F7BFF, 0xEAF3F4},
  {"SUNSET",   0x1E0A22, 0x4C1631, 0xFF8F5C, 0xFFD166, 0xFF5E8A, 0xFFF1E6},
  {"OCEAN",    0x031526, 0x07405F, 0x40CFF4, 0xFFB26B, 0x6F8CFF, 0xE8F3FF},
  {"JADE",     0x05201A, 0x0F3D2E, 0x7BE495, 0xFFC857, 0x3CC3CC, 0xEEF7EE},
  {"GRAPHITE", 0x0C0E13, 0x20242E, 0x5AC8FA, 0xFF5A4F, 0xFFB340, 0xF1F3F7},
};
static constexpr int N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);

static const char* const MONTHS[]   = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
static const char* const WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// ----------------------------------------------------------------------------------------------
// Calendar maths (proleptic Gregorian, days since 1970-01-01)
// ----------------------------------------------------------------------------------------------
static bool g_dirty_all = true;
static Preferences prefs;
#include "timekeeping.h"
#include "network.h"

// ----------------------------------------------------------------------------------------------
// LVGL <-> M5GFX glue
// ----------------------------------------------------------------------------------------------
static lv_display_t* g_disp = nullptr;
static uint32_t g_flush_us = 0, g_flush_px = 0, g_flush_n = 0, g_frames = 0;
static int64_t  g_busy_us = 0, g_stat_t0 = 0;   // time loop() spent working (not sleeping) since the last "S"

// The panel is natively 720x1280 portrait and M5GFX keeps its frame buffer in PSRAM.  Rotating LVGL's landscape
// strips into it pixel-by-pixel on the CPU (M5GFX pushImage) cost ~80 ns/pixel; the PPA's scale-rotate-mirror
// engine does the same by DMA while the CPU sleeps on a semaphore.  pushImage remains as a fallback.
static constexpr int FB_W = 720, FB_H = 1280;
static ppa_client_handle_t g_ppa = nullptr;
static uint16_t* g_fb = nullptr;
static int g_rot = ROTATION;   // current display rotation (1 or 3), changed by auto-rotation

static void ppa_setup() {
  auto* panel = static_cast<lgfx::Panel_DSI*>(M5.Display.getPanel());
  g_fb = panel ? (uint16_t*)panel->config_detail().buffer : nullptr;
  ppa_client_config_t cfg = {};
  cfg.oper_type = PPA_OPERATION_SRM;
  cfg.max_pending_trans_num = 1;
  if (!g_fb || ppa_register_client(&cfg, &g_ppa) != ESP_OK) { g_ppa = nullptr; Serial.println("PPA unavailable, using pushImage"); }
}

static bool ppa_flush(const lv_area_t* a, const uint8_t* px, int w, int h) {
  ppa_srm_oper_config_t op = {};
  op.in.buffer = px;
  op.in.pic_w = op.in.block_w = w;
  op.in.pic_h = op.in.block_h = h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = g_fb;
  op.out.buffer_size = FB_W * FB_H * 2;
  op.out.pic_w = FB_W;
  op.out.pic_h = FB_H;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  if (g_rot == 1) {           // logical (x, y) -> panel (719 - y, x): 90 deg clockwise = 270 counter-clockwise
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    op.out.block_offset_x = FB_W - 1 - a->y2;
    op.out.block_offset_y = a->x1;
  } else {                    // logical (x, y) -> panel (y, 1279 - x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    op.out.block_offset_x = a->y1;
    op.out.block_offset_y = FB_H - 1 - a->x2;
  }
  op.scale_x = op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(g_ppa, &op) == ESP_OK;
}

static void flush_cb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
  const int w = lv_area_get_width(a), h = lv_area_get_height(a);
  const int64_t t0 = esp_timer_get_time();
  if (!g_ppa || !ppa_flush(a, px, w, h)) {
    M5.Display.startWrite();
    M5.Display.pushImage(a->x1, a->y1, w, h, (const lgfx::rgb565_t*)px);
    M5.Display.endWrite();
  }
  g_flush_us += (uint32_t)(esp_timer_get_time() - t0);
  g_flush_px += (uint32_t)(w * h);
  g_flush_n++;
  if (lv_display_flush_is_last(disp)) g_frames++;
  lv_display_flush_ready(disp);
}

static bool g_screenshot_active = false;
static void log_cb(lv_log_level_t, const char* msg) { if (!g_screenshot_active) Serial.print(msg); }

// The ESP-Hosted driver for the WiFi chip needs a big chunk of *internal* RAM for its SDIO buffer pool,
// so the LVGL draw buffers live in PSRAM (internal RAM is the scarce resource here).
static void* alloc_draw_buf(size_t bytes) {
  void* p = heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);  // cache-line aligned for the PPA
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

// ----------------------------------------------------------------------------------------------
// UI state
// ----------------------------------------------------------------------------------------------
static int  g_theme = 0;
static bool g_24h = true;

static lv_font_t *f_digits, *f_sec, *f_date, *f_head, *f_num, *f_cal, *f_small, *f_tiny;

// Objects that need re-colouring when the theme changes
static lv_obj_t *bg_img, *dial_obj, *secbar_obj, *lbl_greet, *lbl_time, *lbl_ampm, *lbl_sec, *lbl_date;
static lv_obj_t *lbl_wifi, *lbl_batt, *lbl_month, *today_mark, *week_mark;
static lv_obj_t *lbl_wd[7], *lbl_day[42];

static lv_draw_buf_t* g_static_bufs[N_THEMES];  // one pre-rendered background per theme
static int g_render_theme = 0;
static lv_grad_dsc_t g_grad_card, g_grad_sheen, g_grad_bar;

// Geometry of the moving parts
static constexpr float R_RING = 327;          // day-progress ring centre line (6 px wide)
// lv_draw_arc covers center-r .. center+r-1, i.e. the arc's true centre is half a pixel up-left of (CX, CY);
// everything belonging to the day ring (track, dots, knob) is centred there so nothing peeks out from under it.
static constexpr float RCX = CX - 0.5f, RCY = CY - 0.5f;
static constexpr float R_TIP  = 246;          // centre of the second hand's lens tip
static constexpr float SEG = 36;              // length of the dirty-rectangle pieces along a hand
static constexpr int BAR_X = PX + 4, BAR_Y = 334, BAR_W = PW - 110, BAR_H = 8;

static const HandSpec HS_H = {-26, 150, 16, 44, 138, 6};
static const HandSpec HS_M = {-26, 226, 11, 44, 212, 4};
static constexpr float SH_DX = 5, SH_DY = 8;                    // hour/minute hand shadow offset
static constexpr float S_R0 = -60, S_TAIL = -20, S_R1 = 237;    // second hand: counterweight, then the thin body
static constexpr float SS_DX = 4, SS_DY = 7;                    // second hand shadow offset

static float g_ah = 0, g_am = 0, g_as = 0;   // hand angles currently on screen (degrees)
static int   g_day_m = 0;                    // minutes since midnight shown by the day ring
static float g_bar = 0;                      // seconds bar fill (0..1)
static bool  g_show_ring = true, g_show_shadows = true, g_soft_shadows = true;

// Composite: static artwork + day ring + hour/minute hands.  Those only move every 3 s (minute), 30 s (hour) or
// 60 s (ring), so they are drawn into this image when they move; each frame then only has to blit it and draw the
// second hand, its tip and the hub on top.
static lv_draw_buf_t* g_comp = nullptr;
static lv_obj_t* comp_canvas = nullptr;       // hidden canvas, only used to get a draw layer on g_comp
static constexpr int MAX_COMP_BOX = 24;
static lv_area_t g_comp_box[MAX_COMP_BOX];
static int  g_comp_n = 0;
static bool g_comp_full = true;
static bool g_merge_regions = true;

// ----------------------------------------------------------------------------------------------
// Small widget / drawing helpers
// ----------------------------------------------------------------------------------------------
static lv_obj_t* mk(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  return o;
}

static lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, lv_opa_t opa,
                          const char* text, int x, int y, int w = 0, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_opa(l, opa, 0);
  lv_obj_set_style_text_align(l, align, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_label_set_text(l, text);
  if (w) lv_obj_set_width(l, w);
  lv_obj_set_pos(l, x, y);
  return l;
}

static void set_grad(lv_grad_dsc_t* g, int n, const uint32_t* c, const lv_opa_t* o, lv_grad_dir_t dir) {
  lv_color_t cols[3];
  uint8_t fr[3];
  for (int i = 0; i < n; i++) { cols[i] = lv_color_hex(c[i]); fr[i] = (uint8_t)(255 * i / (n - 1)); }
  lv_grad_init_stops(g, cols, o, fr, n);
  if (dir == LV_GRAD_DIR_VER) lv_grad_vertical_init(g); else lv_grad_horizontal_init(g);
}

static inline bool clip_hit(const lv_layer_t* L, float x1, float y1, float x2, float y2) {
  const lv_area_t& c = L->_clip_area;
  return x2 >= c.x1 && x1 <= c.x2 && y2 >= c.y1 && y1 <= c.y2;
}

static void d_line(lv_layer_t* L, float x1, float y1, float x2, float y2, int w, uint32_t col, lv_opa_t opa) {
  const float p = w / 2.0f + 2;
  if (!clip_hit(L, fminf(x1, x2) - p, fminf(y1, y2) - p, fmaxf(x1, x2) + p, fmaxf(y1, y2) + p)) return;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
  d.width = w;
  d.color = lv_color_hex(col);
  d.opa = opa;
  d.round_start = d.round_end = 1;
  lv_draw_line(L, &d);
}

// A radial line of the dial: angle in degrees clockwise from 12 o'clock, radii from the dial centre,
// optionally shifted sideways by `off` pixels.
static void d_polar(lv_layer_t* L, float deg, float r0, float r1, int w, uint32_t col, lv_opa_t opa, float off = 0) {
  const float a = deg * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
  const float ox = c * off, oy = s * off;
  d_line(L, CX + s * r0 + ox, CY - c * r0 + oy, CX + s * r1 + ox, CY - c * r1 + oy, w, col, opa);
}

// Circle outline (w > 0) or arc; LVGL arc angles: 0 = 3 o'clock, clockwise; `r` is the outer radius.
static void d_arc(lv_layer_t* L, int cx, int cy, int r, int w, float a0, float a1, uint32_t col, lv_opa_t opa, bool round = false) {
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.center.x = cx; d.center.y = cy;
  d.radius = r;
  d.width = w;
  d.start_angle = a0; d.end_angle = a1;
  d.color = lv_color_hex(col);
  d.opa = opa;
  d.rounded = round;
  lv_draw_arc(L, &d);
}

static void d_disc(lv_layer_t* L, float cx, float cy, float r, uint32_t col, lv_opa_t opa, lv_opa_t rim = 0) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = lv_color_hex(col);
  d.bg_opa = opa;
  if (rim) { d.border_width = 1; d.border_color = d.bg_color; d.border_opa = rim; }
  const lv_area_t a = {(int32_t)lroundf(cx - r), (int32_t)lroundf(cy - r), (int32_t)lroundf(cx + r) - 1, (int32_t)lroundf(cy + r) - 1};
  lv_draw_rect(L, &d, &a);
}

static void d_sprite(lv_layer_t* L, const lv_draw_buf_t* spr, float cx, float cy) {
  const int sz = spr->header.w;
  lv_area_t a;
  a.x1 = (int32_t)lroundf(cx - (sz - 1) / 2.0f);
  a.y1 = (int32_t)lroundf(cy - (sz - 1) / 2.0f);
  a.x2 = a.x1 + sz - 1;
  a.y2 = a.y1 + sz - 1;
  if (!clip_hit(L, a.x1, a.y1, a.x2, a.y2)) return;
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = spr;
  lv_draw_image(L, &d, &a);
}

// ----------------------------------------------------------------------------------------------
// Sprites: small antialiased ARGB8888 images (lens tip, hub, glowing knobs) computed per theme.
// Blending one of these is far cheaper than LVGL's blurred box shadows, which were recomputed every frame.
// ----------------------------------------------------------------------------------------------
static lv_draw_buf_t *spr_tip, *spr_hub, *spr_knob, *spr_head;

#define HOT __attribute__((optimize("O2")))   // for the per-pixel loops (the sketch is otherwise built with -Os)
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline float smooth(float e0, float e1, float v) { const float t = clamp01((v - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
static inline float disc_cov(float d, float r) { return clamp01(r - d + 0.5f); }                         // AA disc
static inline float ring_cov(float d, float r0, float r1) { return disc_cov(d, r1) * clamp01(d - r0 + 0.5f); }
static Rgba rgba(uint32_t c, float a) { return {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, clamp01(a)}; }

static void over(Rgba& d, const Rgba& s) {  // Porter-Duff "source over", straight alpha
  const float a = s.a + d.a * (1 - s.a);
  if (a <= 0.0f) return;
  const float k = d.a * (1 - s.a);
  d.r = (s.r * s.a + d.r * k) / a;
  d.g = (s.g * s.a + d.g * k) / a;
  d.b = (s.b * s.a + d.b * k) / a;
  d.a = a;
}

static lv_draw_buf_t* sprite_begin(lv_draw_buf_t*& b, int size) {
  if (!b) b = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_ARGB8888, 0);
  else lv_image_cache_drop(b);
  return b;
}

static void sprite_put(lv_draw_buf_t* b, int x, int y, const Rgba& p) {
  uint8_t* q = (uint8_t*)b->data + y * b->header.stride + x * 4;   // LVGL ARGB8888 byte order: B G R A
  q[0] = (uint8_t)(p.b * 255 + 0.5f);
  q[1] = (uint8_t)(p.g * 255 + 0.5f);
  q[2] = (uint8_t)(p.r * 255 + 0.5f);
  q[3] = (uint8_t)(p.a * 255 + 0.5f);
}

static HOT void make_sprites(const Theme& th) {
  // Second hand tip: a translucent coloured lens with a bright rim, a soft halo and a small specular highlight
  lv_draw_buf_t* b = sprite_begin(spr_tip, 64);
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      const float dx = x - 31.5f, dy = y - 31.5f, d = sqrtf(dx * dx + dy * dy);
      const float hx = dx + 3.2f, hy = dy + 3.6f;
      Rgba p = {0, 0, 0, 0};
      over(p, rgba(th.acc2, 0.40f * expf(-d * d / (2 * 8.5f * 8.5f))));
      over(p, rgba(th.acc2, 0.26f * disc_cov(d, 9.5f)));
      over(p, rgba(th.acc2, ring_cov(d, 7.2f, 10.0f)));
      over(p, rgba(th.ink, 0.95f * disc_cov(d, 2.4f)));
      over(p, rgba(0xFFFFFF, 0.50f * expf(-(hx * hx + hy * hy) / 3.0f) * disc_cov(d, 9.5f)));
      sprite_put(b, x, y, p);
    }
  // Hub: soft drop shadow, a gently shaded dome with a machined groove, and a coloured jewel
  b = sprite_begin(spr_hub, 72);
  for (int y = 0; y < 72; y++)
    for (int x = 0; x < 72; x++) {
      const float dx = x - 35.5f, dy = y - 35.5f, d = sqrtf(dx * dx + dy * dy);
      const float sx = dx - 2, sy = dy - 6, sd = sqrtf(sx * sx + sy * sy);
      const float jx = dx + 2.2f, jy = dy + 2.4f;
      Rgba p = {0, 0, 0, 0};
      over(p, rgba(0x000000, 0.50f * (1 - smooth(12, 27, sd))));
      Rgba dome = rgba(th.ink, disc_cov(d, 18));
      const float lit = 0.80f + 0.20f * clamp01(0.5f - dy / 36.0f);
      dome.r *= lit; dome.g *= lit; dome.b *= lit;
      over(p, dome);
      over(p, rgba(0x000000, 0.22f * ring_cov(d, 16.6f, 18.0f)));
      over(p, rgba(0x000000, 0.16f * ring_cov(d, 11.0f, 12.0f)));
      over(p, rgba(th.acc2, disc_cov(d, 7.0f)));
      over(p, rgba(0xFFFFFF, 0.60f * expf(-(jx * jx + jy * jy) / 2.5f)));
      sprite_put(b, x, y, p);
    }
  // Day ring knob and seconds-bar head: glowing beads
  b = sprite_begin(spr_knob, 44);
  for (int y = 0; y < 44; y++)
    for (int x = 0; x < 44; x++) {
      const float dx = x - 21.5f, dy = y - 21.5f, d = sqrtf(dx * dx + dy * dy);
      Rgba p = {0, 0, 0, 0};
      over(p, rgba(th.acc1, 0.55f * expf(-d * d / (2 * 6.5f * 6.5f))));
      over(p, rgba(th.acc1, disc_cov(d, 6.5f)));
      over(p, rgba(th.ink, disc_cov(d, 2.6f)));
      sprite_put(b, x, y, p);
    }
  b = sprite_begin(spr_head, 36);
  for (int y = 0; y < 36; y++)
    for (int x = 0; x < 36; x++) {
      const float dx = x - 17.5f, dy = y - 17.5f, d = sqrtf(dx * dx + dy * dy);
      Rgba p = {0, 0, 0, 0};
      over(p, rgba(th.acc2, 0.55f * expf(-d * d / (2 * 5.0f * 5.0f))));
      over(p, rgba(th.acc2, disc_cov(d, 5.5f)));
      over(p, rgba(th.ink, disc_cov(d, 2.0f)));
      sprite_put(b, x, y, p);
    }
}

// ----------------------------------------------------------------------------------------------
// Static artwork (rendered once per theme into an image)
// ----------------------------------------------------------------------------------------------
// The soft background and the radial parts of the dial are painted per pixel straight into a 32-bit buffer, in
// float, before LVGL draws the crisp details on top:
//  * gradient, coloured glows, aurora-like ribbons with fine "curtain" striations and a vignette - all smooth, so they
//    are evaluated on a 1/4-resolution grid and bilinearly upsampled;
//  * the dial at full resolution: drop shadow, smoked-glass face with a reflection, guilloche rings, sunburst rays,
//    minute-track and bezel rings, and the day-ring track.
static float g_exp_lut[1024];                  // exp(-t/2), t = i/64
static inline float gauss(float t) { const int i = (int)(t * 64.0f); return i >= 0 && i < 1024 ? g_exp_lut[i] : 0.0f; }  // t = (d/sigma)^2
static inline float ring_at(float d, float rc, float w) { return clamp01(w * 0.5f + 0.5f - fabsf(d - rc)); }  // AA ring coverage
static inline void mix_to(float* c, const float* to, float a) { c[0] += (to[0] - c[0]) * a; c[1] += (to[1] - c[1]) * a; c[2] += (to[2] - c[2]) * a; }
static inline void screen_add(float* c, const float* col, float a) { for (int i = 0; i < 3; i++) c[i] += col[i] * a * (1 - c[i]); }


// NOTE: newlib's float trig (sinf, atan2f, ...) goes through software double precision on the P4 (single-precision
// FPU only) and costs thousands of cycles, so the per-pixel code avoids it: ribbon shapes are tabulated per column
// and atan2 is a polynomial.
static inline float fast_atan2f(float y, float x) {   // |error| < 1e-5 rad
  const float ax = fabsf(x), ay = fabsf(y), mx = fmaxf(ax, ay);
  if (mx == 0) return 0;
  const float a = fminf(ax, ay) / mx, s = a * a;
  float r = a * (0.99997726f + s * (-0.33262347f + s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f + s * -0.01172120f)))));
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  return y < 0 ? -r : r;
}

static HOT void sky_at(const Theme& th, const Glow* glows, const Ribbon* ribs, const RibbonCol* rc2, float x, float y, float* c) {
  const Rgba top = rgba(th.bg_top, 1), bot = rgba(th.bg_bot, 1);
  const float t = clamp01(y / (SCR_H - 1));
  c[0] = top.r + (bot.r - top.r) * t; c[1] = top.g + (bot.g - top.g) * t; c[2] = top.b + (bot.b - top.b) * t;
  for (int k = 0; k < 3; k++) {         // glows and ribbons use "screen" blending: they add light, keep saturation
    const Glow& g = glows[k];
    const float u = (x - g.x) / g.sx, v = (y - g.y) / g.sy;
    const Rgba gc = rgba(g.c, 1);
    const float col[3] = {gc.r, gc.g, gc.b};
    screen_add(c, col, g.amp * gauss(u * u + v * v));
  }
  for (int k = 0; k < 2; k++) {
    const RibbonCol& r = rc2[k];
    const float dy = y - r.yc, w = r.w * (dy < 0 ? 0.45f : 1.6f);   // sharp upper edge, long fade downwards
    const Rgba rc = rgba(ribs[k].c, 1);
    const float col[3] = {rc.r, rc.g, rc.b};
    screen_add(c, col, r.amp * gauss(dy * dy / (w * w)));
  }
  const float vx = (x - SCR_W / 2) / (SCR_W * 0.60f), vy = (y - SCR_H / 2) / (SCR_H * 0.60f);
  const float vig = 1.0f - 0.50f * clamp01((vx * vx + vy * vy - 0.25f) / 1.0f);
  c[0] *= vig; c[1] *= vig; c[2] *= vig;
}

// smoothstep with a precomputed 1/(e1-e0): the sketch is built with -Os, so divisions are real divisions
static inline float smooth_r(float e0, float inv, float v) { const float t = clamp01((v - e0) * inv); return t * t * (3 - 2 * t); }

static HOT void dial_at(const DialCols& dc, float x, float y, float* c) {
  const float dx = x - CX, dy = y - CY, d2 = dx * dx + dy * dy;
  if (d2 > 345.0f * 345.0f) return;
  const float d = sqrtf(d2);
  const float *ink = dc.ink, *acc3 = dc.acc3, black[3] = {0, 0, 0};

  const float sy = dy - 20, ds2 = dx * dx + sy * sy;                   // drop shadow, offset downwards
  if (ds2 < 350.0f * 350.0f) mix_to(c, black, ds2 < 262.0f * 262.0f ? 0.55f : 0.55f * (1 - smooth_r(262, 1 / 88.0f, sqrtf(ds2))));
  if (d < 301) {
    const float cov = clamp01(300.5f - d), t = d * (1 / 300.0f);
    mix_to(c, black, (0.30f + 0.36f * t * t) * cov);                 // smoked glass, darker towards the rim
    screen_add(c, acc3, 0.10f * (1 - t) * (1 - t) * cov);             // faint coloured light in the centre
    if (dy < -18) {                                                   // reflection across the top
      const float ex = dx * (1 / 255.0f), ey = (dy + 150) * (1 / 132.0f), e = ex * ex + ey * ey;
      if (e < 1) mix_to(c, ink, 0.12f * clamp01((-dy - 22) * (1 / 250.0f)) * (1 - smooth_r(0.55f, 1 / 0.45f, e)));
    }
    if (d > 12 && d < 119) {                                          // guilloche rings in the sub-dial
      const int k = (int)(d * (1 / 6.0f) + 0.5f);
      mix_to(c, ink, (k % 3 == 0 ? 0.075f : 0.045f) * clamp01(1 - fabsf(d - 6 * k)));
    } else if (d > 138 && d < 264) {                                  // sunburst rays every 2 degrees
      constexpr float step = 2.0f * (float)M_PI / 180.0f;
      const float u = fast_atan2f(dx, -dy) * (1 / step);
      const float dist = fabsf(u - roundf(u)) * step * d;
      if (dist < 1) mix_to(c, ink, 0.045f * (1 - dist) * smooth_r(138, 1 / 22.0f, d) * (1 - smooth_r(244, 1 / 20.0f, d)));
    }
  }
  float a = 0;                                                        // rings: sub-dial, minute track, bezel, day track
  if (fabsf(d - 131) < 2) a += 0.18f * ring_at(d, 131, 2);
  if (fabsf(d - 268) < 1.5f) a += 0.16f * ring_at(d, 268, 1);
  if (fabsf(d - 288) < 1.5f) a += 0.16f * ring_at(d, 288, 1);
  if (fabsf(d - R_RING) < 5) {
    const float rx = x - RCX, ry = y - RCY, dr = sqrtf(rx * rx + ry * ry);
    a += 0.09f * ring_at(dr, R_RING, 6);
  }
  if (fabsf(d - 301.5f) < 3.5f) {                                     // glass bezel: lit top edge, shaded bottom edge
    const float up = -dy / d;
    a += 0.12f * ring_at(d, 301, 3);
    mix_to(c, ink, a + 0.50f * ring_at(d, 301, 2) * smooth_r(0.35f, 2.0f, up));
    mix_to(c, black, 0.45f * ring_at(d, 302.5f, 3) * smooth_r(0.35f, 2.0f, -up));
  } else if (a > 0) {
    mix_to(c, ink, a);
  }
}

// Runs on the UI thread for the first theme and in paint_task (core 0) for the others: no LVGL calls in here.
static HOT void paint_background(uint32_t* data, int stride_px, const Theme& th) {
  if (g_exp_lut[0] == 0) for (int i = 0; i < 1024; i++) g_exp_lut[i] = expf(-0.5f * i / 64.0f);
  const Glow glows[3] = {
    {CX, CY, 470, 470, 0.22f, th.acc3},                   // behind the dial
    {PX + PW - 30, 20, 430, 330, 0.18f, th.acc1},         // top right
    {PX + PW / 2, SCR_H + 40, 480, 280, 0.16f, th.acc1},  // under the calendar
  };
  const Ribbon ribs[2] = {
    // y0    slope   a1  k1       p1    a2  k2      p2    w   amp    k3      p3
    {585, -0.36f, 44, 0.0041f, 0.6f, 16, 0.011f, 2.0f, 64, 0.16f, 0.047f, 0.3f, th.acc1},
    {300, -0.20f, 56, 0.0030f, 2.4f, 20, 0.009f, 0.7f, 54, 0.11f, 0.061f, 1.1f, th.acc3},
  };
  DialCols dc;
  const Rgba ik = rgba(th.ink, 1), a3 = rgba(th.acc3, 1);
  dc.ink[0] = ik.r; dc.ink[1] = ik.g; dc.ink[2] = ik.b;
  dc.acc3[0] = a3.r; dc.acc3[1] = a3.g; dc.acc3[2] = a3.b;
  constexpr int S = 4, GW = SCR_W / S + 1, GH = SCR_H / S + 1;
  float* grid = (float*)heap_caps_malloc(sizeof(float) * 3 * GW * GH + sizeof(RibbonCol) * 2 * GW, MALLOC_CAP_SPIRAM);  // internal RAM is for WiFi
  if (!grid) return;
  RibbonCol* rcol = (RibbonCol*)(grid + 3 * GW * GH);
  for (int i = 0; i < GW; i++)
    for (int k = 0; k < 2; k++) {
      const Ribbon& r = ribs[k];
      const float x = i * S;
      RibbonCol& o = rcol[i * 2 + k];
      o.yc = r.y0 + r.slope * x + r.a1 * sinf(r.k1 * x + r.p1) + r.a2 * sinf(r.k2 * x + r.p2);
      o.amp = r.amp * (0.70f + 0.30f * sinf(r.k3 * x + r.p3)) * (0.93f + 0.07f * sinf(0.23f * x + 5 * r.p3));  // curtains
      o.w = r.w * (0.85f + 0.25f * sinf(0.0071f * x + r.p2));
    }
  for (int j = 0; j < GH; j++)
    for (int i = 0; i < GW; i++) sky_at(th, glows, ribs, rcol + i * 2, i * S, j * S, grid + (j * GW + i) * 3);

  for (int y = 0; y < SCR_H; y++) {
    const int gj = y / S;
    const float fy = (y % S) / (float)S;
    const float *g0 = grid + gj * GW * 3, *g1 = g0 + GW * 3;
    uint32_t* out = data + y * stride_px;
    for (int x = 0; x < SCR_W; x++) {
      const int gi = x / S;
      const float fx = (x % S) / (float)S;
      float c[3];
      for (int k = 0; k < 3; k++) {
        const float a = g0[gi * 3 + k] + (g0[gi * 3 + 3 + k] - g0[gi * 3 + k]) * fx;
        const float b = g1[gi * 3 + k] + (g1[gi * 3 + 3 + k] - g1[gi * 3 + k]) * fx;
        c[k] = a + (b - a) * fy;
      }
      if (x < CX + 346 && y > CY - 346 && y < CY + 346) dial_at(dc, x, y, c);
      out[x] = 0xFF000000u | ((uint32_t)(clamp01(c[0]) * 255 + 0.5f) << 16) |
               ((uint32_t)(clamp01(c[1]) * 255 + 0.5f) << 8) | (uint32_t)(clamp01(c[2]) * 255 + 0.5f);
    }
  }
  heap_caps_free(grid);
}

// Bokeh: translucent discs with a faint rim (the ones under the calendar get blurred by its frosted glass)
static void bokeh_draw_cb(lv_event_t* e) {
  lv_layer_t* L = lv_event_get_layer(e);
  const Theme& th = THEMES[g_render_theme];
  const uint32_t cols[3] = {th.acc1, th.acc2, th.acc3};
  uint32_t seed = 0x2545F491u;
  auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
  for (int i = 0, n = 0; i < 200 && n < 16; i++) {
    const float x = rnd() * SCR_W, y = rnd() * SCR_H, r = 6 + rnd() * rnd() * 46;
    const lv_opa_t fill = (lv_opa_t)(6 + rnd() * 12);
    if (hypotf(x - CX, y - CY) < 330 + r) continue;                              // the dial
    if (x + r > PX - 10 && x - r < PX + 480 && y + r > 70 && y - r < 245) continue;  // the big digits
    d_disc(L, x, y, r, cols[n++ % 3], fill, fill + 14);
  }
}

// Dial details: guilloche sub-dial, sunburst, railway minute track, applied indices, glass bezel, day-ring track
static void dial_deco_cb(lv_event_t* e) {
  lv_layer_t* L = lv_event_get_layer(e);
  const Theme& th = THEMES[g_render_theme];

  for (int i = 0; i < 60; i++) d_polar(L, i * 6.0f, 121, i % 5 ? 126 : 129, 1, th.ink, i % 5 ? 36 : 80);
  for (int i = 0; i < 240; i++) if (i % 4) d_polar(L, i * 1.5f, 282, 288, 1, th.ink, 48);
  for (int i = 0; i < 60; i++) if (i % 5) d_polar(L, i * 6.0f, 268, 288, 2, th.ink, 120);
  for (int i = 0; i < 12; i++) {
    const int n = i == 0 ? 2 : 1;                        // doubled index at 12 o'clock
    for (int k = 0; k < n; k++) {
      const float off = n == 2 ? (k ? 6.5f : -6.5f) : 0;
      d_polar(L, i * 30.0f, 247, 289, 16, th.acc1, 46, off);       // coloured glow
      d_polar(L, i * 30.0f, 251, 285, 7, th.acc1, 255, off);       // body
      d_polar(L, i * 30.0f, 253, 283, 2, th.ink, 170, off - 1.2f); // lit edge
    }
  }
  for (int h = 0; h < 24; h++) {                          // hour dots on the day-ring track
    const float a = h * 15.0f * (float)M_PI / 180.0f;
    const float x = RCX + sinf(a) * R_RING, y = RCY - cosf(a) * R_RING;
    if (h % 6 == 0) d_disc(L, x, y, 3.0f, th.acc1, 230);   // 6 px: exactly the arc's width, hidden once passed
    else d_disc(L, x, y, 2.0f, th.ink, 110);
  }
}

// Seconds bar track with 5-second ticks
static void panel_deco_cb(lv_event_t* e) {
  lv_layer_t* L = lv_event_get_layer(e);
  const Theme& th = THEMES[g_render_theme];
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = lv_color_hex(th.ink);
  d.bg_opa = 26;
  const lv_area_t a = {BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1};
  lv_draw_rect(L, &d, &a);
  for (int i = 0; i <= 12; i++) {
    const float x = BAR_X + 4 + i * (BAR_W - 8) / 12.0f;
    const bool major = i % 3 == 0;
    d_line(L, x, BAR_Y + BAR_H + 7, x, BAR_Y + BAR_H + (major ? 14 : 10), major ? 2 : 1, th.ink, major ? 100 : 55);
  }
}

static lv_obj_t* build_static_screen(const Theme& th, const lv_draw_buf_t* bg) {
  lv_obj_t* s = lv_obj_create(nullptr);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, SCR_W, SCR_H);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s, lv_color_hex(th.bg_top), 0);

  lv_obj_t* bgi = lv_image_create(s);
  lv_image_set_src(bgi, bg);
  lv_obj_set_pos(bgi, 0, 0);

  lv_obj_t* bokeh = mk(s, 0, 0, SCR_W, SCR_H);
  lv_obj_add_event_cb(bokeh, bokeh_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

  lv_obj_t* det = mk(s, CX - 340, CY - 340, 680, 680);
  lv_obj_add_event_cb(det, dial_deco_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // Numerals, each with a soft shadow
  for (int i = 1; i <= 12; i++) {
    char t[4];
    snprintf(t, sizeof(t), "%d", i);
    const float ang = i * 30.0f * (float)M_PI / 180.0f;
    const float rx = CX + sinf(ang) * 212.0f, ry = CY - cosf(ang) * 212.0f;
    const bool card = i % 3 == 0;
    lv_obj_t* sh = mk_label(s, f_num, 0x000000, LV_OPA_50, t, 0, 0, 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_t* l = mk_label(s, f_num, card ? th.acc1 : th.ink, card ? LV_OPA_COVER : 225, t, 0, 0, 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_update_layout(l);
    const int y = (int)ry - lv_obj_get_height(l) / 2;
    lv_obj_set_pos(sh, (int)rx - 50 + 1, y + 3);
    lv_obj_set_pos(l, (int)rx - 50, y);
  }

  lv_obj_t* brand = mk_label(s, f_tiny, th.ink, 110, "TAB5", CX - 100, CY - 92, 200, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_style_text_letter_space(brand, 8, 0);
  lv_obj_t* tname = mk_label(s, f_tiny, th.acc1, 150, th.name, CX - 100, CY + 70, 200, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_style_text_letter_space(tname, 6, 0);

  // Right panel: a glass pill behind the status icons, the seconds bar track
  lv_obj_t* pill = mk(s, PX + PW - 186, 25, 200, 42);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(pill, lv_color_hex(th.ink), 0);
  lv_obj_set_style_bg_opa(pill, 16, 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(th.ink), 0);
  lv_obj_set_style_border_opa(pill, 34, 0);

  lv_obj_t* pd = mk(s, PX, BAR_Y - 10, PW, 40);
  lv_obj_add_event_cb(pd, panel_deco_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // Calendar card: frosted glass (blurs the ribbons and bokeh behind it), lit top edge, drop shadow
  lv_obj_t* card = mk(s, PX, 392, PW, 296);
  lv_obj_set_style_radius(card, 28, 0);
  lv_obj_set_style_blur_backdrop(card, true, 0);
  lv_obj_set_style_blur_radius(card, 26, 0);
  {
    const uint32_t c[2] = {th.ink, th.ink};
    const lv_opa_t o[2] = {34, 12};
    set_grad(&g_grad_card, 2, c, o, LV_GRAD_DIR_VER);
  }
  lv_obj_set_style_bg_grad(card, &g_grad_card, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(th.ink), 0);
  lv_obj_set_style_border_opa(card, 36, 0);
  lv_obj_set_style_shadow_width(card, 50, 0);
  lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(card, 110, 0);
  lv_obj_set_style_shadow_offset_y(card, 16, 0);
  lv_obj_set_style_shadow_spread(card, -6, 0);
  {
    const uint32_t c[3] = {th.ink, th.ink, th.ink};
    const lv_opa_t o[3] = {0, 150, 0};
    set_grad(&g_grad_sheen, 3, c, o, LV_GRAD_DIR_HOR);
    lv_obj_t* sheen = mk(s, PX + 40, 392, PW - 80, 2);
    lv_obj_set_style_bg_grad(sheen, &g_grad_sheen, 0);
  }
  // faint tint behind the weekend columns
  const int cw = (PW - 40) / 7, gx = PX + 20;
  for (int c = 0; c < 7; c += 6) {
    lv_obj_t* col = mk(s, gx + c * cw + 8, 448, cw - 16, 234);
    lv_obj_set_style_radius(col, 16, 0);
    lv_obj_set_style_bg_color(col, lv_color_hex(th.acc2), 0);
    lv_obj_set_style_bg_opa(col, 14, 0);
  }

  lv_obj_update_layout(s);
  return s;
}

// RGB565 bands badly on smooth gradients, so the artwork is rendered in 32 bit and
// ordered-dithered (4x4 Bayer) down to the RGB565 image that is actually displayed.
static HOT lv_draw_buf_t* dither_to_rgb565(const lv_draw_buf_t* src) {
  static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const uint32_t w = src->header.w, h = src->header.h;
  lv_draw_buf_t* dst = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!dst) return nullptr;
  for (uint32_t y = 0; y < h; y++) {
    const uint32_t* in = (const uint32_t*)((const uint8_t*)src->data + y * src->header.stride);
    uint16_t* out = (uint16_t*)((uint8_t*)dst->data + y * dst->header.stride);
    const uint8_t* brow = bayer[y & 3];
    for (uint32_t x = 0; x < w; x++) {
      const uint32_t p = in[x];
      const int t = brow[x & 3];
      const int add5 = (t * 8 + 4) >> 4, add6 = (t * 4 + 2) >> 4;
      const int r = min(255, (int)((p >> 16) & 0xFF) + add5) >> 3;
      const int g = min(255, (int)((p >> 8) & 0xFF) + add6) >> 2;
      const int b = min(255, (int)(p & 0xFF) + add5) >> 3;
      out[x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
  return dst;
}

// Render (once) the artwork for theme `idx`, from a background painted by paint_task if there is one.
static void render_static(int idx, uint32_t* painted = nullptr) {
  if (g_static_bufs[idx]) { heap_caps_free(painted); return; }
  const int64_t t0 = esp_timer_get_time();
  g_render_theme = idx;
  uint32_t* px = painted ? painted : (uint32_t*)heap_caps_aligned_alloc(64, SCR_W * SCR_H * 4, MALLOC_CAP_SPIRAM);
  lv_draw_buf_t* full = nullptr;
  if (px) {
    if (!painted) paint_background(px, SCR_W, THEMES[idx]);
    static lv_draw_buf_t bg;
    lv_draw_buf_init(&bg, SCR_W, SCR_H, LV_COLOR_FORMAT_XRGB8888, SCR_W * 4, px, SCR_W * SCR_H * 4);
    lv_obj_t* s = build_static_screen(THEMES[idx], &bg);
    full = lv_snapshot_take(s, LV_COLOR_FORMAT_XRGB8888);
    lv_obj_delete(s);
    lv_image_cache_drop(&bg);
    heap_caps_free(px);
  }
  g_static_bufs[idx] = full ? dither_to_rgb565(full) : nullptr;
  if (full) lv_draw_buf_destroy(full);
  Serial.printf("theme %d artwork rendered in %d ms%s%s\n", idx, (int)((esp_timer_get_time() - t0) / 1000),
                painted ? " (background pre-painted)" : "", g_static_bufs[idx] ? "" : " (FAILED)");
}

// The per-pixel painting of the other themes' backgrounds runs on core 0 at idle priority, one at a time; the UI
// thread only has to snapshot + dither each one (~0.3 s).
static std::atomic<uint32_t*> g_painted[N_THEMES];

static void paint_task(void* arg) {
  const int initial_theme = (int)(intptr_t)arg;
  for (int i = 0; i < N_THEMES; i++) {
    if (i == initial_theme) continue;
    uint32_t* px = (uint32_t*)heap_caps_aligned_alloc(64, SCR_W * SCR_H * 4, MALLOC_CAP_SPIRAM);
    if (!px) break;
    paint_background(px, SCR_W, THEMES[i]);
    g_painted[i] = px;
    while (g_painted[i]) vTaskDelay(pdMS_TO_TICKS(100));   // wait until the UI has used it (bounds PSRAM use)
  }
  vTaskDelete(nullptr);
}

static void show_static() {
  if (!g_static_bufs[g_theme]) render_static(g_theme);
  g_comp_full = true;   // re-composited (and redrawn) on the next frame
}

// ----------------------------------------------------------------------------------------------
// Live layer: day ring, hands and hub are drawn by one callback; only small areas around what moved are
// invalidated each frame (lv_inv_area directly - lv_obj_invalidate walks the whole object tree every call).
// ----------------------------------------------------------------------------------------------
static void inv_box(float x1, float y1, float x2, float y2) {    // redraw this screen area
  const lv_area_t a = {(int32_t)floorf(x1), (int32_t)floorf(y1), (int32_t)ceilf(x2), (int32_t)ceilf(y2)};
  lv_inv_area(g_disp, &a);
}

static void comp_box(float x1, float y1, float x2, float y2) {   // re-composite (and then redraw) this area
  if (g_merge_regions && g_comp_n == MAX_COMP_BOX) merge_dirty_regions(g_comp_box, g_comp_n);
  if (g_comp_n == MAX_COMP_BOX) { g_comp_full = true; return; }
  lv_area_t& a = g_comp_box[g_comp_n++];
  a.x1 = max(0, (int)floorf(x1)); a.y1 = max(0, (int)floorf(y1));
  a.x2 = min(SCR_W - 1, (int)ceilf(x2)); a.y2 = min(SCR_H - 1, (int)ceilf(y2));
}

// Invalidate what a hand covers at angle d0 and at d1 (old and new position) including its shadow, as a chain of
// short boxes hugging the (usually diagonal) line rather than one big bounding box.
static void inv_hand(BoxSink sink, float d0, float d1, float r0, float r1, float pad, float shx, float shy) {
  if (fabsf(d1 - d0) > 3.0f) {  // a big jump: two separate chains are smaller than one fat one
    inv_hand(sink, d0, d0, r0, r1, pad, shx, shy);
    inv_hand(sink, d1, d1, r0, r1, pad, shx, shy);
    return;
  }
  const float a0 = d0 * (float)M_PI / 180.0f, a1 = d1 * (float)M_PI / 180.0f;
  const float s0 = sinf(a0), c0 = cosf(a0), s1 = sinf(a1), c1 = cosf(a1);
  const int n = max(1, (int)ceilf((r1 - r0) / SEG));
  for (int i = 0; i < n; i++) {
    const float ra = r0 + (r1 - r0) * i / n, rb = r0 + (r1 - r0) * (i + 1) / n;
    const float xs[4] = {s0 * ra, s0 * rb, s1 * ra, s1 * rb}, ys[4] = {-c0 * ra, -c0 * rb, -c1 * ra, -c1 * rb};
    float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
    for (int k = 1; k < 4; k++) { x1 = fminf(x1, xs[k]); x2 = fmaxf(x2, xs[k]); y1 = fminf(y1, ys[k]); y2 = fmaxf(y2, ys[k]); }
    sink(CX + x1 - pad + fminf(0, shx), CY + y1 - pad + fminf(0, shy), CX + x2 + pad + fmaxf(0, shx), CY + y2 + pad + fmaxf(0, shy));
  }
}

static void inv_sprite_path(BoxSink sink, float d0, float d1, float r, float half) {
  const float a0 = d0 * (float)M_PI / 180.0f, a1 = d1 * (float)M_PI / 180.0f;
  const float x0 = CX + sinf(a0) * r, y0 = CY - cosf(a0) * r, x1 = CX + sinf(a1) * r, y1 = CY - cosf(a1) * r;
  if (fabsf(d1 - d0) > 3.0f) {
    sink(x0 - half, y0 - half, x0 + half, y0 + half);
    sink(x1 - half, y1 - half, x1 + half, y1 + half);
  } else {
    sink(fminf(x0, x1) - half, fminf(y0, y1) - half, fmaxf(x0, x1) + half, fmaxf(y0, y1) + half);
  }
}

static void draw_hand(lv_layer_t* L, float deg, const HandSpec& h, const Theme& th) {
  const float a = deg * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
  const float x0 = CX + s * h.r0, y0 = CY - c * h.r0, x1 = CX + s * h.r1, y1 = CY - c * h.r1;
  d_line(L, x0, y0, x1, y1, (int)h.w, th.ink, LV_OPA_COVER);
  // a lighter facet along one side makes the hand read as bevelled metal
  const float fx = c * (-h.w / 4), fy = s * (-h.w / 4);
  d_line(L, CX + s * 4 + fx, CY - c * 4 + fy, CX + s * (h.r1 - h.w / 2) + fx, CY - c * (h.r1 - h.w / 2) + fy,
         (int)(h.w / 2), 0xFFFFFF, 70);
  d_line(L, CX + s * h.lume0, CY - c * h.lume0, CX + s * h.lume1, CY - c * h.lume1, (int)h.lume_w, th.acc1, 235);
}

static void draw_hand_shadow(lv_layer_t* L, float deg, const HandSpec& h) {
  const float a = deg * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
  const float x0 = CX + s * h.r0 + SH_DX, y0 = CY - c * h.r0 + SH_DY, x1 = CX + s * h.r1 + SH_DX, y1 = CY - c * h.r1 + SH_DY;
  if (g_soft_shadows) d_line(L, x0, y0, x1, y1, (int)h.w + 8, 0x000000, 30);   // soft penumbra
  d_line(L, x0, y0, x1, y1, (int)h.w + 1, 0x000000, 60);   // core
}

// Everything that lives in the composite, clipped to the layer's current clip area.
static void comp_draw(lv_layer_t* L) {
  const Theme& th = THEMES[g_theme];
  const lv_area_t& cl = L->_clip_area;
  lv_draw_image_dsc_t img;
  lv_draw_image_dsc_init(&img);
  img.src = g_static_bufs[g_theme];
  const lv_area_t scr = {0, 0, SCR_W - 1, SCR_H - 1};
  if (img.src) lv_draw_image(L, &img, &scr);

  // Day ring - skipped unless this area actually touches the ring annulus
  if (g_show_ring) {
    const float nx = fmaxf(cl.x1 - CX, fminf(0.0f, (float)(cl.x2 - CX))), ny = fmaxf(cl.y1 - CY, fminf(0.0f, (float)(cl.y2 - CY)));
    const float fx = fmaxf(fabsf(cl.x1 - CX), fabsf(cl.x2 - CX)), fy = fmaxf(fabsf(cl.y1 - CY), fabsf(cl.y2 - CY));
    if (sqrtf(nx * nx + ny * ny) < R_RING + 24 && sqrtf(fx * fx + fy * fy) > R_RING - 24) {
      const float deg = g_day_m * 0.25f;
      if (g_day_m > 0) d_arc(L, CX, CY, (int)R_RING + 3, 6, 270, 270 + deg, th.acc1, LV_OPA_COVER, true);
      const float a = deg * (float)M_PI / 180.0f;
      d_sprite(L, spr_knob, RCX + sinf(a) * R_RING, RCY - cosf(a) * R_RING);
    }
  }
  if (g_show_shadows) {
    draw_hand_shadow(L, g_ah, HS_H);
    draw_hand_shadow(L, g_am, HS_M);
  }
  draw_hand(L, g_ah, HS_H, th);
  draw_hand(L, g_am, HS_M, th);
}

// Re-composite the queued areas (or everything) and invalidate them on screen.
static void comp_apply() {
  if (!g_comp_full && !g_comp_n) return;
  if (g_merge_regions && !g_comp_full) merge_dirty_regions(g_comp_box, g_comp_n);
  lv_layer_t layer;
  lv_canvas_init_layer(comp_canvas, &layer);
  if (g_comp_full) {
    comp_draw(&layer);
  } else {
    for (int i = 0; i < g_comp_n; i++) {
      layer._clip_area = layer.phy_clip_area = g_comp_box[i];
      comp_draw(&layer);
    }
  }
  lv_canvas_finish_layer(comp_canvas, &layer);
  if (g_comp_full) lv_obj_invalidate(bg_img);
  else for (int i = 0; i < g_comp_n; i++) lv_inv_area(g_disp, &g_comp_box[i]);
  g_comp_full = false;
  g_comp_n = 0;
}

// Per frame, on top of the composite: second hand (with shadow), its lens tip, and the hub.
static void dial_draw_cb(lv_event_t* e) {
  lv_layer_t* L = lv_event_get_layer(e);
  const Theme& th = THEMES[g_theme];
  const float as = g_as * (float)M_PI / 180.0f, ss = sinf(as), cs = cosf(as);
  if (g_show_shadows) {
    d_line(L, CX + ss * S_R0 + SS_DX, CY - cs * S_R0 + SS_DY, CX + ss * S_TAIL + SS_DX, CY - cs * S_TAIL + SS_DY, 9, 0x000000, 60);
    d_line(L, CX + ss * S_TAIL + SS_DX, CY - cs * S_TAIL + SS_DY, CX + ss * S_R1 + SS_DX, CY - cs * S_R1 + SS_DY, 4, 0x000000, 60);
  }
  d_line(L, CX + ss * S_R0, CY - cs * S_R0, CX + ss * S_TAIL, CY - cs * S_TAIL, 8, th.acc2, LV_OPA_COVER);
  d_line(L, CX + ss * S_TAIL, CY - cs * S_TAIL, CX + ss * S_R1, CY - cs * S_R1, 3, th.acc2, LV_OPA_COVER);
  d_sprite(L, spr_tip, CX + ss * R_TIP, CY - cs * R_TIP);
  d_sprite(L, spr_hub, CX, CY);
}

static void bar_draw_cb(lv_event_t* e) {
  lv_layer_t* L = lv_event_get_layer(e);
  const float xe = BAR_X + g_bar * BAR_W;
  if (xe - BAR_X >= BAR_H) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad = g_grad_bar;
    d.bg_color = g_grad_bar.stops[0].color;
    const lv_area_t a = {BAR_X, BAR_Y, (int32_t)lroundf(xe), BAR_Y + BAR_H - 1};
    lv_draw_rect(L, &d, &a);
  }
  d_sprite(L, spr_head, xe, BAR_Y + (BAR_H - 1) / 2.0f);
}

// Move the second hand / seconds bar to `sec` (0..60, fractional)
static void set_seconds(float sec, bool force) {
  const float as = sec * 6.0f;
  if (!force) {
    inv_hand(inv_box, g_as, as, S_R0, S_R1, 7, SS_DX, SS_DY);
    inv_sprite_path(inv_box, g_as, as, R_TIP, 33);
  }
  g_as = as;
  const float f = sec / 60.0f;
  if (!force) {
    if (f < g_bar) inv_box(BAR_X - 20, BAR_Y - 18, BAR_X + BAR_W + 20, BAR_Y + BAR_H + 18);  // wrapped around
    else inv_box(BAR_X + g_bar * BAR_W - 20, BAR_Y - 18, BAR_X + f * BAR_W + 20, BAR_Y + BAR_H + 18);
  }
  g_bar = f;
}

static void set_hands_hm(const Now& n, bool hour_too, bool force) {
  const float min_f = n.mi + n.s / 60.0f;
  const float am = min_f * 6.0f, ah = ((n.h % 12) + min_f / 60.0f) * 30.0f;
  if (!force) inv_hand(comp_box, g_am, am, HS_M.r0, HS_M.r1, HS_M.w / 2 + 6, SH_DX, SH_DY);
  g_am = am;
  if (hour_too) {
    if (!force) inv_hand(comp_box, g_ah, ah, HS_H.r0, HS_H.r1, HS_H.w / 2 + 6, SH_DX, SH_DY);
    g_ah = ah;
  }
}

static void set_day_ring(int m, bool force) {
  if (m == g_day_m && !force) return;
  if (!force) {
    if (m < g_day_m) g_comp_full = true;   // midnight: the ring empties
    else inv_sprite_path(comp_box, g_day_m * 0.25f, m * 0.25f, R_RING, 24);
  }
  g_day_m = m;
}

static void apply_dynamic_theme() {
  const Theme& th = THEMES[g_theme];
  make_sprites(th);
  const uint32_t c[2] = {th.acc1, th.acc2};
  const lv_opa_t o[2] = {LV_OPA_COVER, LV_OPA_COVER};
  set_grad(&g_grad_bar, 2, c, o, LV_GRAD_DIR_HOR);
  lv_obj_set_style_text_color(lbl_greet, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_text_color(lbl_time, lv_color_hex(th.ink), 0);
  lv_obj_set_style_text_color(lbl_date, lv_color_hex(th.ink), 0);
  lv_obj_set_style_text_color(lbl_batt, lv_color_hex(th.ink), 0);
  lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(ntp_active() ? th.acc1 : th.ink), 0);
  lv_obj_set_style_text_color(lbl_ampm, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_text_color(lbl_sec, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_bg_color(today_mark, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_shadow_color(today_mark, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_bg_color(week_mark, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_text_color(lbl_month, lv_color_hex(th.acc1), 0);
  for (int c = 0; c < 7; c++) {
    const bool we = c == 0 || c == 6;
    lv_obj_set_style_text_color(lbl_wd[c], lv_color_hex(we ? th.acc2 : th.ink), 0);
  }
}

static void build_dynamic_ui() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  g_comp = lv_draw_buf_create(SCR_W, SCR_H, LV_COLOR_FORMAT_RGB565, 0);
  comp_canvas = lv_canvas_create(lv_layer_bottom());
  lv_obj_add_flag(comp_canvas, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_draw_buf(comp_canvas, g_comp);
  bg_img = lv_image_create(scr);
  lv_image_set_src(bg_img, g_comp);
  lv_obj_set_pos(bg_img, 0, 0);

  dial_obj = mk(scr, CX - 345, CY - 345, 690, 690);
  lv_obj_add_event_cb(dial_obj, dial_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  secbar_obj = mk(scr, BAR_X - 20, BAR_Y - 18, BAR_W + 40, BAR_H + 36);
  lv_obj_add_event_cb(secbar_obj, bar_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // Right-hand panel: greeting, time, date, seconds
  lbl_greet = mk_label(scr, f_head, 0xFFFFFF, LV_OPA_COVER, "", PX + 4, 30, 400);
  lv_obj_set_style_text_letter_space(lbl_greet, 4, 0);
  lv_obj_set_width(lbl_greet, 330);  // keep clear of the WiFi/battery icons on the right
  lbl_batt = mk_label(scr, &lv_font_montserrat_20, 0xFFFFFF, 190, "", PX + PW - 190, 36, 180, LV_TEXT_ALIGN_RIGHT);
  lbl_wifi = mk_label(scr, &lv_font_montserrat_20, 0xFFFFFF, LV_OPA_30, LV_SYMBOL_WIFI, PX + PW - 170, 36, 40);

  lbl_time = mk_label(scr, f_digits, 0xFFFFFF, LV_OPA_COVER, "00:00", PX - 6, 36, 470);
  lbl_ampm = mk_label(scr, f_sec, 0xFFFFFF, LV_OPA_COVER, "", PX + PW - 60, 104, 70);
  lbl_date = mk_label(scr, f_date, 0xFFFFFF, 235, "", PX + 4, 252, PW);
  lbl_sec = mk_label(scr, f_small, 0xFFFFFF, LV_OPA_COVER, "", PX + PW - 90, 322, 90, LV_TEXT_ALIGN_RIGHT);

  // Calendar
  lbl_month = mk_label(scr, f_head, 0xFFFFFF, LV_OPA_COVER, "", PX + 26, 408, PW - 52);
  lv_obj_set_style_text_letter_space(lbl_month, 4, 0);
  const int cw = (PW - 40) / 7, gx = PX + 20;
  const char* wd_short[7] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int c = 0; c < 7; c++)
    lbl_wd[c] = mk_label(scr, f_small, 0xFFFFFF, 150, wd_short[c], gx + c * cw, 452, cw, LV_TEXT_ALIGN_CENTER);

  week_mark = mk(scr, gx + 2, 0, 7 * cw - 4, 32);   // translucent band behind the current week
  lv_obj_set_style_radius(week_mark, 16, 0);
  lv_obj_set_style_bg_opa(week_mark, 30, 0);
  lv_obj_add_flag(week_mark, LV_OBJ_FLAG_HIDDEN);

  today_mark = mk(scr, 0, 0, 34, 34);
  lv_obj_set_style_radius(today_mark, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(today_mark, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(today_mark, 20, 0);
  lv_obj_set_style_shadow_opa(today_mark, LV_OPA_60, 0);
  lv_obj_add_flag(today_mark, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < 42; i++)
    lbl_day[i] = mk_label(scr, f_cal, 0xFFFFFF, LV_OPA_COVER, "", gx + (i % 7) * cw, 486 + (i / 7) * 33, cw,
                          LV_TEXT_ALIGN_CENTER);
}

// ----------------------------------------------------------------------------------------------
// Content updates
// ----------------------------------------------------------------------------------------------
static void update_calendar(const Now& n) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%s %d", MONTHS[n.mo - 1], n.y);
  for (char* p = buf; *p; p++) *p = toupper((unsigned char)*p);
  lv_label_set_text(lbl_month, buf);

  const Theme& th = THEMES[g_theme];
  const int first = weekday_of(n.y, n.mo, 1);
  const int dim = days_in_month(n.y, n.mo);
  const int pm = n.mo == 1 ? 12 : n.mo - 1, py = n.mo == 1 ? n.y - 1 : n.y;
  const int pdim = days_in_month(py, pm);

  lv_obj_add_flag(today_mark, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(week_mark, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 42; i++) {
    int day = i - first + 1;
    const bool in_month = day >= 1 && day <= dim;
    if (day < 1) day += pdim;
    else if (day > dim) day -= dim;
    snprintf(buf, sizeof(buf), "%d", day);
    lv_label_set_text(lbl_day[i], buf);

    const bool weekend = (i % 7 == 0) || (i % 7 == 6);
    lv_obj_t* l = lbl_day[i];
    if (!in_month) {
      lv_obj_set_style_text_color(l, lv_color_hex(th.ink), 0);
      lv_obj_set_style_text_opa(l, 56, 0);
    } else if (day == n.d) {
      lv_obj_set_style_text_color(l, lv_color_hex(th.bg_top), 0);
      lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
      lv_obj_update_layout(l);
      const int cy = lv_obj_get_y(l) + lv_obj_get_height(l) / 2;
      lv_obj_set_pos(today_mark, lv_obj_get_x(l) + lv_obj_get_width(l) / 2 - 17, cy - 17);
      lv_obj_remove_flag(today_mark, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_y(week_mark, cy - 16);
      lv_obj_remove_flag(week_mark, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_set_style_text_color(l, lv_color_hex(weekend ? th.acc2 : th.ink), 0);
      lv_obj_set_style_text_opa(l, weekend ? 240 : 215, 0);
    }
  }
}

static void update_date(const Now& n) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s, %s %d", WEEKDAYS[n.wd], MONTHS[n.mo - 1], n.d);
  lv_label_set_text(lbl_date, buf);
  update_calendar(n);
}

static void update_minute(const Now& n, bool force) {
  char buf[16];
  if (g_24h) {
    snprintf(buf, sizeof(buf), "%02d:%02d", n.h, n.mi);
    lv_obj_add_flag(lbl_ampm, LV_OBJ_FLAG_HIDDEN);
  } else {
    snprintf(buf, sizeof(buf), "%d:%02d", n.h % 12 == 0 ? 12 : n.h % 12, n.mi);
    lv_label_set_text(lbl_ampm, n.h < 12 ? "AM" : "PM");
    lv_obj_remove_flag(lbl_ampm, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text(lbl_time, buf);
  set_day_ring(n.h * 60 + n.mi, force);

  const char* g = n.h < 5 ? "GOOD NIGHT" : n.h < 12 ? "GOOD MORNING" : n.h < 18 ? "GOOD AFTERNOON" : "GOOD EVENING";
  lv_label_set_text(lbl_greet, g);
}

static BatteryPresence g_battery_presence;
static bool g_usb_only = false; // display preference for installations with no battery pack
static int g_battery_mv = 0, g_battery_ma = 0, g_battery_level = 0;
static bool g_battery_charging = false, g_battery_external = false;
static int64_t g_battery_sample_at = -1000;

static ChargeLimiter g_charge_limit;
static bool g_charge_en = true;  // CHG_EN as last written (M5Unified enables charging at boot)

static void apply_charge_limit() {
  const bool want = g_charge_limit.update(g_battery_mv, !g_usb_only && g_battery_presence.state == BatteryPresence::Present);
  if (want == g_charge_en) return;
  M5.Power.setBatteryCharge(want);
  g_charge_en = want;
  Serial.printf("battery: charging %s at %d mV\n", want ? "resumed" : "paused", g_battery_mv);
}

static void sample_battery() {
  if (mono_ms() - g_battery_sample_at < 500) return;
  g_battery_sample_at = mono_ms();
  if (g_usb_only) { g_battery_presence.state = BatteryPresence::Absent; apply_charge_limit(); return; }
  g_battery_mv = M5.Power.getBatteryVoltage();
  g_battery_presence.update(g_battery_mv, g_battery_sample_at);
  apply_charge_limit();
  if (g_battery_presence.state == BatteryPresence::Present) {
    g_battery_ma = M5.Power.getBatteryCurrent();
    g_battery_level = battery_percent(g_battery_mv);
    // CHG_STAT (what isCharging() reads) floats "charging" with USB unplugged, so use the pack current
    // instead: measured ~+680 mA charging, ~0 mA full on USB, ~-150 mA running on the battery.
    g_battery_charging = g_battery_ma > 20;
    g_battery_external = g_battery_ma > -30;
  }
}

static void update_battery() {
  sample_battery();
  const bool synced = ntp_active();
  static int last_theme = -1, last_synced = -1;
  if (last_theme != g_theme || last_synced != (int)synced) {
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(synced ? THEMES[g_theme].acc1 : THEMES[g_theme].ink), 0);
    lv_obj_set_style_text_opa(lbl_wifi, synced ? LV_OPA_COVER : LV_OPA_30, 0);
    last_theme = g_theme; last_synced = synced;
  }
  char buf[48];
  if (g_battery_presence.state != BatteryPresence::Present) {
    snprintf(buf, sizeof(buf), "%s", g_battery_presence.state == BatteryPresence::Absent ? LV_SYMBOL_USB "  USB" : "...");
  } else {
    const int lvl = g_battery_level;
    const char* icon = lvl > 85 ? LV_SYMBOL_BATTERY_FULL : lvl > 60 ? LV_SYMBOL_BATTERY_3 : lvl > 35 ? LV_SYMBOL_BATTERY_2
                     : lvl > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    snprintf(buf, sizeof(buf), "%s%s  %d%%", g_battery_external ? LV_SYMBOL_CHARGE "  " : "", icon, lvl);
  }
  // Polling presence must not redraw an unchanged header twice per second.
  if (strcmp(lv_label_get_text(lbl_batt), buf)) lv_label_set_text(lbl_batt, buf);
}

static Stat st_upd, st_tap, st_rtc, st_ser, st_lv;
static void stat_add(Stat& st, int64_t t0) {
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  if (us > st.max_us) st.max_us = us;
  if (us > 50000) st.slow++;
}
static uint16_t g_slow_by_pos[12];  // slow (>40 ms) frames bucketed by second-hand position (5 s = 30 deg)
static uint16_t g_frames_by_pos[12];
static int g_auto_left = 0;  // stress test: remaining automatic theme switches
static int64_t g_auto_next = 0;

static void set_theme(int idx, bool save = true) {
  const int64_t t0 = esp_timer_get_time();
  g_theme = ((idx % N_THEMES) + N_THEMES) % N_THEMES;
  if (save) prefs.putInt("theme", g_theme);
  const int64_t t1 = esp_timer_get_time();
  show_static();
  const int64_t t2 = esp_timer_get_time();
  apply_dynamic_theme();
  g_dirty_all = true;
  Serial.printf("set_theme: prefs %d ms, static %d ms, dyn %d ms\n", (int)((t1 - t0) / 1000),
                (int)((t2 - t1) / 1000), (int)((esp_timer_get_time() - t2) / 1000));
}

static void fast_cb(lv_timer_t*) {
  static int last_s = -1;
  static int64_t last_mi = -1, last_d = -1;
  const Now n = now_local();

  // DST can jump local time while UTC remains continuous. Repaint the entire day
  // ring and hands after any discontinuity, not just its old/new endpoint sprites.
  const int64_t minute = minute_key(n);
  const bool force = g_dirty_all || (last_mi >= 0 && minute != last_mi && minute != last_mi + 1);
  if (force) {
    last_s = last_mi = last_d = -1;
    g_dirty_all = false;
    g_comp_full = true;
    lv_obj_invalidate(secbar_obj);
  }
  set_seconds(n.s + n.ms / 1000.0f, force);
  if (n.s != last_s) {
    last_s = n.s;
    // The hour/minute hands move well under a pixel per second: step the minute hand every 3 s and the hour
    // hand every 30 s.
    if (force || n.s % 3 == 0) set_hands_hm(n, force || n.s % 30 == 0, force);
    char b[8];
    snprintf(b, sizeof(b), "%02d s", n.s);
    lv_label_set_text(lbl_sec, b);
  }
  if (minute_key(n) != last_mi) { last_mi = minute_key(n); update_minute(n, force); }
  if (date_key(n) != last_d) { last_d = date_key(n); update_date(n); }
  comp_apply();
}

// Finish the themes whose backgrounds paint_task has painted (at most one per call).
static void render_pending_themes() {
  for (int i = 0; i < N_THEMES; i++)
    if (g_painted[i]) { uint32_t* px = g_painted[i]; render_static(i, px); g_painted[i] = nullptr; return; }
}

// ----------------------------------------------------------------------------------------------
// WiFi settings screen: scan -> pick a network -> type the password on the on-screen keyboard.
// Lives on LVGL's top layer; while it is open the clock animation is paused.
// ----------------------------------------------------------------------------------------------
static lv_obj_t *wifi_ui, *wl_list, *wl_status, *wl_page_list, *wl_page_pw, *wl_ta, *wl_kb, *wl_pw_title;
static lv_obj_t *wl_btn_a_lbl, *wl_btn_connect, *wl_btn_show_lbl;
static lv_timer_t *g_fast_timer = nullptr, *g_wifi_timer = nullptr;
static lv_indev_t *g_indev = nullptr;          // LVGL touch input, only polled while the WiFi screen is open
static bool    g_wifi_open = false;
static int     g_wl_page = 0;                 // 0 = network list, 1 = password entry
static int     g_wl_count_at_connect = -1;    // g_ntp_count when the user pressed Connect (-1 = not waiting)
static int64_t g_wl_close_at = 0;
static char    g_sel_ssid[33];
static ScanEntry g_wl_scan[MAX_SCAN]; // snapshot corresponding to the displayed rows
static int g_wl_scan_n = 0;

static lv_obj_t* wl_button(lv_obj_t* parent, const char* text, int x, int y, int w, int h, lv_event_cb_t cb,
                           uint32_t color, lv_obj_t** label_out = nullptr) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 18, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(b, lv_color_lighten(lv_color_hex(color), 60), LV_STATE_PRESSED);
  lv_obj_t* l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  if (label_out) *label_out = l;
  return b;
}

static void wl_show_page(int p) {
  g_wl_page = p;
  if (p == 0) { lv_obj_remove_flag(wl_page_list, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(wl_page_pw, LV_OBJ_FLAG_HIDDEN); }
  else        { lv_obj_add_flag(wl_page_list, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(wl_page_pw, LV_OBJ_FLAG_HIDDEN); }
  lv_label_set_text(wl_btn_a_lbl, p == 0 ? "Scan" : "Back");
  if (p == 1) {
    lv_keyboard_set_textarea(wl_kb, wl_ta);
    lv_obj_add_state(wl_ta, LV_STATE_FOCUSED);
  }
}

static void wifi_close() {
  if (!g_wifi_open) return;
  g_wifi_open = false;
  lv_obj_add_flag(wifi_ui, LV_OBJ_FLAG_HIDDEN);
  lv_timer_pause(g_wifi_timer);
  lv_timer_pause(lv_indev_get_read_timer(g_indev));
  lv_timer_resume(g_fast_timer);
  g_dirty_all = true;
  lv_obj_invalidate(lv_screen_active());
}

static void wl_connect(const char* ssid, const char* pass) {
  if (strlen(ssid) > 32 || strlen(pass) > 64) { lv_label_set_text(wl_status, "SSID or password too long"); return; }
  const String new_pass(pass);  // pass may point into g_pass (saved password reused)
  ++g_net_generation;
  g_ssid = ssid;
  g_pass = new_pass;
  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  g_wl_count_at_connect = g_ntp_count;
  g_wl_close_at = 0;
  net_request(1);
  char b[80];
  snprintf(b, sizeof(b), "Connecting to %s ...", ssid);
  lv_label_set_text(wl_status, b);
  wl_show_page(0);
}

// A saved password is never put in the text area (Show would reveal it). A fixed stand-in shows
// that one is saved; connecting unchanged reuses it, and the first edit replaces the stand-in.
static constexpr const char* WL_SAVED_PW = "********";
static bool g_wl_saved_pw = false;
static const char* wl_password() { return g_wl_saved_pw ? g_pass.c_str() : lv_textarea_get_text(wl_ta); }

static void wl_select(int idx) {
  if (idx < 0 || idx >= g_wl_scan_n) return;
  strlcpy(g_sel_ssid, g_wl_scan[idx].ssid, sizeof(g_sel_ssid));
  if (!g_wl_scan[idx].secure) { wl_connect(g_sel_ssid, ""); return; }
  char b[80];
  snprintf(b, sizeof(b), "Password for  %s", g_sel_ssid);
  lv_label_set_text(wl_pw_title, b);
  g_wl_saved_pw = false;
  lv_textarea_set_text(wl_ta, g_ssid == g_sel_ssid && !g_pass.isEmpty() ? WL_SAVED_PW : "");
  g_wl_saved_pw = g_ssid == g_sel_ssid && !g_pass.isEmpty();
  lv_textarea_set_password_mode(wl_ta, true);
  lv_label_set_text(wl_btn_show_lbl, "Show");
  wl_show_page(1);
}

static void wl_row_cb(lv_event_t* e) { wl_select((int)(intptr_t)lv_event_get_user_data(e)); }

static void wl_rebuild_list() {
  lv_obj_clean(wl_list);
  g_wl_scan_n = g_scan_n;
  memcpy(g_wl_scan, g_scan, sizeof(g_wl_scan));
  const int n = g_wl_scan_n;
  if (n == 0) {
    lv_obj_t* l = lv_label_create(wl_list);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A93B8), 0);
    lv_label_set_text(l, "No networks found - press Scan");
    return;
  }
  for (int i = 0; i < n; i++) {
    const ScanEntry& s = g_wl_scan[i];
    lv_obj_t* row = lv_button_create(wl_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 68);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1C2340), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x33407A), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, wl_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    const bool saved = g_ssid == s.ssid;
    char nm[64];
    snprintf(nm, sizeof(nm), "%s%s", saved ? LV_SYMBOL_OK "  " : "", s.ssid);
    lv_obj_t* name = lv_label_create(row);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(name, saved ? lv_color_hex(THEMES[g_theme].acc1) : lv_color_white(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 760);
    lv_label_set_text(name, nm);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 24, 0);

    const uint32_t sig = s.rssi > -60 ? 0x4ADE80 : s.rssi > -75 ? 0xFACC15 : 0xFB923C;
    char info[48];
    snprintf(info, sizeof(info), "%s %d dBm    %s", LV_SYMBOL_WIFI, (int)s.rssi, s.secure ? "secured" : "OPEN");
    lv_obj_t* inf = lv_label_create(row);
    lv_obj_set_style_text_font(inf, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(inf, lv_color_hex(sig), 0);
    lv_label_set_text(inf, info);
    lv_obj_align(inf, LV_ALIGN_RIGHT_MID, -24, 0);
  }
}

static void wl_a_cb(lv_event_t*) {  // "Scan" on the list page, "Back" on the password page
  if (g_wl_page == 1) { wl_show_page(0); return; }
  net_request(2);
  lv_label_set_text(wl_status, "Scanning ...");
}
static void wl_close_cb(lv_event_t*) { wifi_close(); }
static void wl_show_cb(lv_event_t*) {
  const bool pw = !lv_textarea_get_password_mode(wl_ta);
  lv_textarea_set_password_mode(wl_ta, pw);
  lv_label_set_text(wl_btn_show_lbl, pw ? "Show" : "Hide");
}
static void wl_ta_cb(lv_event_t*) {
  if (!g_wl_saved_pw) return;
  g_wl_saved_pw = false;  // keep only what was typed after the stand-in; a backspace clears it
  const char* t = lv_textarea_get_text(wl_ta);
  const size_t n = strlen(WL_SAVED_PW);
  lv_textarea_set_text(wl_ta, strlen(t) > n && !strncmp(t, WL_SAVED_PW, n) ? String(t + n).c_str() : "");
}
static void wl_connect_cb(lv_event_t*) { wl_connect(g_sel_ssid, wl_password()); }
static void wl_kb_cb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) wl_connect(g_sel_ssid, wl_password());
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL) wl_show_page(0);
}

static void wl_tick(lv_timer_t*) {
  if (g_scan_ready) { g_scan_ready = false; wl_rebuild_list(); }
  static char last[64] = "";
  if (strcmp(last, g_net_state) != 0) {
    strlcpy(last, g_net_state, sizeof(last));
    const bool bad = strstr(last, "failed") || strstr(last, "timeout");
    lv_obj_set_style_text_color(wl_status, bad ? lv_color_hex(0xFB7185) : lv_color_hex(0xB8C0E0), 0);
    lv_label_set_text(wl_status, last);
  }
  if (g_wl_count_at_connect >= 0 && g_ntp_count > g_wl_count_at_connect) {
    g_wl_count_at_connect = -1;
    lv_obj_set_style_text_color(wl_status, lv_color_hex(0x4ADE80), 0);
    lv_label_set_text(wl_status, LV_SYMBOL_OK "  Connected - time synced");
    g_wl_close_at = mono_ms() + 1800;
  }
  if (g_wl_close_at && mono_ms() >= g_wl_close_at) { g_wl_close_at = 0; wifi_close(); }
}

static void wifi_open() {
  if (g_wifi_open) return;
  g_wifi_open = true;
  g_wl_close_at = 0;
  const uint32_t acc = THEMES[g_theme].acc1;
  lv_obj_set_style_bg_color(wl_btn_connect, lv_color_hex(acc), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(wl_btn_connect, 0), lv_color_hex(THEMES[g_theme].bg_top), 0);
  lv_timer_pause(g_fast_timer);
  wl_show_page(0);
  wl_rebuild_list();
  lv_label_set_text(wl_status, g_net_state);
  lv_obj_remove_flag(wifi_ui, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(wifi_ui);
  lv_timer_resume(g_wifi_timer);
  lv_timer_resume(lv_indev_get_read_timer(g_indev));
  net_request(2);  // scan right away
}

static void wifi_build() {
  wifi_ui = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(wifi_ui);
  lv_obj_set_size(wifi_ui, SCR_W, SCR_H);
  lv_obj_set_style_bg_opa(wifi_ui, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(wifi_ui, lv_color_hex(0x0C1233), 0);
  lv_obj_remove_flag(wifi_ui, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(wifi_ui, LV_OBJ_FLAG_HIDDEN);

  mk_label(wifi_ui, f_date, 0xFFFFFF, LV_OPA_COVER, "WiFi", 40, 22, 300);
  wl_status = mk_label(wifi_ui, &lv_font_montserrat_20, 0xB8C0E0, LV_OPA_COVER, "", 40, 86, 800);
  lv_obj_t* a = wl_button(wifi_ui, "Scan", 860, 22, 170, 62, wl_a_cb, 0x2A3152, &wl_btn_a_lbl);
  (void)a;
  wl_button(wifi_ui, "Close", 1050, 22, 190, 62, wl_close_cb, 0x5B2A45);

  // page 0: network list
  wl_page_list = mk(wifi_ui, 0, 130, SCR_W, 590);
  wl_list = lv_obj_create(wl_page_list);
  lv_obj_remove_style_all(wl_list);
  lv_obj_set_pos(wl_list, 40, 0);
  lv_obj_set_size(wl_list, 1200, 570);
  lv_obj_set_flex_flow(wl_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wl_list, 10, 0);
  lv_obj_add_flag(wl_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(wl_list, LV_DIR_VER);

  // page 1: password entry
  wl_page_pw = mk(wifi_ui, 0, 110, SCR_W, 610);
  lv_obj_add_flag(wl_page_pw, LV_OBJ_FLAG_HIDDEN);
  wl_pw_title = mk_label(wl_page_pw, &lv_font_montserrat_28, 0xFFFFFF, LV_OPA_COVER, "", 40, 8, 1200);

  wl_ta = lv_textarea_create(wl_page_pw);
  lv_obj_set_pos(wl_ta, 40, 60);
  lv_obj_set_size(wl_ta, 790, 76);
  lv_textarea_set_one_line(wl_ta, true);
  lv_textarea_set_password_mode(wl_ta, true);
  lv_textarea_set_placeholder_text(wl_ta, "Password");
  lv_obj_add_event_cb(wl_ta, wl_ta_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_textarea_set_max_length(wl_ta, 63);
  lv_obj_set_style_text_font(wl_ta, &lv_font_montserrat_28, 0);
  lv_obj_set_style_bg_color(wl_ta, lv_color_hex(0x161C38), 0);
  lv_obj_set_style_bg_opa(wl_ta, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(wl_ta, lv_color_white(), 0);
  lv_obj_set_style_text_color(wl_ta, lv_color_hex(0x6B739A), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_radius(wl_ta, 18, 0);
  lv_obj_set_style_border_width(wl_ta, 2, 0);
  lv_obj_set_style_border_color(wl_ta, lv_color_hex(0x3A4470), 0);
  lv_obj_set_style_pad_hor(wl_ta, 22, 0);
  lv_obj_set_style_pad_ver(wl_ta, 16, 0);

  wl_button(wl_page_pw, "Show", 850, 60, 170, 76, wl_show_cb, 0x2A3152, &wl_btn_show_lbl);
  wl_btn_connect = wl_button(wl_page_pw, "Connect", 1040, 60, 200, 76, wl_connect_cb, 0x2EE6C5);

  wl_kb = lv_keyboard_create(wl_page_pw);
  lv_obj_set_size(wl_kb, 1200, 420);
  lv_obj_align(wl_kb, LV_ALIGN_TOP_LEFT, 40, 170);  // the keyboard defaults to bottom-centre alignment
  lv_obj_set_style_bg_color(wl_kb, lv_color_hex(0x0F1430), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(wl_kb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(wl_kb, 22, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wl_kb, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(wl_kb, 8, LV_PART_MAIN);
  lv_obj_set_style_text_font(wl_kb, &lv_font_montserrat_28, LV_PART_ITEMS);
  lv_obj_set_style_text_color(wl_kb, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(wl_kb, lv_color_hex(0x252C52), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(wl_kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_radius(wl_kb, 12, LV_PART_ITEMS);
  lv_obj_set_style_border_width(wl_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(wl_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(wl_kb, lv_color_hex(0x4B5AA8), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(wl_kb, lv_color_hex(0x3A4A9A), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(wl_kb, wl_kb_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(wl_kb, wl_kb_cb, LV_EVENT_CANCEL, nullptr);

  g_wifi_timer = lv_timer_create(wl_tick, 300, nullptr);
  lv_timer_pause(g_wifi_timer);
}

// ----------------------------------------------------------------------------------------------
// Battery details: tap the battery icon (top right) for a small card with the gauge readings; the next tap closes it.
// It sits clear of everything that is redrawn per frame, so while it is open it only costs its own 1 s refresh.
// ----------------------------------------------------------------------------------------------
static constexpr int BATT_MAH = 2000;          // the Tab5's NP-F550-type pack (2 cells in series), for the time estimate
static constexpr int BC_W = 380, BC_H = 296, BC_X = PX + PW - BC_W, BC_Y = 74;
static lv_obj_t *batt_ui, *batt_pct, *batt_state, *batt_bar, *batt_keys, *batt_vals, *batt_mode_lbl, *batt_limit_lbl;
static lv_timer_t* g_batt_timer = nullptr;
static bool  g_batt_open = false;
static float g_batt_ma = NAN;                  // smoothed current (+ = charging), for the time estimate

static void batt_refresh(lv_timer_t*) {
  const Theme& th = THEMES[g_theme];
  lv_label_set_text(batt_mode_lbl, g_usb_only ? "Battery display: USB only  (tap to change)" : "Battery display: Auto  (tap to change)");
  lv_label_set_text(batt_limit_lbl, g_charge_limit.enabled ? "Charge limit: 80-90%  (tap to change)" : "Charge limit: Off, charge to 100%  (tap to change)");
  sample_battery();
  const int mv = g_battery_mv, lvl = g_battery_level;
  const float ma = g_battery_ma;
  const bool chg = g_battery_charging;
  char b[128];
  if (g_battery_presence.state != BatteryPresence::Present) {
    lv_label_set_text(batt_pct, "--");
    const bool absent = g_battery_presence.state == BatteryPresence::Absent;
    lv_label_set_text(batt_state, absent ? "USB power" : "Checking battery");
    lv_obj_set_style_text_color(batt_state, lv_color_hex(th.ink), 0);
    lv_label_set_text(batt_keys, "Power source\nBattery\n\n");
    lv_bar_set_value(batt_bar, 0, LV_ANIM_OFF);
    lv_label_set_text(batt_vals, g_usb_only ? "USB\nNot fitted\n\n" : absent ? "USB\nNot detected\n\n" : "--\nChecking...\n\n");
    g_batt_ma = NAN;
    return;
  }
  g_batt_ma = isnan(g_batt_ma) ? ma : g_batt_ma + (ma - g_batt_ma) * 0.2f;   // the reading jumps with the CPU load

  snprintf(b, sizeof(b), "%d%%", lvl);
  lv_label_set_text(batt_pct, b);
  const char* st = chg ? "Charging" : g_battery_external ? (!g_charge_en ? "Resting at limit" : lvl >= 95 ? "Full" : "Not charging")
                 : "On battery";
  lv_label_set_text(batt_state, st);
  lv_obj_set_style_text_color(batt_state, lv_color_hex(chg ? th.acc1 : th.ink), 0);
  lv_bar_set_value(batt_bar, lvl, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(batt_bar, lv_color_hex(lvl <= 15 && !chg ? th.acc2 : th.acc1), LV_PART_INDICATOR);

  // Rough time estimate from the level and the smoothed current (charging tapers off near full, so it is optimistic)
  float hours = -1;
  if (chg && g_batt_ma > 50) hours = (100 - lvl) / 100.0f * BATT_MAH / g_batt_ma;
  else if (!g_battery_external && g_batt_ma < -30) hours = lvl / 100.0f * BATT_MAH / -g_batt_ma;
  char est[32] = "-";
  if (hours >= 0) {
    const int m = (int)(hours * 60 + 0.5f);
    snprintf(est, sizeof(est), "~%d h %02d min", m / 60, m % 60);
  }
  lv_label_set_text(batt_keys, chg ? "Voltage\nCurrent\nPower\nFull in" : "Voltage\nCurrent\nPower\nTime left");
  snprintf(b, sizeof(b), "%.2f V  (%.2f V/cell)\n%+d mA\n%.2f W\n%s", mv / 1000.0f, mv / 2000.0f, (int)lroundf(ma),
           fabsf(mv * ma) / 1e6f, est);
  lv_label_set_text(batt_vals, b);
}

static void batt_build() {
  batt_ui = mk(lv_layer_top(), BC_X, BC_Y, BC_W, BC_H);
  lv_obj_set_style_radius(batt_ui, 24, 0);
  lv_obj_set_style_bg_opa(batt_ui, 250, 0);   // nearly opaque: the big digits would ghost through
  lv_obj_set_style_border_width(batt_ui, 1, 0);
  lv_obj_set_style_border_opa(batt_ui, 50, 0);
  lv_obj_add_flag(batt_ui, LV_OBJ_FLAG_HIDDEN);
  batt_pct = mk_label(batt_ui, f_date, 0xFFFFFF, LV_OPA_COVER, "", 24, 14, 140);
  batt_state = mk_label(batt_ui, f_small, 0xFFFFFF, LV_OPA_COVER, "", 150, 28, BC_W - 174, LV_TEXT_ALIGN_RIGHT);
  batt_bar = lv_bar_create(batt_ui);
  lv_obj_remove_style_all(batt_bar);
  lv_obj_set_pos(batt_bar, 24, 74);
  lv_obj_set_size(batt_bar, BC_W - 48, 8);
  lv_bar_set_range(batt_bar, 0, 100);
  lv_obj_set_style_radius(batt_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(batt_bar, 40, LV_PART_MAIN);
  lv_obj_set_style_radius(batt_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(batt_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  batt_keys = mk_label(batt_ui, f_small, 0xFFFFFF, 150, "", 24, 96, 140);
  batt_vals = mk_label(batt_ui, f_small, 0xFFFFFF, 235, "", 120, 96, BC_W - 144, LV_TEXT_ALIGN_RIGHT);
  batt_mode_lbl = mk_label(batt_ui, f_tiny, 0xFFFFFF, 180, "", 24, 234, BC_W - 48, LV_TEXT_ALIGN_CENTER);
  batt_limit_lbl = mk_label(batt_ui, f_tiny, 0xFFFFFF, 180, "", 24, 264, BC_W - 48, LV_TEXT_ALIGN_CENTER);
  g_batt_timer = lv_timer_create(batt_refresh, 1000, nullptr);
  lv_timer_pause(g_batt_timer);
}

static void batt_open() {
  if (g_batt_open) return;
  g_batt_open = true;
  const Theme& th = THEMES[g_theme];
  lv_obj_set_style_bg_color(batt_ui, lv_color_hex(th.bg_top), 0);
  lv_obj_set_style_border_color(batt_ui, lv_color_hex(th.ink), 0);
  for (lv_obj_t* l : {batt_pct, batt_keys, batt_vals}) lv_obj_set_style_text_color(l, lv_color_hex(th.ink), 0);
  lv_obj_set_style_bg_color(batt_bar, lv_color_hex(th.ink), LV_PART_MAIN);
  g_batt_ma = NAN;
  batt_refresh(nullptr);
  lv_obj_remove_flag(batt_ui, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(batt_ui);
  lv_timer_resume(g_batt_timer);
}

static void set_battery_mode(bool usb_only) {
  g_usb_only = usb_only;
  prefs.putBool("batt_usb", usb_only);
  g_battery_presence = BatteryPresence();
  g_battery_sample_at = -1000;
  update_battery();
  if (g_batt_open) batt_refresh(nullptr);
}

static void set_charge_limit(bool on) {
  g_charge_limit.enabled = on;
  prefs.putBool("chg_lim", on);
  apply_charge_limit();
  if (g_batt_open) batt_refresh(nullptr);
}

static void batt_close() {
  if (!g_batt_open) return;
  g_batt_open = false;
  lv_obj_add_flag(batt_ui, LV_OBJ_FLAG_HIDDEN);
  lv_timer_pause(g_batt_timer);
}

// ----------------------------------------------------------------------------------------------
// Auto-rotation between landscape and landscape upside-down, from the accelerometer.  Lying flat, standing on a
// short side or being moved around never turns the screen, and a new orientation has to be held for ROT_HOLD_MS.
// Touch coordinates follow M5.Display's rotation; LVGL keeps drawing in landscape and only the PPA flush (and the
// frame-buffer screenshot) map to the panel differently, so a turn is just one full redraw.
// ----------------------------------------------------------------------------------------------
static constexpr int   ORIENT_POLL_MS = 200;
static constexpr int   ORIENT_AXIS = 0;        // accelerometer axis along the screen's short side
static constexpr float ORIENT_SIGN = -1;       // gravity along +axis (times this) means rotation 1 is upright
                                               // (on the device: upright in rotation 1 reads x = -1.0 g)
static constexpr float ORIENT_MIN_G = 0.5f;    // in-plane gravity needed, in g (~30 degrees of tilt from flat)
static bool g_imu_ok = false;

static void imu_setup() {
  g_imu_ok = M5.Imu.isEnabled();
  if (!g_imu_ok) { Serial.println("WARNING: no IMU, auto-rotation off"); return; }
  // Only the accelerometer is needed; M5Unified switches the gyro (~0.9 mA) on too, so turn it off again.
  for (uint8_t a : {0x68, 0x69})
    if (M5.In_I2C.readRegister8(a, 0x00, 400000) == 0x24)   // BMI270 CHIP_ID
      M5.In_I2C.writeRegister8(a, 0x7D, 0x04, 400000);       // PWR_CTRL: accelerometer only
}

// The rotation the accelerometer asks for: 1 or 3, or 0 if undecided.
static int orient_sample() {
  if (!g_imu_ok) return 0;
  float a[3];
  M5.Imu.getAccel(&a[0], &a[1], &a[2]);
  const float g = ORIENT_SIGN * a[ORIENT_AXIS], side = fabsf(a[1 - ORIENT_AXIS]);
  const float mag2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
  if (mag2 < 0.7f * 0.7f || mag2 > 1.3f * 1.3f) return 0;   // being moved
  if (fabsf(g) < ORIENT_MIN_G || fabsf(g) < 1.5f * side) return 0;
  return g > 0 ? 1 : 3;
}

static void set_rotation(int r) {
  if (r == g_rot) return;
  g_rot = r;
  M5.Display.setRotation(r);
  lv_obj_invalidate(lv_screen_active());   // the whole screen, all layers
  Serial.printf("rotation %d\n", r);
}

// Called every loop pass while the display is on; `now` (display wake-up) turns at once if the answer is clear.
static void orient_tick(bool now) {
  static int64_t next = 0, cand_since = 0;
  static int cand = 0;
  const int64_t t = mono_ms();
  if (!g_imu_ok || (!now && t < next)) return;
  next = t + ORIENT_POLL_MS;
  const int r = orient_sample();
  if (r == 0 || r == g_rot) { cand = 0; return; }
  if (r != cand) { cand = r; cand_since = t; }
  if ((now || t - cand_since >= ROT_HOLD_MS) && !M5.Touch.getCount()) { cand = 0; set_rotation(r); }
}

// ----------------------------------------------------------------------------------------------
// Display power & brightness: swipe left/right to dim/brighten, double-tap to turn the display off/on.
// ----------------------------------------------------------------------------------------------
static constexpr int MIN_BRIGHTNESS = 8;   // never fully black by swiping, so the screen can't be "lost"
static uint8_t g_bri = BRIGHTNESS;
static int64_t g_bri_hide_at = 0;
static lv_obj_t *bri_ui, *bri_lbl, *bri_bar;

static void bri_build() {
  bri_ui = mk(lv_layer_top(), (SCR_W - 460) / 2, SCR_H - 96 - 40, 460, 96);
  lv_obj_set_style_radius(bri_ui, 30, 0);
  lv_obj_set_style_bg_color(bri_ui, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(bri_ui, LV_OPA_70, 0);
  lv_obj_set_style_border_width(bri_ui, 1, 0);
  lv_obj_set_style_border_color(bri_ui, lv_color_white(), 0);
  lv_obj_set_style_border_opa(bri_ui, LV_OPA_30, 0);
  lv_obj_add_flag(bri_ui, LV_OBJ_FLAG_HIDDEN);
  bri_lbl = mk_label(bri_ui, &lv_font_montserrat_20, 0xFFFFFF, LV_OPA_COVER, "", 0, 18, 460, LV_TEXT_ALIGN_CENTER);
  bri_bar = lv_bar_create(bri_ui);
  lv_obj_remove_style_all(bri_bar);
  lv_obj_set_pos(bri_bar, 40, 60);
  lv_obj_set_size(bri_bar, 380, 10);
  lv_bar_set_range(bri_bar, MIN_BRIGHTNESS, 255);
  lv_obj_set_style_radius(bri_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bri_bar, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bri_bar, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_radius(bri_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bri_bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

static void set_brightness(int v, bool show) {
  v = constrain(v, MIN_BRIGHTNESS, 255);
  const bool changed = v != g_bri;
  g_bri = (uint8_t)v;
  if (g_disp_on) M5.Display.setBrightness(g_bri);
  if (show && (changed || lv_obj_has_flag(bri_ui, LV_OBJ_FLAG_HIDDEN))) {
    char b[32];
    snprintf(b, sizeof(b), "Brightness  %d%%", (g_bri * 100 + 127) / 255);
    lv_label_set_text(bri_lbl, b);
    lv_obj_set_style_bg_color(bri_bar, lv_color_hex(THEMES[g_theme].acc1), LV_PART_INDICATOR);
    lv_bar_set_value(bri_bar, g_bri, LV_ANIM_OFF);
    lv_obj_remove_flag(bri_ui, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bri_ui);
    g_bri_hide_at = 0;  // stays up while adjusting; hidden shortly after the finger lifts
  }
}

static void bri_tick() {
  if (g_bri_hide_at && mono_ms() >= g_bri_hide_at) {
    g_bri_hide_at = 0;
    lv_obj_add_flag(bri_ui, LV_OBJ_FLAG_HIDDEN);
  }
}

// "Off" = the lowest-power state that still keeps touch, serial and the clock's time alive:
//   backlight 0, no LVGL work, no automatic WiFi syncs, and loop() polls touch at ~30 Hz (see loop()).
//   The RTC/NTP-derived time keeps running on the crystal timer.
// NOTE 1: the panel must NOT be put into DSI sleep (M5.Display.sleep()): the Tab5's touch controller lives in
//   the same chip as the display driver and stops reporting touches, so nothing could wake it again.
// NOTE 2: lowering the CPU clock is NOT possible - the P4 only offers 360 or 40 MHz, and at 40 MHz the
//   MIPI-DSI controller (which keeps streaming from PSRAM) underruns and the chip resets.
static void display_set(bool on) {
  if (on == g_disp_on) return;
  g_disp_on = on;
  if (on) {
    // Bring the picture up to date *before* the backlight comes on, so the second hand (and time, battery,
    // ...) don't visibly jump from their stale positions.
    lv_timer_resume(g_fast_timer);
    g_dirty_all = true;
    orient_tick(true);              // it may have been turned round while dark
    update_battery();
    lv_obj_invalidate(lv_screen_active());
    lv_timer_ready(g_fast_timer);   // run the clock update on the next handler pass
    lv_timer_handler();
    lv_refr_now(g_disp);            // render + flush everything now
    M5.Display.setBrightness(g_bri);
  } else {
    batt_close();
    lv_timer_pause(g_fast_timer);
    M5.Display.setBrightness(0);
  }
  Serial.printf("display %s\n", on ? "on" : "off");
}

// ----------------------------------------------------------------------------------------------
// Serial console (time sync, screenshots, debugging)
// ----------------------------------------------------------------------------------------------
#include "serial_console.h"

// LVGL pointer input for the WiFi screen (the clock face itself is handled by handle_touch below)
static void touch_read_cb(lv_indev_t*, lv_indev_data_t* d) {
  static int16_t lx = 0, ly = 0;
  const auto t = M5.Touch.getDetail();
  if (t.isPressed()) { lx = t.x; ly = t.y; d->state = LV_INDEV_STATE_PRESSED; }
  else d->state = LV_INDEV_STATE_RELEASED;
  d->point.x = lx;
  d->point.y = ly;
}

// A confirmed single tap on the clock face.
static void run_tap(int x, int y) {
  if (g_batt_open) {
    if (x >= BC_X && x < BC_X + BC_W && y >= BC_Y + 220 && y < BC_Y + 252)
      set_battery_mode(!g_usb_only);
    else if (x >= BC_X && x < BC_X + BC_W && y >= BC_Y + 252 && y < BC_Y + BC_H)
      set_charge_limit(!g_charge_limit.enabled);
    else batt_close();
  } else if (x >= PX + PW - 128 && y < 90) {        // battery icon (top right)
    batt_open();
  } else if (x >= PX + PW - 260 && y < 90) {        // WiFi icon, left of it
    wifi_open();
  } else if (x >= PX && y < 250) {                  // the big digits
    g_24h = !g_24h;
    prefs.putBool("h24", g_24h);
    g_dirty_all = true;
  } else {
    set_theme(g_theme + 1);
  }
}

// Gestures on the clock face (the WiFi screen is driven by LVGL's own input device instead):
//   horizontal drag  -> brightness      double tap -> display off/on      single tap -> run_tap()
// A single tap is only acted on after the double-tap window has passed without a second tap.
static void handle_touch() {
  static constexpr int64_t DOUBLE_TAP_MS = 350;
  static bool down = false, adjusting = false, pending = false;
  static int sx, sy, lx, ly, px, py;
  static int64_t t_down = 0, pend_at = 0, ignore_until = 0;
  static uint8_t bri0 = 0;

  const int64_t now = mono_ms();
  const auto t = M5.Touch.getDetail();
  const bool pressed = t.isPressed();

  if (g_wifi_open) { down = adjusting = pending = false; return; }

  if (pressed && !down) {                           // finger down
    down = true; adjusting = false;
    sx = lx = t.x; sy = ly = t.y;
    t_down = now; bri0 = g_bri;
  } else if (pressed) {                             // finger moving
    lx = t.x; ly = t.y;
    const int dx = lx - sx, dy = ly - sy;
    if (!adjusting && g_disp_on && abs(dx) > 30 && abs(dx) > 2 * abs(dy)) adjusting = true;
    if (adjusting) set_brightness(bri0 + dx * 255 / 800, true);  // ~800 px of travel = full range
  } else if (down) {                                // finger up
    down = false;
    if (adjusting) {
      adjusting = false;
      prefs.putUChar("bri", g_bri);
      g_bri_hide_at = now + 900;
      Serial.printf("brightness %d (%d%%)\n", g_bri, (g_bri * 100 + 127) / 255);
    } else if (abs(lx - sx) <= 40 && abs(ly - sy) <= 40 && now - t_down < 600 && now >= ignore_until) {
      if (!g_disp_on) {                             // any tap wakes a sleeping display
        display_set(true);
        pending = false;
        ignore_until = now + 600;                   // swallow the second tap of a wake double-tap
      } else if (pending && now - pend_at <= DOUBLE_TAP_MS) {
        pending = false;
        display_set(false);
        ignore_until = now + 500;
      } else {
        pending = true; px = lx; py = ly; pend_at = now;
      }
    }
  }

  if (pending && now - pend_at > DOUBLE_TAP_MS) {
    pending = false;
    run_tap(px, py);
  }
}

// ----------------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  serial_logging_setup();
  Serial.setTxTimeoutMs(0);  // never block the UI when nobody is reading the USB port
  auto cfg = M5.config();
  cfg.output_power = false;   // don't power the 5 V external/USB-host outputs - nothing is plugged in there
  cfg.internal_imu = true;    // accelerometer for auto-rotation (the gyro is switched off again in imu_setup)
  cfg.internal_mic = false;   // the clock uses neither the microphone nor the speaker/audio codec
  cfg.internal_spk = false;
  M5.begin(cfg);
  imu_setup();
  { const int r = orient_sample(); g_rot = r ? r : ROTATION; }   // start the right way up
  M5.Display.setRotation(g_rot);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.display();
  ppa_setup();
  Serial.printf("\nFancy clock: display %dx%d, PSRAM %u bytes free\n", (int)M5.Display.width(), (int)M5.Display.height(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  prefs.begin("clock", false);
  g_theme = prefs.getInt("theme", 0) % N_THEMES;
  g_24h = prefs.getBool("h24", true);
  g_usb_only = prefs.getBool("batt_usb", false);
  g_charge_limit.enabled = prefs.getBool("chg_lim", true);
  g_bri = (uint8_t)constrain((int)prefs.getUChar("bri", BRIGHTNESS), MIN_BRIGHTNESS, 255);
  M5.Display.setBrightness(g_bri);

  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
  g_tz = prefs.getString("tz", g_tz);
  setenv("TZ", g_tz.c_str(), 1);
  tzset();
  if (!g_ssid.isEmpty()) snprintf(g_net_state, sizeof(g_net_state), "waiting to sync");
  init_clock(prefs);
  net_setup();

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000); });
  lv_log_register_print_cb(log_cb);

  g_disp = lv_display_create(SCR_W, SCR_H);
  lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(g_disp, flush_cb);
  const size_t buf_px = SCR_W * 64;
  void* b1 = alloc_draw_buf(buf_px * 2);
  void* b2 = alloc_draw_buf(buf_px * 2);
  lv_display_set_buffers(g_disp, b1, b2, buf_px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);

  f_digits = lv_tiny_ttf_create_data(font_light_data, font_light_size, 180);
  f_sec    = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 34);
  f_date   = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 40);
  f_head   = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 28);
  f_num    = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 46);
  f_cal    = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 24);
  f_small  = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 22);
  f_tiny   = lv_tiny_ttf_create_data(font_medium_data, font_medium_size, 16);

  build_dynamic_ui();
  wifi_build();
  bri_build();
  batt_build();
  g_indev = lv_indev_create();
  lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_indev, touch_read_cb);
  lv_timer_set_period(lv_indev_get_read_timer(g_indev), 16);
  lv_timer_pause(lv_indev_get_read_timer(g_indev));   // resumed while the WiFi screen is open
  set_theme(g_theme, false);                          // the other themes are rendered in the background later
  xTaskCreatePinnedToCore(paint_task, "paint", 4096, (void*)(intptr_t)g_theme, 0, nullptr, 0);
  update_battery();
  g_fast_timer = lv_timer_create(fast_cb, FRAME_MS, nullptr);
  lv_timer_create([](lv_timer_t*) { update_battery(); }, 500, nullptr);
  // the fuel gauge can report 0% right after power-up, so read it again shortly after boot
  lv_timer_set_repeat_count(lv_timer_create([](lv_timer_t*) { update_battery(); }, 3000, nullptr), 1);
  Serial.printf("UI ready, heap int=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
  if (!g_disp_on) {          // display off: only touch (to wake) and serial; nothing else runs
    M5.update();
    handle_touch();
    handle_serial();
    net_apply();
    const bool rtc_busy = poll_rtc();
    rtc_backup_write();
    delay(rtc_busy || g_rtc_write_utc_s ? 4 : 30);
    return;
  }
  int64_t t = esp_timer_get_time();
  const int64_t t_loop = t;
  M5.update();          stat_add(st_upd, t); t = esp_timer_get_time();
  handle_touch();       stat_add(st_tap, t); t = esp_timer_get_time();
  const bool rtc_busy = poll_rtc(); stat_add(st_rtc, t); t = esp_timer_get_time();
  handle_serial();      stat_add(st_ser, t); t = esp_timer_get_time();
  bri_tick();
  orient_tick(false);
  net_apply();
  rtc_backup_write();
  {
    static int shown_count = 0;
    if (g_ntp_count != shown_count) { shown_count = g_ntp_count; update_battery(); }
  }
  if (g_auto_left > 0 && mono_ms() >= g_auto_next) {
    g_auto_left--;
    g_auto_next = mono_ms() + 3000;
    set_theme(g_theme + 1, false);
  }
  const uint32_t idle_ms = lv_timer_handler();   stat_add(st_lv, t);
  {
    const int pos = now_local().s / 5;
    g_frames_by_pos[pos]++;
    if (esp_timer_get_time() - t > 40000) g_slow_by_pos[pos]++;
  }
  if (!g_wifi_open) render_pending_themes();
  g_busy_us += esp_timer_get_time() - t_loop;
  // Sleep until LVGL's next timer is due (the idle task halts the CPU meanwhile), but wake every TOUCH_MS to
  // poll the touch panel, and every 4 ms while phase-locking to the RTC.
  uint32_t ms = min<uint32_t>(idle_ms, (rtc_busy || g_rtc_write_utc_s) ? 4 : TOUCH_MS);
  delay(ms ? ms : 1);
}
