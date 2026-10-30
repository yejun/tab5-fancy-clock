// The custom blend hooks declared in lv_blend.h. They give exactly LVGL's results (checked by comparing whole
// frames on the PC), but mix all three channels of an RGB565 pixel at once, skip transparent pixels, and keep
// the mix inline where LVGL calls lv_color_16_16_mix per pixel. Translucent fills (flap cards, panels) and
// sprites (second hand, numerals) were 30-60% of a show frame.
#include <lvgl.h>
#include <draw/sw/blend/lv_draw_sw_blend_private.h>

#define HOT __attribute__((optimize("O2")))
// RGB565 spread out as 0b00000ggggggg00000rrrrr000000bbbbb so one multiply mixes all three channels
#define PREP(c) (((uint32_t)(c) | (uint32_t)(c) << 16) & 0x07E0F81Fu)

// fg: PREP'd colour, mix5: weight 0-32 (LVGL's (opa + 4) >> 3)
static inline __attribute__((always_inline)) uint16_t mix_prep(uint32_t fg, uint16_t d, uint32_t mix5) {
  const uint32_t bg = PREP(d), r = ((((fg - bg) * mix5) >> 5) + bg) & 0x07E0F81Fu;
  return (uint16_t)(r >> 16 | r);
}
static inline __attribute__((always_inline)) uint16_t c24_16(const uint8_t* c) {
  return (uint16_t)((c[2] & 0xF8) << 8 | (c[1] & 0xFC) << 3 | c[0] >> 3);
}

HOT int clock_fill_opa_565(lv_draw_sw_blend_fill_dsc_t* d) {
  const uint32_t fg = PREP(lv_color_to_u16(d->color)), mix5 = ((uint32_t)d->opa + 4) >> 3;
  uint8_t* row = d->dest_buf;
  for (int y = 0; y < d->dest_h; y++, row += d->dest_stride) {
    uint16_t* p = (uint16_t*)row;
    for (int x = 0; x < d->dest_w; x++) p[x] = mix_prep(fg, p[x], mix5);
  }
  return LV_RESULT_OK;
}

HOT int clock_argb_565(lv_draw_sw_blend_image_dsc_t* d) {
  uint8_t* row = d->dest_buf;
  const uint8_t* src = d->src_buf;
  for (int y = 0; y < d->dest_h; y++, row += d->dest_stride, src += d->src_stride) {
    uint16_t* p = (uint16_t*)row;
    const uint8_t* s = src;
    for (int x = 0; x < d->dest_w; x++, s += 4) {
      const uint32_t a = s[3];
      if (a == 0) continue;
      p[x] = a >= LV_OPA_MAX ? c24_16(s) : mix_prep(PREP(c24_16(s)), p[x], (a + 4) >> 3);
    }
  }
  return LV_RESULT_OK;
}

HOT int clock_argb_opa_565(lv_draw_sw_blend_image_dsc_t* d) {
  const uint32_t opa = d->opa;
  uint8_t* row = d->dest_buf;
  const uint8_t* src = d->src_buf;
  for (int y = 0; y < d->dest_h; y++, row += d->dest_stride, src += d->src_stride) {
    uint16_t* p = (uint16_t*)row;
    const uint8_t* s = src;
    for (int x = 0; x < d->dest_w; x++, s += 4) {
      if (s[3] == 0) continue;
      p[x] = mix_prep(PREP(c24_16(s)), p[x], ((uint32_t)LV_OPA_MIX2(s[3], opa) + 4) >> 3);
    }
  }
  return LV_RESULT_OK;
}
