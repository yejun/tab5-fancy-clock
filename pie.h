#pragma once
// SIMD kernels for the show renderers on core 0, on the ESP32-P4's PIE unit (128-bit registers q0-q7: 8 RGB565
// pixels at a time). Each has a plain-C reference that gives exactly the same result; at start-up pie_setup() runs
// both on random data and only uses PIE if they match. Serial "R" repeats that check and times both, "R <n>" runs
// n stress rounds (new data each round, outputs pre-filled with a marker, so a misplaced write can't hide).
//
// Lessons from the Tab5 PC and web display projects: PIE instructions take addresses and counts only in x8-x15
// (s0, s1, a0-a5), hence the register variables; and the loops are plain addi/bnez loops - the hardware loop
// (esp.lp.setup) sometimes wrote output in the wrong place on v1.x chips.
#if defined(__riscv_xesppie)
#define CLOCK_PIE 1
#else
#define CLOCK_PIE 0
#endif

#include "lv_blend.h"   // the PIE versions of LVGL's fill and additive blends (lv_blend.c), checked here too

static bool g_pie_aurora = false;
static bool g_pie_blend_ok = false;

// ---- aurora: light added to the band, per RGB565 channel, saturating ----
// For each pixel i: r5 = min(31, r5 + (cr * w >> 11)), g6 = min(63, g6 + (cg2 * w >> 11)), b5 = min(31, b5 + (cb * w >> 11)),
// which is the same as adding c * w >> 8 to the 8-bit channels and cutting back to 565. w: the light's weight (0-255).
// The colours come in blocks of 8 pixels: 8 reds, 8 greens (times 2), 8 blues. n: a multiple of 8; all 16-byte aligned.
static constexpr int AU_TAB_BLOCK = 24;   // uint16 per block

static void au_px_c(const uint16_t* base, const uint16_t* w, const uint16_t* tab, uint16_t* out, int n) {
  for (int i = 0; i < n; i++) {
    const uint16_t* t = tab + (i >> 3) * AU_TAB_BLOCK + (i & 7);
    const uint32_t v = base[i], k = w[i];
    const uint32_t r = min<uint32_t>(31, (v >> 11) + ((t[0] * k) >> 11));
    const uint32_t g = min<uint32_t>(63, ((v >> 5) & 63) + ((t[8] * k) >> 11));
    const uint32_t b = min<uint32_t>(31, (v & 31) + ((t[16] * k) >> 11));
    out[i] = (uint16_t)(r << 11 | g << 5 | b);
  }
}

#if CLOCK_PIE
// The same, 8 pixels a step. There are no 16-bit shifts: channels come out with 32-bit shifts (by SAR) and a mask,
// which drops whatever crosses over from the neighbouring pixel; esp.vmul.u16 multiplies and shifts right by SAR.
alignas(16) static const uint16_t AU_CONST[16] = {31, 31, 31, 31, 31, 31, 31, 31, 63, 63, 63, 63, 63, 63, 63, 63};

static void au_px_pie(const uint16_t* base, const uint16_t* w, const uint16_t* tab, uint16_t* out, int n) {
  if (n < 8) return;
  register const uint16_t* r_base asm("a0") = base;
  register const uint16_t* r_w asm("a1") = w;
  register const uint16_t* r_tab asm("a2") = tab;
  register uint16_t* r_out asm("a3") = out;
  register int r_steps asm("a4") = n / 8;
  asm volatile(
      "mv a5, %[k]\n\t"
      "esp.vld.128.ip q6, a5, 16\n\t"       // 31 in every lane
      "esp.vld.128.ip q7, a5, 16\n\t"       // 63
      "1:\n\t"
      "li a5, 11\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vld.128.ip q0, %[base], 16\n\t"
      "esp.vld.128.ip q1, %[w], 16\n\t"
      "esp.vsr.u32 q2, q0\n\t"              // red: base >> 11
      "esp.andq q2, q2, q6\n\t"
      "esp.vld.128.ip q3, %[tab], 16\n\t"
      "esp.vmul.u16 q3, q3, q1\n\t"         //      cr * w >> 11
      "esp.vadd.u16 q2, q2, q3\n\t"
      "esp.vmin.u16 q2, q2, q6\n\t"
      "esp.vsl.32 q2, q2\n\t"               //      back to bits 11-15
      "esp.vld.128.ip q3, %[tab], 16\n\t"
      "esp.vmul.u16 q3, q3, q1\n\t"         // green's addend: cg2 * w >> 11 (used below)
      "esp.vld.128.ip q4, %[tab], 16\n\t"
      "esp.vmul.u16 q4, q4, q1\n\t"         // blue: cb * w >> 11
      "esp.andq q5, q0, q6\n\t"
      "esp.vadd.u16 q5, q5, q4\n\t"
      "esp.vmin.u16 q5, q5, q6\n\t"
      "esp.orq q2, q2, q5\n\t"
      "li a5, 5\n\t"
      "esp.movx.w.sar a5\n\t"
      "esp.vsr.u32 q5, q0\n\t"              // green: base >> 5
      "esp.andq q5, q5, q7\n\t"
      "esp.vadd.u16 q5, q5, q3\n\t"
      "esp.vmin.u16 q5, q5, q7\n\t"
      "esp.vsl.32 q5, q5\n\t"               //        back to bits 5-10
      "esp.orq q2, q2, q5\n\t"
      "esp.vst.128.ip q2, %[out], 16\n\t"
      "addi %[steps], %[steps], -1\n\t"
      "bnez %[steps], 1b\n\t"
      : [base] "+r"(r_base), [w] "+r"(r_w), [tab] "+r"(r_tab), [out] "+r"(r_out), [steps] "+r"(r_steps)
      : [k] "r"(AU_CONST)
      : "a5", "memory");
}
#endif

