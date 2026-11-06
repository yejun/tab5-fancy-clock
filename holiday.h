#pragma once
// The HOLIDAY theme's artwork (Theme::scene == 1), painted per pixel into the static background with the rest of it,
// so none of it costs anything once the theme is drawn:
//  * a winter night: stars at the top, a far snowy ridge with small misty pines, near pines with snow on their
//    branches and a snow drift along the bottom (much of it shows blurred through the calendar card's glass);
//  * on the dial a gold star at 12 o'clock and snowflakes for the other hour marks (dial_deco_cb then skips the bars);
//  * the hub as a red glass bauble (holiday_hub, from make_sprites).
// Runs on either core (paint_task paints the other themes on core 0): no LVGL, no globals written, and no newlib
// float trig per pixel (it goes through software double precision on the P4) - the flakes' angles are all multiples
// of 30 degrees, read from a table.

static constexpr float HOL_SIN30[12] = {0, 0.5f, 0.8660254f, 1, 0.8660254f, 0.5f, 0, -0.5f, -0.8660254f, -1, -0.8660254f, -0.5f};
static inline float hol_sin(int k) { return HOL_SIN30[((k % 12) + 12) % 12]; }   // sin(k * 30 degrees)
static inline float hol_cos(int k) { return hol_sin(k + 3); }

