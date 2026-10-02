#pragma once
// Types live in a header so the Arduino auto-generated prototypes can see them.
#include <lvgl.h>
#include <stdint.h>

struct Theme { const char* name; uint32_t bg_top, bg_bot, acc1, acc2, acc3, ink; };
#include "clock_logic.h"
struct HandSpec { float r0, r1, w, lume0, lume1, lume_w; };  // radii from the dial centre, widths in px
struct Rgba  { float r, g, b, a; };  // sprite compositing (0..1, straight alpha)
struct Glow   { float x, y, sx, sy, amp; uint32_t c; };                                  // background light
struct Ribbon { float y0, slope, a1, k1, p1, a2, k2, p2, w, amp, k3, p3; uint32_t c; };  // aurora band
struct RibbonCol { float yc, amp, w; };                                               // a ribbon at one x
struct DialCols  { float ink[3], acc3[3]; };                                         // dial paint colours
typedef void (*BoxSink)(float x1, float y1, float x2, float y2);                        // receives dirty boxes
struct ScanEntry { char ssid[33]; int8_t rssi; bool secure; };  // one WiFi scan result
struct Stat { uint32_t max_us = 0, slow = 0; };  // loop-section timing stats
