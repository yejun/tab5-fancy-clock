// The custom blend hooks declared in lv_blend.h. They give exactly LVGL's results (checked by comparing whole
// frames on the PC), but mix all three channels of an RGB565 pixel at once, skip transparent pixels, and keep
// the mix inline where LVGL calls lv_color_16_16_mix per pixel. Translucent fills (flap cards, panels) and
// sprites (second hand, numerals) were 30-60% of a show frame.
//
// Translucent fills and additive sprites (the numerals' glow, fireworks, sweep) also have PIE versions, 8 pixels a
// step, used once clock_blend_check() has found them equal to the C versions (see pie.h; serial "R").
#include <lvgl.h>
#include <draw/sw/blend/lv_draw_sw_blend_private.h>
#include <esp_heap_caps.h>
#include <esp_cpu.h>
#include <string.h>
#include "lv_blend.h"

#define HOT __attribute__((optimize("O2")))
#define INLINE static inline __attribute__((always_inline))
// RGB565 spread out as 0b00000ggggggg00000rrrrr000000bbbbb so one multiply mixes all three channels
#define PREP(c) (((uint32_t)(c) | (uint32_t)(c) << 16) & 0x07E0F81Fu)

#if defined(__riscv_xesppie)
#define BLEND_PIE 1
#else
#define BLEND_PIE 0
#endif
static int g_blend_pie = 0;   // 1: rows use the PIE versions

// fg: PREP'd colour, mix5: weight 0-32 (LVGL's (opa + 4) >> 3)
INLINE uint16_t mix_prep(uint32_t fg, uint16_t d, uint32_t mix5) {
  const uint32_t bg = PREP(d), r = ((((fg - bg) * mix5) >> 5) + bg) & 0x07E0F81Fu;
  return (uint16_t)(r >> 16 | r);
}
INLINE uint16_t c24_16(const uint8_t* c) {
  return (uint16_t)((c[2] & 0xF8) << 8 | (c[1] & 0xFC) << 3 | c[0] >> 3);
}

// ---- the rows in C ----
INLINE void fill_row_c(uint16_t* p, int w, uint32_t fg, uint32_t mix5) {
  for (int x = 0; x < w; x++) p[x] = mix_prep(fg, p[x], mix5);
}
// The same as the additive path of patches/lvgl-*-additive-rgb565.patch. opa: the image's overall opacity.
INLINE void add_row_c(uint16_t* p, const uint8_t* s, int w, uint32_t opa) {
  for (int x = 0; x < w; x++, s += 4) {
    const uint32_t a = opa >= LV_OPA_MAX ? s[3] : LV_OPA_MIX2(opa, s[3]);
    if (a == 0) continue;
    const uint32_t d = p[x];
    const uint32_t r = (d >> 11) + (s[2] >> 3), g = ((d >> 5) & 63) + (s[1] >> 2), b = (d & 31) + (s[0] >> 3);
    const uint16_t res = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
    p[x] = a >= LV_OPA_MAX ? res : mix_prep(PREP(res), (uint16_t)d, (a + 4) >> 3);
  }
}

// Normal ARGB8888 sprites; opa >= LV_OPA_MAX: LVGL's plain hook, else its "with opa" hook.
INLINE void argb_row_c(uint16_t* p, const uint8_t* s, int w, uint32_t opa) {
  if (opa >= LV_OPA_MAX) {
    for (int x = 0; x < w; x++, s += 4) {
      const uint32_t a = s[3];
      if (a == 0) continue;
      p[x] = a >= LV_OPA_MAX ? c24_16(s) : mix_prep(PREP(c24_16(s)), p[x], (a + 4) >> 3);
    }
  } else {
    for (int x = 0; x < w; x++, s += 4) {
      if (s[3] == 0) continue;
      p[x] = mix_prep(PREP(c24_16(s)), p[x], ((uint32_t)LV_OPA_MIX2(s[3], opa) + 4) >> 3);
    }
  }
}