static inline uint32_t hol_hash(uint32_t x) {   // a small integer hash (lowbias32)
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// Smooth bumpy lines without sinf: alternating parabolic humps, a cosine-like wave with a continuous slope
static inline float hol_wave(float x) {   // period 4, range -1..1
  const float t = x - 4 * floorf(x * 0.25f);   // 0..4
  const float u = t < 2 ? t - 1 : t - 3;       // -1..1 within each half
  return (t < 2 ? 1 : -1) * (1 - u * u);
}

// Snow surface heights: near drift (the bottom) and the far ridge behind the trees
static inline float hol_ground(float x) { return SCR_H - 52 + 6 * hol_wave(x * 0.0060f + 0.4f) + 3 * hol_wave(x * 0.0170f + 1.3f); }
static inline float hol_ridge(float x) { return SCR_H - 112 + 16 * hol_wave(x * 0.0035f + 2.2f) + 6 * hol_wave(x * 0.0110f + 0.7f); }

struct HolPine { float x, h, w, fog; bool far; };
// Near trees stand in the drift, far ones on the ridge. The dial (x < ~690) hides the middle of the left side, so
// the trees there sit in its corners; the ones on the right show through the calendar card's frosted glass.
static const HolPine HOL_PINES[] = {
  // far, small and misty
  {560, 60, 34, 0.55f, true}, {640, 74, 40, 0.55f, true}, {790, 66, 36, 0.55f, true}, {905, 82, 44, 0.55f, true},
  {1010, 62, 34, 0.55f, true}, {1185, 78, 42, 0.55f, true}, {1265, 64, 36, 0.55f, true}, {40, 70, 38, 0.55f, true},
  // near
  {22, 200, 96, 0.0f, false}, {108, 132, 68, 0.12f, false}, {700, 214, 100, 0.0f, false},
  {858, 168, 84, 0.08f, false}, {1080, 236, 112, 0.0f, false}, {1236, 176, 88, 0.05f, false},
};
static constexpr float HOL_SCENE_TOP = SCR_H - 300;   // nothing of the landscape above this

static const float HOL_PINE_COL[3] = {0.035f, 0.090f, 0.085f};   // dark blue-green
static const float HOL_SNOW_COL[3] = {0.74f, 0.80f, 0.90f};
static const float HOL_TRUNK_COL[3] = {0.10f, 0.065f, 0.05f};

// One pine: 5 stacked tiers (each a cone with up-turned tips and snow along its upper edges) and a trunk.
static HOT void hol_pine(const HolPine& p, float base, float x, float y, float* c) {
  const float dx = fabsf(x - p.x), top = base - p.h;
  if (dx > p.w * 0.5f + 1 || y < top - 1 || y > base + 1) return;
  constexpr int N = 5;
  float col[3];
  for (int k = 0; k < 3; k++) col[k] = HOL_PINE_COL[k] + (c[k] - HOL_PINE_COL[k]) * p.fog;   // mist: towards the sky
  float snow[3];
  for (int k = 0; k < 3; k++) snow[k] = HOL_SNOW_COL[k] * (1 - 0.45f * p.fog) + c[k] * 0.45f * p.fog;
  // trunk first: the tiers cover it
  const float tw = p.w * 0.07f;
  if (dx < tw + 0.5f && y > base - p.h * 0.2f) {
    float tc[3];
    for (int k = 0; k < 3; k++) tc[k] = HOL_TRUNK_COL[k] + (c[k] - HOL_TRUNK_COL[k]) * p.fog;
    mix_to(c, tc, clamp01(tw + 0.5f - dx));
  }
  for (int i = N - 1; i >= 0; i--) {   // lowest (widest) tier first, so the ones above overlap it
    const float th = p.h * 0.34f, yt = top + i * (p.h * 0.80f - th) / (N - 1) + (i ? p.h * 0.04f : 0);
    const float yb = yt + th, hwb = p.w * 0.5f * (0.38f + 0.62f * i / (N - 1));
    if (y < yt - 1 || y > yb + 1 || dx > hwb + 1) continue;
    const float k = th / sqrtf(th * th + hwb * hwb);   // horizontal -> perpendicular distance to the slanted edge
    const float side = (hwb * (y - yt) / th - dx) * k;
    const float u = dx / hwb, ybe = yb - th * 0.22f * u * u;   // the bottom edge curls up towards the tips
    const float cov = clamp01(side + 0.5f) * clamp01(ybe - y + 0.5f);
    if (cov <= 0) continue;
    // snow lying on the branches: along the upper edges, thicker towards the tier's tips, in uneven clumps
    const float v = (y - yt) / th, thick = (0.6f + 4.2f * v * v) * (0.75f + 0.35f * hol_wave(y * 0.21f + p.x * 0.37f));
    const float s = 0.85f * clamp01(thick - side + 0.5f) * clamp01((v - 0.12f) * 5);
    float t[3];
    for (int j = 0; j < 3; j++) t[j] = col[j] + (snow[j] * (0.80f + 0.20f * v) - col[j]) * s;
    mix_to(c, t, cov);
  }
}

static HOT void hol_scene_at(float x, float y, float* c) {
  if (y < 340) {   // stars, fading out towards the horizon
    constexpr int CELL = 26;
    const int cx = (int)(x * (1.0f / CELL)), cy = (int)(y * (1.0f / CELL));
    const uint32_t h = hol_hash((uint32_t)(cx * 7919 + cy * 104729 + 17));
    if ((h & 255) < 70) {
      const float sx = cx * CELL + 5 + ((h >> 8) & 15), sy = cy * CELL + 5 + ((h >> 12) & 15);
      const float b = (h >> 16) & 255, bright = 0.25f + 0.75f * (b / 255) * (b / 255);
      const float sig = 0.55f + 0.45f * ((h >> 24) & 255) / 255, dx = x - sx, dy = y - sy;
      const float g = gauss((dx * dx + dy * dy) / (sig * sig));
      if (g > 0) {
        const float w[3] = {0.92f, 0.95f, 1.0f};
        screen_add(c, w, bright * g * clamp01((340 - y) / 160));
      }
    }
  }
  if (y < HOL_SCENE_TOP) return;
  // far ridge, then the far pines on it
  const float ridge = hol_ridge(x);
  if (y > ridge - 1) {
    const float far[3] = {0.30f, 0.38f, 0.52f};
    float f[3];
    for (int k = 0; k < 3; k++) f[k] = c[k] + (far[k] - c[k]) * 0.75f;
    mix_to(c, f, clamp01(y - ridge + 0.5f));
  }
  for (const HolPine& p : HOL_PINES)
    if (p.far) hol_pine(p, hol_ridge(p.x) + 6, x, y, c);
  for (const HolPine& p : HOL_PINES)
    if (!p.far) hol_pine(p, hol_ground(p.x) + 8, x, y, c);
  // near drift: a lit top edge, deeper blue further down
  const float g = hol_ground(x);
  if (y > g - 1) {
    const float deep[3] = {0.42f, 0.50f, 0.66f};
    const float t = clamp01((y - g) / 40);
    float s[3];
    for (int k = 0; k < 3; k++) s[k] = HOL_SNOW_COL[k] * 0.92f + (deep[k] - HOL_SNOW_COL[k] * 0.92f) * t;
    mix_to(c, s, clamp01(y - g + 0.5f));
  }
}

// ---- Dial marks: a gold star at 12, snowflakes at the other hours ----
static constexpr float HOL_MARK_R = 268;   // where the hour marks sit (the bars' middle)
static constexpr float HOL_FLAKE_R = 14;

static inline float hol_seg(float px, float py, float ax, float ay, float bx, float by) {   // distance to a segment
  const float dx = bx - ax, dy = by - ay, t = clamp01(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy));
  const float ex = px - ax - t * dx, ey = py - ay - t * dy;
  return sqrtf(ex * ex + ey * ey);
}

// Signed distance to a 5-pointed star (Inigo Quilez's sdStar5), y up; r: outer radius, rf: inner/outer ratio
static inline float hol_star(float px, float py, float r, float rf) {
  const float k1x = 0.809016994f, k1y = -0.587785252f, k2x = -k1x, k2y = k1y;
  px = fabsf(px);
  float d = fmaxf(k1x * px + k1y * py, 0.0f);
  px -= 2 * d * k1x; py -= 2 * d * k1y;
  d = fmaxf(k2x * px + k2y * py, 0.0f);
  px -= 2 * d * k2x; py -= 2 * d * k2y;
  px = fabsf(px);
  py -= r;
  const float bax = rf * -k1y, bay = rf * k1x - 1;
  const float h = clamp01((px * bax + py * bay) / (bax * bax + bay * bay) / r) * r;
  const float ex = px - bax * h, ey = py - bay * h, len = sqrtf(ex * ex + ey * ey);
  return py * bax - px * bay > 0 ? len : -len;
}

