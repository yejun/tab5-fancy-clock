#pragma once
// LVGL's custom blend hooks (lv_conf.h: LV_USE_DRAW_SW_ASM = LV_DRAW_SW_ASM_CUSTOM). LVGL calls these for the
// RGB565 blends below instead of its own loops; a hook that returns LV_RESULT_OK has done the work. Included by
// lv_draw_sw_blend.h before the descriptor structs are defined, hence the forward declarations.
struct _lv_draw_sw_blend_fill_dsc_t;
struct _lv_draw_sw_blend_image_dsc_t;
#ifdef __cplusplus
extern "C" {
#endif
int clock_fill_opa_565(struct _lv_draw_sw_blend_fill_dsc_t* d);
int clock_argb_565(struct _lv_draw_sw_blend_image_dsc_t* d);
int clock_argb_opa_565(struct _lv_draw_sw_blend_image_dsc_t* d);
#ifdef __cplusplus
}
#endif
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565_WITH_OPA(dsc) clock_fill_opa_565(dsc)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB565(dsc) clock_argb_565(dsc)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB565_WITH_OPA(dsc) clock_argb_opa_565(dsc)