// ---- the rows on PIE ----
// Both rest on one identity: LVGL's packed mix is, per channel c, d + ((f - d) * m >> 5) with m = (opa + 4) >> 3
// and an arithmetic (rounding-down) shift (checked for every m and 1.5 billion colour pairs). Written as
// (d * (32 - m) + f * m) >> 5, or for additive blending (f >= d) as d + ((f - d) * m >> 5), every step is
// unsigned. There are no 16-bit shifts: a channel comes out with a 32-bit shift (by SAR) and a mask, which drops
// what crosses over from the neighbouring pixel; esp.vmul.u16 multiplies and shifts right by SAR. PIE takes
// addresses and counts only in x8-x15 (a0-a5 here), the loops are plain addi/bnez loops (esp.lp.setup is
// unreliable on v1.x chips), and the destination must be 16-byte aligned (the callers do the first pixels in C).
// k: the per-call constants, 8 lanes each, in the order the loop reads them.
#if BLEND_PIE
static void fill_row_pie(uint16_t* dst, int steps, const uint16_t* k) {
  register uint16_t* r_d asm("a0") = dst;
  register int r_n asm("a2") = steps;
  register const uint16_t* r_k asm("a3") = k;
  asm volatile(
      "1:\n\t"
      "mv a4, %[k]\n\t"
      "esp.vld.128.ip q0, %[d], 0\n\t"      // 8 destination pixels
      "esp.vld.128.ip q2, a4, 16\n\t"       // 32 - m
      "li a5, 11\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q1, q0\n\t"              // red
      "esp.vld.128.ip q3, a4, 16\n\t"       // 31
      "esp.andq q1, q1, q3\n\t"
      "li a5, 0\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vmul.u16 q1, q1, q2\n\t"         //   d * (32 - m)
      "esp.vld.128.ip q5, a4, 16\n\t"       //   + f * m
      "esp.vadd.u16 q1, q1, q5\n\t"
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q1, q1\n\t"              //   >> 5
      "esp.andq q1, q1, q3\n\t"
      "li a5, 11\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsl.32 q4, q1\n\t"               //   back to bits 11-15
      "esp.andq q1, q0, q3\n\t"             // blue
      "li a5, 0\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vmul.u16 q1, q1, q2\n\t"
      "esp.vld.128.ip q5, a4, 16\n\t"
      "esp.vadd.u16 q1, q1, q5\n\t"
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q1, q1\n\t"
      "esp.andq q1, q1, q3\n\t"
      "esp.orq q4, q4, q1\n\t"
      "esp.vsr.u32 q1, q0\n\t"              // green (SAR is 5)
      "esp.vld.128.ip q3, a4, 16\n\t"       // 63
      "esp.andq q1, q1, q3\n\t"
      "li a5, 0\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vmul.u16 q1, q1, q2\n\t"
      "esp.vld.128.ip q5, a4, 16\n\t"
      "esp.vadd.u16 q1, q1, q5\n\t"
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q1, q1\n\t"
      "esp.andq q1, q1, q3\n\t"
      "esp.vsl.32 q1, q1\n\t"               //   back to bits 5-10
      "esp.orq q4, q4, q1\n\t"
      "esp.vst.128.ip q4, %[d], 16\n\t"
      "addi %[n], %[n], -1\n\t"
      "bnez %[n], 1b\n\t"
      : [d] "+r"(r_d), [n] "+r"(r_n)
      : [k] "r"(r_k)
      : "a4", "a5", "memory");
}