static HOT void hol_marks_at(const DialCols& dc, float x, float y, float d, float* c) {
  if (d < HOL_MARK_R - 30 || d > HOL_MARK_R + 30) return;
  const float dx = x - CX, dy = y - CY;
  // the nearest hour, from the angle (0 at 12, clockwise)
  float a = fast_atan2f(dx, -dy) * (6 / (float)M_PI);
  if (a < 0) a += 12;
  const int i = ((int)(a + 0.5f)) % 12;
  const float mx = CX + hol_sin(i) * HOL_MARK_R, my = CY - hol_cos(i) * HOL_MARK_R;
  const float px = x - mx, py = y - my;
  if (fabsf(px) > 30 || fabsf(py) > 30) return;
  if (i == 0) {   // star: soft gold glow, gold body lit from the top, a bright rim
    const float sd = hol_star(px, -py + 1.5f, 21, 0.45f);
    screen_add(c, dc.acc1, 0.30f * gauss(fmaxf(0, sd) * fmaxf(0, sd) / 36));
    const float cov = clamp01(0.5f - sd);
    if (cov > 0) {
      float body[3];
      const float lit = 0.80f + 0.30f * clamp01((10 - py) / 30);
      for (int k = 0; k < 3; k++) body[k] = fminf(1, dc.acc1[k] * lit);
      mix_to(c, body, cov);
      mix_to(c, dc.ink, 0.55f * cov * clamp01(1.6f - fabsf(sd + 1.0f)));
    }
    return;
  }
  // snowflake: 6 arms (one pointing out from the centre), each with a pair of short branches
  float dmin = 1e9f;
  for (int j = 0; j < 6; j++) {
    const int k = i + 2 * j;                       // arm direction in 30-degree steps
    const float ux = hol_sin(k), uy = -hol_cos(k);
    dmin = fminf(dmin, hol_seg(px, py, 0, 0, ux * HOL_FLAKE_R, uy * HOL_FLAKE_R));
    const float bx = ux * HOL_FLAKE_R * 0.56f, by = uy * HOL_FLAKE_R * 0.56f;
    for (int s = -2; s <= 2; s += 4) {             // branches at +-60 degrees
      const float vx = hol_sin(k + s), vy = -hol_cos(k + s);
      dmin = fminf(dmin, hol_seg(px, py, bx, by, bx + vx * HOL_FLAKE_R * 0.36f, by + vy * HOL_FLAKE_R * 0.36f));
    }
  }
  screen_add(c, dc.acc1, 0.16f * gauss(dmin * dmin / 30));   // the faint glow the bars had
  mix_to(c, dc.ink, 0.92f * clamp01(1.4f - dmin));           // 1.8 px strokes, antialiased
}

// Hub: a red glass bauble with a gold rim, lit from the top left; dx, dy from its centre
static Rgba holiday_hub(const Theme& th, float dx, float dy) {
  const float d = sqrtf(dx * dx + dy * dy);
  const float sx = dx - 2, sy = dy - 6, sd = sqrtf(sx * sx + sy * sy);
  Rgba p = {0, 0, 0, 0};
  over(p, rgba(0x000000, 0.50f * (1 - smooth(12, 27, sd))));
  const float r = 17.5f, nz2 = 1 - (dx * dx + dy * dy) / (r * r);
  if (nz2 > -0.2f) {
    const float nz = sqrtf(fmaxf(0, nz2)), nx = dx / r, ny = dy / r;
    const float lam = clamp01(-0.45f * nx - 0.55f * ny + 0.70f * nz);   // light from the top left, towards the viewer
    Rgba ball = rgba(th.acc2, disc_cov(d, r));
    const float k = 0.35f + 0.75f * lam;
    ball.r = fminf(1, ball.r * k); ball.g = fminf(1, ball.g * k); ball.b = fminf(1, ball.b * k);
    over(p, ball);
    const float hx = dx + 6, hy = dy + 7;   // specular highlight and a soft reflected light at the bottom right
    over(p, rgba(0xFFFFFF, 0.85f * expf(-(hx * hx + hy * hy) / 9.0f)));
    over(p, rgba(0xFFFFFF, 0.18f * ring_cov(d, r - 4, r - 1.5f) * clamp01((dx + dy) / 20)));
    over(p, rgba(th.acc1, ring_cov(d, r - 1.4f, r + 0.3f)));   // gold rim
  }
  return p;
}
