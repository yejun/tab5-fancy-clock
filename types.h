#pragma once
// Types live in a header so the Arduino auto-generated prototypes can see them.
#include <lvgl.h>
#include <stdint.h>

struct Theme { uint32_t bg_top, bg_bot, acc1, acc2; };
struct Now   { int y, mo, d, h, mi, s, ms, wd; };
struct LineObj { lv_obj_t* o; lv_point_precise_t p[2]; };
struct ScanEntry { char ssid[33]; int8_t rssi; bool secure; };  // one WiFi scan result
struct Stat { uint32_t max_us = 0, slow = 0; };  // loop-section timing stats