// The source (ARGB8888, 32 bytes a step) need not be aligned: esp.ld.128.usar loads the aligned block and notes
// the offset, esp.src.q joins two blocks at that offset. The last block can lie up to 15 bytes past the row (in
// the same 16 bytes, so never on another page).
static void add_row_pie(uint16_t* dst, const uint8_t* src, int steps, const uint16_t* k) {
  register uint16_t* r_d asm("a0") = dst;
  register const uint8_t* r_s asm("a1") = src;
  register int r_n asm("a2") = steps;
  register const uint16_t* r_k asm("a3") = k;
  asm volatile(
      "1:\n\t"
      "esp.ld.128.usar.ip q0, %[s], 16\n\t"
      "esp.ld.128.usar.ip q1, %[s], 16\n\t"
      "esp.ld.128.usar.ip q2, %[s], 0\n\t"
      "esp.src.q q0, q0, q1\n\t"            // pixels 0-3
      "esp.src.q q1, q1, q2\n\t"            // pixels 4-7
      "esp.vunzip.16 q0, q1\n\t"            // q0: b | g << 8, q1: r | a << 8
      "mv a4, %[k]\n\t"
      "li a5, 3\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q2, q1\n\t"
      "esp.vld.128.ip q3, a4, 16\n\t"       // 31
      "esp.andq q2, q2, q3\n\t"             // q2: r >> 3
      "li a5, 8\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q1, q1\n\t"
      "esp.vld.128.ip q3, a4, 16\n\t"       // 255
      "esp.andq q1, q1, q3\n\t"             // alpha
      "esp.vld.128.ip q3, a4, 16\n\t"       // opa (256 when opaque)
      "esp.vmul.u16 q1, q1, q3\n\t"         //   alpha * opa >> 8
      "esp.vld.128.ip q3, a4, 16\n\t"       // 4
      "esp.vadd.u16 q1, q1, q3\n\t"
      "li a5, 3\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q1, q1\n\t"
      "esp.vld.128.ip q3, a4, 16\n\t"       // 63
      "esp.andq q1, q1, q3\n\t"             // q1: m = (a + 4) >> 3
      "esp.vld.128.ip q4, %[d], 0\n\t"      // 8 destination pixels
      "li a5, 11\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q5, q4\n\t"              // red
      "esp.vld.128.ip q3, a4, 16\n\t"       // 31
      "esp.andq q5, q5, q3\n\t"
      "esp.vsub.u16 q6, q3, q5\n\t"         //   room left: 31 - d
      "esp.vmin.u16 q2, q2, q6\n\t"         //   f - d (f = d + light, saturated)
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vmul.u16 q2, q2, q1\n\t"         //   * m >> 5
      "esp.vadd.u16 q5, q5, q2\n\t"
      "li a5, 11\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsl.32 q5, q5\n\t"               //   back to bits 11-15
      "li a5, 3\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q2, q0\n\t"              // blue
      "esp.andq q2, q2, q3\n\t"
      "esp.andq q6, q4, q3\n\t"
      "esp.vsub.u16 q7, q3, q6\n\t"
      "esp.vmin.u16 q2, q2, q7\n\t"
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vmul.u16 q2, q2, q1\n\t"
      "esp.vadd.u16 q6, q6, q2\n\t"
      "esp.orq q5, q5, q6\n\t"
      "li a5, 10\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q2, q0\n\t"              // green
      "esp.vld.128.ip q3, a4, 16\n\t"       // 63
      "esp.andq q2, q2, q3\n\t"
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q6, q4\n\t"
      "esp.andq q6, q6, q3\n\t"
      "esp.vsub.u16 q7, q3, q6\n\t"
      "esp.vmin.u16 q2, q2, q7\n\t"
      "esp.vmul.u16 q2, q2, q1\n\t"
      "esp.vadd.u16 q6, q6, q2\n\t"
      "esp.vsl.32 q6, q6\n\t"               //   back to bits 5-10
      "esp.orq q5, q5, q6\n\t"
      "esp.vst.128.ip q5, %[d], 16\n\t"
      "addi %[n], %[n], -1\n\t"
      "bnez %[n], 1b\n\t"
      : [d] "+r"(r_d), [s] "+r"(r_s), [n] "+r"(r_n)
      : [k] "r"(r_k)
      : "a4", "a5", "memory");
}
#endif

INLINE void lanes(uint16_t* k, uint32_t v) { for (int i = 0; i < 8; i++) k[i] = (uint16_t)v; }

struct FillK { uint16_t k[6][8]; } __attribute__((aligned(16)));
static void fill_k(struct FillK* k, uint16_t c, uint32_t mix5) {
  lanes(k->k[0], 32 - mix5);
  lanes(k->k[1], 31);
  lanes(k->k[2], (c >> 11) * mix5);
  lanes(k->k[3], (c & 31) * mix5);
  lanes(k->k[4], 63);
  lanes(k->k[5], ((c >> 5) & 63) * mix5);
}
struct AddK { uint16_t k[7][8]; } __attribute__((aligned(16)));
static void add_k(struct AddK* k, uint32_t opa) {
  lanes(k->k[0], 31);
  lanes(k->k[1], 255);
  lanes(k->k[2], opa >= LV_OPA_MAX ? 256 : opa);
  lanes(k->k[3], 4);
  lanes(k->k[4], 63);
  lanes(k->k[5], 31);
  lanes(k->k[6], 63);
}