static inline void au_px(const uint16_t* base, const uint16_t* w, const uint16_t* tab, uint16_t* out, int n) {
#if CLOCK_PIE
  if (g_pie_aurora) { au_px_pie(base, w, tab, out, n); return; }
#endif
  au_px_c(base, w, tab, out, n);
}

// ---- checks ----
static inline uint32_t pie_rand(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static constexpr int PIE_N = 304;   // one aurora column

struct PieBufs {
  alignas(16) uint16_t base[PIE_N], w[PIE_N], tab[PIE_N / 8 * AU_TAB_BLOCK], a[PIE_N + 8], b[PIE_N + 8];
};

// One round on new data: false if PIE and C differ (or PIE wrote outside its n pixels).
static bool au_round(PieBufs& q, uint32_t& s, int n, int* first = nullptr) {
  for (int i = 0; i < PIE_N; i++) {
    q.base[i] = (uint16_t)pie_rand(s);
    q.w[i] = (uint16_t)(pie_rand(s) % 256);
  }
  for (int i = 0; i < PIE_N / 8 * AU_TAB_BLOCK; i++) {
    const int ch = (i % AU_TAB_BLOCK) / 8;
    q.tab[i] = (uint16_t)(pie_rand(s) % 256) * (ch == 1 ? 2 : 1);
  }
  for (int i = 0; i < PIE_N + 8; i++) q.a[i] = q.b[i] = 0xA5C3;
  au_px_c(q.base, q.w, q.tab, q.b, n);
#if CLOCK_PIE
  au_px_pie(q.base, q.w, q.tab, q.a, n);
#endif
  for (int i = 0; i < PIE_N + 8; i++)
    if (q.a[i] != q.b[i]) { if (first) *first = i; return false; }
  return true;
}

// The start-up check (and "R"): a few rounds at full and shorter lengths, then the time of each version.
static void pie_check(bool report) {
#if CLOCK_PIE
  static PieBufs* q = (PieBufs*)heap_caps_aligned_alloc(16, sizeof(PieBufs), MALLOC_CAP_INTERNAL);
  if (!q) return;
  uint32_t s = 0x9E3779B9u;
  int first = -1;
  bool ok = true;
  for (int r = 0; r < 32 && ok; r++) ok = au_round(*q, s, r & 1 ? PIE_N - 8 * (r % 7 + 1) : PIE_N, &first);
  g_pie_aurora = ok;
  uint32_t c = ~0u, p = ~0u;
  for (int k = 0; k < 4; k++) {   // the fastest of a few runs each (an interrupt can land in one)
    uint32_t t = esp_cpu_get_cycle_count();
    au_px_c(q->base, q->w, q->tab, q->b, PIE_N);
    c = min(c, esp_cpu_get_cycle_count() - t);
    t = esp_cpu_get_cycle_count();
    au_px_pie(q->base, q->w, q->tab, q->a, PIE_N);
    p = min(p, esp_cpu_get_cycle_count() - t);
  }
  if (report || !ok)
    Serial.printf("pie: aurora kernel %s; %d pixels: C %u cycles, PIE %u cycles (%.1fx)%s\n", ok ? "matches C" : "DIFFERS, using C",
                  PIE_N, (unsigned)c, (unsigned)p, (float)c / (p ? p : 1), ok ? "" : (String(" at pixel ") + first).c_str());
  int bf = 0;
  const int bad = clock_blend_check(2000, 0x2545F491u, &bf);
  g_pie_blend_ok = bad == 0;
  clock_blend_pie(g_pie_blend_ok);
  unsigned t[4] = {};
  clock_blend_time(t);
  if (report || bad)
    Serial.printf("pie: blends %s; 1280 pixels, C/PIE cycles: fill %u/%u (%.1fx), additive %u/%u (%.1fx)%s\n",
                  bad ? "DIFFER, using C" : "match C", t[0], t[1], (float)t[0] / (t[1] ? t[1] : 1), t[2], t[3],
                  (float)t[2] / (t[3] ? t[3] : 1),
                  bad ? (String(" (") + ((bf & 3) == 1 ? "fill" : "additive") + " at pixel " + (bf >> 2) + ")").c_str() : "");
#else
  if (report) Serial.println("pie: not available in this build (plain C)");
#endif
}

static void pie_stress(int rounds) {
#if CLOCK_PIE
  static PieBufs* q = (PieBufs*)heap_caps_aligned_alloc(16, sizeof(PieBufs), MALLOC_CAP_INTERNAL);
  if (!q) return;
  uint32_t s = esp_cpu_get_cycle_count() | 1;
  int bad = 0, first = -1;
  for (int r = 0; r < rounds; r++) {
    int f = -1;
    if (!au_round(*q, s, PIE_N, &f) && !bad++) first = f;
    if (r % 500 == 499) vTaskDelay(1);   // (the task watchdog)
  }
  Serial.printf("pie stress: aurora %d of %d rounds differ%s\n", bad, rounds, bad ? (String(", first at pixel ") + first).c_str() : "");
  int bf = 0, bb = 0;
  for (int done = 0; done < rounds; done += 500) {   // (yielding for the task watchdog)
    bb += clock_blend_check(min(500, rounds - done), esp_cpu_get_cycle_count(), bb ? nullptr : &bf);
    vTaskDelay(1);
  }
  Serial.printf("pie stress: blends %d of %d rounds differ%s\n", bb, rounds,
                bb ? (String(", first: ") + ((bf & 3) == 1 ? "fill" : "additive") + " at pixel " + (bf >> 2)).c_str() : "");
#else
  (void)rounds;
  Serial.println("pie: not available in this build (plain C)");
#endif
}

static void pie_setup() { pie_check(true); }