// A row: the pixels before the first 16-byte boundary and after the last whole step in C, the rest on PIE.
INLINE int head_px(const uint16_t* p, int w) {
  const int h = (int)((16 - ((uintptr_t)p & 15)) & 15) >> 1;
  return h < w ? h : w;
}
static void fill_row(uint16_t* p, int w, uint16_t c, uint32_t mix5, const struct FillK* k, int pie) {
#if BLEND_PIE
  if (pie && w >= 16) {
    const int h = head_px(p, w), n = (w - h) >> 3;
    fill_row_c(p, h, PREP(c), mix5);
    fill_row_pie(p + h, n, &k->k[0][0]);
    fill_row_c(p + h + n * 8, w - h - n * 8, PREP(c), mix5);
    return;
  }
#endif
  (void)k; (void)pie;
  fill_row_c(p, w, PREP(c), mix5);
}
static void add_row(uint16_t* p, const uint8_t* s, int w, uint32_t opa, const struct AddK* k, int pie) {
#if BLEND_PIE
  if (pie && w >= 16) {
    const int h = head_px(p, w), n = (w - h) >> 3;
    add_row_c(p, s, h, opa);
    add_row_pie(p + h, s + h * 4, n, &k->k[0][0]);
    add_row_c(p + h + n * 8, s + (h + n * 8) * 4, w - h - n * 8, opa);
    return;
  }
#endif
  (void)k; (void)pie;
  add_row_c(p, s, w, opa);
}

// ---- the hooks ----
HOT int clock_fill_opa_565(lv_draw_sw_blend_fill_dsc_t* d) {
  const uint16_t c = lv_color_to_u16(d->color);
  const uint32_t mix5 = ((uint32_t)d->opa + 4) >> 3;
  struct FillK k;
  const int pie = g_blend_pie && d->dest_w >= 16 && !((uintptr_t)d->dest_buf & 1) && !(d->dest_stride & 1);
  if (pie) fill_k(&k, c, mix5);
  uint8_t* row = d->dest_buf;
  for (int y = 0; y < d->dest_h; y++, row += d->dest_stride) fill_row((uint16_t*)row, d->dest_w, c, mix5, &k, pie);
  return LV_RESULT_OK;
}

HOT int clock_argb_add_565(lv_draw_sw_blend_image_dsc_t* d) {
  if (d->mask_buf) return LV_RESULT_INVALID;
  struct AddK k;
  const int pie = g_blend_pie && d->dest_w >= 16 && !((uintptr_t)d->dest_buf & 1) && !(d->dest_stride & 1);
  if (pie) add_k(&k, d->opa);
  uint8_t* row = d->dest_buf;
  const uint8_t* src = d->src_buf;
  for (int y = 0; y < d->dest_h; y++, row += d->dest_stride, src += d->src_stride)
    add_row((uint16_t*)row, src, d->dest_w, d->opa, &k, pie);
  return LV_RESULT_OK;
}

// Sprites stay in C: they are mostly transparent, and skipping those pixels (no destination read or write) beat
// a PIE version that blends every pixel (numerals 23.0 vs 22.6 fps, flap 27.3 vs 26.7).
HOT static int argb_blend(lv_draw_sw_blend_image_dsc_t* d, uint32_t opa) {
  uint8_t* row = d->dest_buf;
  const uint8_t* src = d->src_buf;
  for (int y = 0; y < d->dest_h; y++, row += d->dest_stride, src += d->src_stride)
    argb_row_c((uint16_t*)row, src, d->dest_w, opa);
  return LV_RESULT_OK;
}
HOT int clock_argb_565(lv_draw_sw_blend_image_dsc_t* d) { return argb_blend(d, LV_OPA_COVER); }
HOT int clock_argb_opa_565(lv_draw_sw_blend_image_dsc_t* d) { return argb_blend(d, d->opa); }

// ---- checks (see lv_blend.h) ----
#define CK_W 160   // pixels per test row
struct CheckBufs {
  uint16_t base[CK_W + 16], a[CK_W + 16], b[CK_W + 16];
  uint8_t src[(CK_W + 16) * 4];
} __attribute__((aligned(16)));
static struct CheckBufs* g_ck;

INLINE uint32_t ck_rand(uint32_t* s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

// One round on new data, both kernels at a random alignment and length: 0 if PIE and C agree everywhere
// (outputs are pre-filled alike, so a write outside the row shows too), else 1 (fill) or 2 (additive) + 4 * pixel.
static int ck_round(uint32_t* s) {
#if BLEND_PIE
  struct CheckBufs* q = g_ck;
  const int off = (int)(ck_rand(s) % 8), w = 16 + (int)(ck_rand(s) % (CK_W - 16 - 8 + 1)), soff = (int)(ck_rand(s) % 4) * 4;
  for (int i = 0; i < CK_W + 16; i++) q->base[i] = (uint16_t)ck_rand(s);
  const int alpha_mode = (int)(ck_rand(s) % 4);   // random, mostly clear, mostly opaque, all levels
  for (int i = 0; i < (CK_W + 16) * 4; i += 4) {
    const uint32_t r = ck_rand(s);
    q->src[i] = (uint8_t)r; q->src[i + 1] = (uint8_t)(r >> 8); q->src[i + 2] = (uint8_t)(r >> 16);
    const uint32_t a = r >> 24;
    q->src[i + 3] = (uint8_t)(alpha_mode == 0 ? a : alpha_mode == 1 ? (a < 200 ? 0 : a) : alpha_mode == 2 ? (a < 100 ? 255 : a) : (i / 4) & 255);
  }
  const uint32_t opas[6] = {255, 254, 253, 252, 70, ck_rand(s) & 255};
  const uint32_t opa = opas[ck_rand(s) % 6];
  const uint16_t c = (uint16_t)ck_rand(s);
  const uint32_t mix5 = (opa + 4) >> 3;
  struct FillK fk; struct AddK ak;
  fill_k(&fk, c, mix5);
  add_k(&ak, opa);
  for (int kind = 1; kind <= 2; kind++) {
    memcpy(q->a, q->base, sizeof(q->a));
    memcpy(q->b, q->base, sizeof(q->b));
    if (kind == 1) { fill_row(q->a + off, w, c, mix5, &fk, 1); fill_row(q->b + off, w, c, mix5, &fk, 0); }
    else { add_row(q->a + off, q->src + soff, w, opa, &ak, 1); add_row(q->b + off, q->src + soff, w, opa, &ak, 0); }
    for (int i = 0; i < CK_W + 16; i++)
      if (q->a[i] != q->b[i]) return kind + 4 * i;
  }
#else
  (void)s;
#endif
  return 0;
}

int clock_blend_check(int rounds, unsigned seed, int* first) {
#if BLEND_PIE
  if (!g_ck) g_ck = (struct CheckBufs*)heap_caps_aligned_alloc(16, sizeof(struct CheckBufs), MALLOC_CAP_INTERNAL);
  if (!g_ck) return -1;
  uint32_t s = seed | 1;
  int bad = 0;
  for (int r = 0; r < rounds; r++) {
    const int e = ck_round(&s);
    if (e && !bad++ && first) *first = e;
  }
  return bad;
#else
  (void)rounds; (void)seed; (void)first;
  return -1;
#endif
}

int clock_blend_pie(int on) {
  if (on >= 0) g_blend_pie = BLEND_PIE && on;
  return g_blend_pie;
}

// Cycles for one 1280-pixel row (aligned), the fastest of a few runs: fill C/PIE, additive C/PIE.
void clock_blend_time(unsigned t[4]) {
  static uint16_t* row;
  static uint8_t* src;
  if (!row) row = (uint16_t*)heap_caps_aligned_alloc(16, 1280 * 2, MALLOC_CAP_INTERNAL);
  if (!src) src = (uint8_t*)heap_caps_aligned_alloc(16, 1280 * 4, MALLOC_CAP_INTERNAL);
  if (!row || !src) return;
  uint32_t s = 12345;
  for (int i = 0; i < 1280; i++) row[i] = (uint16_t)ck_rand(&s);
  for (int i = 0; i < 1280 * 4; i++) src[i] = (uint8_t)ck_rand(&s);
  struct FillK fk; struct AddK ak;
  fill_k(&fk, 0x7BEF, 20);
  add_k(&ak, 200);
  for (int i = 0; i < 4; i++) t[i] = ~0u;
  for (int r = 0; r < 4; r++)
    for (int i = 0; i < 4; i++) {
      const uint32_t t0 = esp_cpu_get_cycle_count();
      if (i < 2) fill_row(row, 1280, 0x7BEF, 20, &fk, i);
      else add_row(row, src, 1280, 200, &ak, i - 2);
      const uint32_t dt = esp_cpu_get_cycle_count() - t0;
      if (dt < t[i]) t[i] = dt;
    }
}
