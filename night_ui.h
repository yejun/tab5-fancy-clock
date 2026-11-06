#pragma once
// Hourly announcement, night mode and the settings card.
//
//  * Announcement: on the hour, a two-note bell and the spoken hour (voice_clips.h). The ES8388 codec and its amp are
//    only powered while a clip plays: M5.Speaker starts lazily on the first playRaw and is ended afterwards. All
//    speaker calls happen on the UI thread (loop), since starting/stopping it talks to the codec over the internal
//    I2C bus that the touch panel and fuel gauge share.
//  * Night mode (23:00-06:59, see clock_logic.h): the same layout as by day - dial, digital time, date, calendar -
//    drawn as a faint red outline on black at its own low backlight level, without the second hand and seconds bar,
//    redrawn once a minute; the hourly features keep quiet. A tap
//    shows the normal clock for PEEK_MS, so settings stay reachable at night.
//  * Settings card (gear icon in the calendar header): three switches and the announcement volume.
#include "voice_clips.h"

static constexpr int PEEK_MS = 30000;
static constexpr uint8_t DEFAULT_VOLUME = 140;
static constexpr uint8_t DEFAULT_NIGHT_BRIGHTNESS = 24;
static constexpr uint32_t NIGHT_RED = 0xD02A1C;

static bool    g_chime_enabled = true;
static uint8_t g_volume = DEFAULT_VOLUME;
static int64_t g_peek_until = 0;

// ----------------------------------------------------------------------------------------------
// Announcement playback: a small state machine stepped from loop(), queueing the next piece only when the speaker
// channel has a free slot (playRaw would otherwise block until one frees up).
// ----------------------------------------------------------------------------------------------
static int g_ann_step = 0, g_ann_hour = -1;
static int64_t g_ann_last_key = -1;    // date and hour of the last announcement check
static int16_t* g_silence = nullptr;   // 150 ms of zeros, in PSRAM
static constexpr uint32_t SILENCE_N = CLIP_RATE * 150 / 1000;

// hour 0-23, or -1 for the bell alone (volume preview)
static void announce_start(int hour) {
  if (!g_silence) g_silence = (int16_t*)heap_caps_calloc(SILENCE_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!g_silence) return;
  if (g_ann_step) M5.Speaker.stop();
  g_ann_hour = hour;
  g_ann_step = 1;
}

static bool ann_queue(const int16_t* pcm, uint32_t n) {
  if (M5.Speaker.isPlaying(0) >= 2) return false;
  M5.Speaker.playRaw(pcm, n, CLIP_RATE, false, 1, 0);
  return true;
}

static void announce_tick() {
  switch (g_ann_step) {
    case 0: return;
    case 1:   // power up; a little silence first lets the amp settle so the bell's attack isn't clipped
      M5.Speaker.setVolume(g_volume);
      if (!M5.Speaker.begin()) { Serial.println("WARNING: speaker won't start"); g_ann_step = 0; return; }
      if (ann_queue(g_silence, SILENCE_N * 2 / 5)) g_ann_step = 2;
      break;
    case 2: if (ann_queue(CLIP_CHIME.pcm, CLIP_CHIME.n)) g_ann_step = g_ann_hour >= 0 ? 3 : 5; break;
    case 3: if (ann_queue(g_silence, SILENCE_N)) g_ann_step = 4; break;
    case 4: if (ann_queue(CLIP_HOUR[g_ann_hour].pcm, CLIP_HOUR[g_ann_hour].n)) g_ann_step = 5; break;
    case 5: if (ann_queue(g_silence, SILENCE_N * 4 / 5)) g_ann_step = 6; break;
    case 6:   // done: codec and amp off until the next hour
      if (!M5.Speaker.isPlaying()) { M5.Speaker.end(); g_ann_step = 0; }
      break;
  }
}

// Called every loop pass, display on or off.
static void hourly_announce_tick() {
  static int64_t next = 0;
  if (mono_ms() < next) return;
  next = mono_ms() + 250;
  const Now n = now_local();
  const int64_t key = date_key(n) * 24 + n.h;
  if (n.mi != 0 || n.s > 5 || key == g_ann_last_key) return;
  g_ann_last_key = key;
  if (!hourly_allowed(g_chime_enabled, g_night_mode, n.h)) return;
  Serial.printf("announce %02d:00\n", n.h);
  announce_start(n.h);
}

// ----------------------------------------------------------------------------------------------
// Night display: an opaque full-screen object on top of the clock, so LVGL draws nothing underneath it.
// ----------------------------------------------------------------------------------------------
static lv_obj_t* night_obj = nullptr;
static lv_obj_t* set_gear = nullptr;   // settings gear in the calendar header
static int64_t g_night_key = -1;

static void night_line(lv_layer_t* L, float deg, float r0, float r1, int w, uint32_t col, lv_opa_t opa) {
  const float a = deg * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
  d_line(L, CX + s * r0, CY - c * r0, CX + s * r1, CY - c * r1, w, col, opa);
}

// A hollow capsule (hour mark, hand): a 3 px red wall round a black inside (which hides what it covers, such as the
// other hand), drawn as a capsule image (see capsule_render) so the black sits centred in the red at the ends too.
static constexpr int NIGHT_CAPSULES = 14;   // 12 hour marks, 2 hands
static CapsuleImg g_night_capsule[NIGHT_CAPSULES];

static void night_outline(lv_layer_t* L, int slot, float deg, float r0, float r1, float w) {
  const float a = deg * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
  const float x0 = CX + s * r0, y0 = CY - c * r0, x1 = CX + s * r1, y1 = CY - c * r1, p = w / 2 + 2;
  if (!clip_hit(L, fminf(x0, x1) - p, fminf(y0, y1) - p, fmaxf(x0, x1) + p, fmaxf(y0, y1) + p)) return;
  const Capsule cs[2] = {{x0, y0, x1, y1, w, NIGHT_RED, 1}, {x0, y0, x1, y1, w - 6, 0x000000, 1}};
  if (capsule_render(g_night_capsule[slot], cs, 2, 0.5f)) d_capsules(L, g_night_capsule[slot]);
}

static void night_text_at(lv_layer_t* L, const char* text, const lv_font_t* font, lv_text_align_t align, int letter_space,
                          const lv_area_t& a, lv_opa_t opa) {
  if (!text || !*text || !clip_hit(L, a.x1, a.y1, a.x2, a.y2)) return;
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = text;
  d.font = font;
  d.align = align;
  d.letter_space = letter_space;
  d.color = lv_color_hex(NIGHT_RED);
  d.opa = opa;
  lv_draw_label(L, &d, &a);
}

// Outlined digits of the big time: each slot's text is drawn once (when it changes) into an image - the glyph's own
// coverage grown by HOLLOW_R in every direction gives the red outline, the glyph itself stays black inside. Drawing
// the text in red at 8 offsets instead left the outline 2 px thick on straight strokes but ~1.4 px on slanted ones.
static constexpr float HOLLOW_R = 2.5f;
static constexpr int HOLLOW_M = 4;          // margin round the label for the outline
static lv_obj_t* hollow_canvas = nullptr;   // hidden; only lends its draw layer
struct Hollow { lv_draw_buf_t* buf = nullptr; char text[4] = ""; lv_area_t at = {}; };
static Hollow g_hollow[5];

static void hollow_prepare(Hollow& h, lv_obj_t* l) {
  const char* text = lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN) ? "" : lv_label_get_text(l);
  lv_obj_update_layout(l);
  lv_area_t a;
  lv_obj_get_coords(l, &a);
  const lv_area_t at = {a.x1 - HOLLOW_M, a.y1 - HOLLOW_M, a.x2 + HOLLOW_M, a.y2 + HOLLOW_M};
  if (h.buf && !strncmp(h.text, text, sizeof(h.text)) && !memcmp(&h.at, &at, sizeof(at))) return;
  snprintf(h.text, sizeof(h.text), "%s", text);
  h.at = at;
  if (!*text) return;
  const int w = lv_area_get_width(&at), hh = lv_area_get_height(&at);
  if (!h.buf || (int)h.buf->header.w != w || (int)h.buf->header.h != hh) {
    if (h.buf) lv_draw_buf_destroy(h.buf);
    h.buf = lv_draw_buf_create(w, hh, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!h.buf) return;
  } else {
    lv_image_cache_drop(h.buf);
  }
  lv_draw_buf_clear(h.buf, nullptr);
  lv_canvas_set_draw_buf(hollow_canvas, h.buf);
  lv_layer_t layer;
  lv_canvas_init_layer(hollow_canvas, &layer);
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = text;
  d.font = lv_obj_get_style_text_font(l, LV_PART_MAIN);
  d.align = lv_obj_get_style_text_align(l, LV_PART_MAIN);
  d.letter_space = lv_obj_get_style_text_letter_space(l, LV_PART_MAIN);
  d.color = lv_color_white();
  const lv_area_t inner = {HOLLOW_M, HOLLOW_M, w - 1 - HOLLOW_M, hh - 1 - HOLLOW_M};
  lv_draw_label(&layer, &d, &inner);
  lv_canvas_finish_layer(hollow_canvas, &layer);

  // grow the coverage: each pixel takes the strongest glyph pixel nearby, weighted by how far inside the disc it is
  uint8_t* cov = (uint8_t*)heap_caps_malloc(w * hh, MALLOC_CAP_SPIRAM);
  if (!cov) { lv_draw_buf_destroy(h.buf); h.buf = nullptr; return; }
  const uint32_t stride = h.buf->header.stride;
  for (int y = 0; y < hh; y++)
    for (int x = 0; x < w; x++) cov[y * w + x] = ((uint8_t*)h.buf->data)[y * stride + x * 4 + 3];
  struct Off { int8_t dx, dy; uint8_t k; } offs[64];
  int no = 0;
  for (int dy = -3; dy <= 3; dy++)
    for (int dx = -3; dx <= 3; dx++) {
      const float k = clamp01(HOLLOW_R + 0.5f - sqrtf((float)(dx * dx + dy * dy)));
      if (k > 0) offs[no++] = {(int8_t)dx, (int8_t)dy, (uint8_t)(k * 255 + 0.5f)};
    }
  const uint8_t red[3] = {NIGHT_RED & 255, (NIGHT_RED >> 8) & 255, (NIGHT_RED >> 16) & 255};   // B G R
  for (int y = 0; y < hh; y++) {
    uint8_t* row = (uint8_t*)h.buf->data + y * stride;
    for (int x = 0; x < w; x++) {
      int out = 0;
      for (int i = 0; i < no; i++) {
        const int sx = x + offs[i].dx, sy = y + offs[i].dy;
        if (sx < 0 || sy < 0 || sx >= w || sy >= hh) continue;
        const int v = cov[sy * w + sx] * offs[i].k;
        if (v > out) out = v;
      }
      out = (out + 127) / 255;
      const int g = cov[y * w + x];   // glyph coverage: black on top of the red
      uint8_t* q = row + x * 4;
      for (int k = 0; k < 3; k++) q[k] = out ? (uint8_t)(red[k] * (out - g) / out) : 0;
      q[3] = (uint8_t)out;
    }
  }
  heap_caps_free(cov);
}

static void hollow_draw(lv_layer_t* L, const Hollow& h) {
  if (!h.buf || !*h.text || !clip_hit(L, h.at.x1, h.at.y1, h.at.x2, h.at.y2)) return;
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = h.buf;
  lv_draw_image(L, &d, &h.at);
}

// One of the clock's own labels, where it is and as it reads, in red (its own opacity scales the red)
static void night_label(lv_layer_t* L, lv_obj_t* l) {
  if (!l || lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN)) return;
  lv_area_t a;
  lv_obj_get_coords(l, &a);
  night_text_at(L, lv_label_get_text(l), lv_obj_get_style_text_font(l, LV_PART_MAIN), lv_obj_get_style_text_align(l, LV_PART_MAIN),
                lv_obj_get_style_text_letter_space(l, LV_PART_MAIN), a, lv_obj_get_style_text_opa(l, LV_PART_MAIN));
}

static void night_draw_cb(lv_event_t* e) {
  lv_layer_t* L = lv_event_get_layer(e);
  const Now n = now_local();

  // Dial: rim, minute track, indices, numerals, day ring, hands, hub
  d_arc(L, CX, CY, 302, 2, 0, 360, NIGHT_RED, LV_OPA_COVER);
  for (int i = 0; i < 60; i++)
    if (i % 5) night_line(L, i * 6.0f, 270, 288, 2, NIGHT_RED, 130);
  for (int i = 0; i < 12; i++) night_outline(L, i, i * 30.0f, i == 0 ? 243 : 251, 289, 10);
  const int lh = lv_font_get_line_height(f_num);
  for (int i = 1; i <= 12; i++) {
    char t[4];
    snprintf(t, sizeof(t), "%d", i);
    const float ang = i * 30.0f * (float)M_PI / 180.0f;
    const int x = (int)(CX + sinf(ang) * 212.0f), y = (int)(CY - cosf(ang) * 212.0f) - lh / 2;
    const lv_area_t a = {x - 50, y, x + 49, y + lh - 1};
    night_text_at(L, t, f_num, LV_TEXT_ALIGN_CENTER, 0, a, 200);
  }
  d_arc(L, CX, CY, (int)R_RING + 1, 1, 0, 360, NIGHT_RED, 90);
  if (g_day_m > 0) d_arc(L, CX, CY, (int)R_RING + 2, 3, 270, 270 + g_day_m * 0.25f, NIGHT_RED, LV_OPA_COVER, true);
  const float min_f = n.mi + n.s / 60.0f;
  night_outline(L, 12, ((n.h % 12) + min_f / 60.0f) * 30.0f, HS_H.r0, HS_H.r1, HS_H.w + 4);
  night_outline(L, 13, min_f * 6.0f, HS_M.r0, HS_M.r1, HS_M.w + 4);
  d_disc(L, CX, CY, 13, NIGHT_RED, LV_OPA_COVER);
  d_disc(L, CX, CY, 10, 0x000000, LV_OPA_COVER);
  d_disc(L, CX, CY, 3, NIGHT_RED, LV_OPA_COVER);

  // Right panel: greeting, status, big time (hollow digits), date - no seconds
  night_label(L, lbl_greet);
  night_label(L, lbl_wifi);
  night_label(L, lbl_batt);
  for (const Hollow& h : g_hollow) hollow_draw(L, h);
  night_label(L, lbl_ampm);
  night_label(L, lbl_date);

  // Calendar card outline, month, weekdays, days, a ring round today
  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.radius = 28;
  r.bg_opa = LV_OPA_TRANSP;
  r.border_width = 2;
  r.border_color = lv_color_hex(NIGHT_RED);
  r.border_opa = 170;
  const lv_area_t card = {PX, 392, PX + PW - 1, 392 + 296 - 1};
  lv_draw_rect(L, &r, &card);
  night_label(L, lbl_month);
  night_label(L, set_gear);
  for (lv_obj_t* l : lbl_wd) night_label(L, l);
  if (!lv_obj_has_flag(today_mark, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t a;
    lv_obj_get_coords(today_mark, &a);
    r.radius = LV_RADIUS_CIRCLE;
    r.border_opa = LV_OPA_COVER;
    lv_draw_rect(L, &r, &a);
  }
  for (lv_obj_t* l : lbl_day) {
    if (lv_obj_get_style_text_opa(l, LV_PART_MAIN) == LV_OPA_COVER) {   // today (dark text on the day mark by day)
      lv_area_t a;
      lv_obj_get_coords(l, &a);
      night_text_at(L, lv_label_get_text(l), lv_obj_get_style_text_font(l, LV_PART_MAIN), LV_TEXT_ALIGN_CENTER, 0, a, LV_OPA_COVER);
    } else {
      night_label(L, l);
    }
  }
}

static uint8_t active_brightness() { return g_night_on ? g_bri_night : g_bri; }

static void night_enter() {
  show_stop();
  g_night_on = true;
  g_night_key = -1;
  lv_obj_remove_flag(night_obj, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(night_obj);
  lv_timer_set_period(g_fast_timer, 1000);
  if (g_disp_on) M5.Display.setBrightness(g_bri_night);
  Serial.println("night display on");
}

static void night_leave() {
  g_night_on = false;
  lv_obj_add_flag(night_obj, LV_OBJ_FLAG_HIDDEN);
  lv_timer_set_period(g_fast_timer, FRAME_MS);
  g_dirty_all = true;
  lv_obj_invalidate(lv_screen_active());
  if (g_disp_on) M5.Display.setBrightness(g_bri);
  Serial.println("night display off");
}

// Called at the start of every fast_cb; true while the night display is up (the clock itself then isn't updated).
static bool night_tick(const Now& n) {
  const bool want = night_active(g_night_mode, n.h) && mono_ms() >= g_peek_until && !g_wifi_open && !g_tz_open &&
                    !g_batt_open && !g_settings_open;
  if (want != g_night_on) want ? night_enter() : night_leave();
  if (!g_night_on) return false;
  const int64_t key = minute_key(n);
  if (key != g_night_key) {   // keep the clock's text model current (the night display reads it), then redraw
    if (g_night_key < 0 || key / 60 / 24 != g_night_key / 60 / 24) update_date(n);
    g_night_key = key;
    update_minute(n, false);
    for (int i = 0; i < 5; i++) hollow_prepare(g_hollow[i], lbl_slot[i]);
    lv_obj_invalidate(night_obj);
  }
  return true;
}

// ----------------------------------------------------------------------------------------------
// Settings card: over the calendar, opened from the gear in its header. Taps are hit-tested in run_tap, like the
// battery card (the clock face doesn't use LVGL's input device).
// ----------------------------------------------------------------------------------------------
static constexpr int SC_X = PX, SC_Y = 392, SC_W = PW, SC_H = 296;
static constexpr int SC_ROW0 = 62, SC_ROW_H = 57;                    // rows: announcement, volume, show, night
static constexpr int SC_VOL_X = 200, SC_VOL_W = SC_W - 200 - 100;    // volume bar, card-relative
static lv_obj_t *set_ui, *set_title, *set_sw[3], *set_lbl[4], *set_sub[3], *set_vol, *set_vol_lbl;

static void settings_refresh() {
  const bool on[3] = {g_chime_enabled, g_shows_enabled, g_night_mode};
  for (int i = 0; i < 3; i++) {
    if (on[i]) lv_obj_add_state(set_sw[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(set_sw[i], LV_STATE_CHECKED);
  }
  lv_bar_set_value(set_vol, g_volume, LV_ANIM_OFF);
  char b[8];
  snprintf(b, sizeof(b), "%d%%", (g_volume * 100 + 127) / 255);
  lv_label_set_text(set_vol_lbl, b);
  lv_label_set_text(set_sub[2], g_night_mode ? "23:00-07:00: faint red clock, hourly features quiet"
                                             : "Off: hourly features around the clock");
}

static void settings_build() {
  set_gear = mk_label(lv_screen_active(), &lv_font_montserrat_20, 0xFFFFFF, 150, LV_SYMBOL_SETTINGS,
                      PX + PW - 56, 414, 30);
  set_ui = mk(lv_layer_top(), SC_X, SC_Y, SC_W, SC_H);
  lv_obj_set_style_radius(set_ui, 28, 0);
  lv_obj_set_style_bg_opa(set_ui, 250, 0);   // nearly opaque: the calendar would ghost through
  lv_obj_set_style_border_width(set_ui, 1, 0);
  lv_obj_set_style_border_opa(set_ui, 50, 0);
  lv_obj_add_flag(set_ui, LV_OBJ_FLAG_HIDDEN);
  set_title = mk_label(set_ui, f_head, 0xFFFFFF, LV_OPA_COVER, "SETTINGS", 26, 16, 300);
  lv_obj_set_style_text_letter_space(set_title, 4, 0);
  const char* names[4] = {"Hourly announcement", "Volume", "Hourly show", "Night mode"};
  const char* subs[3] = {"Bell and the spoken hour", "A short animation on the hour", ""};
  for (int r = 0, s = 0; r < 4; r++) {
    const int y = SC_ROW0 + r * SC_ROW_H;
    set_lbl[r] = mk_label(set_ui, f_small, 0xFFFFFF, LV_OPA_COVER, names[r], 26, y + (r == 1 ? 12 : 2), 400);
    if (r == 1) continue;
    set_sub[s] = mk_label(set_ui, f_tiny, 0xFFFFFF, 150, subs[s], 26, y + 30, SC_W - 130);
    lv_obj_t* sw = set_sw[s++] = lv_switch_create(set_ui);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_set_pos(sw, SC_W - 26 - 64, y + 8);
  }
  set_vol = lv_bar_create(set_ui);
  lv_obj_remove_style_all(set_vol);
  lv_obj_set_pos(set_vol, SC_VOL_X, SC_ROW0 + SC_ROW_H + 22);
  lv_obj_set_size(set_vol, SC_VOL_W, 10);
  lv_bar_set_range(set_vol, 0, 255);
  lv_obj_set_style_radius(set_vol, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(set_vol, 40, LV_PART_MAIN);
  lv_obj_set_style_radius(set_vol, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(set_vol, LV_OPA_COVER, LV_PART_INDICATOR);
  set_vol_lbl = mk_label(set_ui, f_small, 0xFFFFFF, 200, "", SC_VOL_X + SC_VOL_W + 10, SC_ROW0 + SC_ROW_H + 12,
                         SC_W - SC_VOL_X - SC_VOL_W - 36, LV_TEXT_ALIGN_RIGHT);

  night_obj = mk(lv_screen_active(), 0, 0, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(night_obj, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(night_obj, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(night_obj, night_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_flag(night_obj, LV_OBJ_FLAG_HIDDEN);
  hollow_canvas = lv_canvas_create(lv_layer_bottom());
  lv_obj_add_flag(hollow_canvas, LV_OBJ_FLAG_HIDDEN);
}

static void settings_theme() {   // the gear follows the theme like the other header text
  if (set_gear) lv_obj_set_style_text_color(set_gear, lv_color_hex(THEMES[g_theme].ink), 0);
}

static void settings_open() {
  if (g_settings_open) return;
  g_settings_open = true;
  const Theme& th = THEMES[g_theme];
  lv_obj_set_style_bg_color(set_ui, lv_color_hex(th.bg_top), 0);
  lv_obj_set_style_border_color(set_ui, lv_color_hex(th.ink), 0);
  lv_obj_set_style_text_color(set_title, lv_color_hex(th.acc1), 0);
  for (lv_obj_t* l : set_lbl) lv_obj_set_style_text_color(l, lv_color_hex(th.ink), 0);
  for (lv_obj_t* l : set_sub) lv_obj_set_style_text_color(l, lv_color_hex(th.ink), 0);
  lv_obj_set_style_text_color(set_vol_lbl, lv_color_hex(th.ink), 0);
  lv_obj_set_style_bg_color(set_vol, lv_color_hex(th.ink), LV_PART_MAIN);
  lv_obj_set_style_bg_color(set_vol, lv_color_hex(th.acc1), LV_PART_INDICATOR);
  for (lv_obj_t* sw : set_sw) {
    lv_obj_set_style_bg_color(sw, lv_color_hex(th.ink), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, 50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(th.acc1), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(th.ink), LV_PART_KNOB);
  }
  settings_refresh();
  lv_obj_remove_flag(set_ui, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(set_ui);
}

static void settings_close() {
  if (!g_settings_open) return;
  g_settings_open = false;
  lv_obj_add_flag(set_ui, LV_OBJ_FLAG_HIDDEN);
}

static void set_chime(bool on) { g_chime_enabled = on; prefs.putBool("chime", on); }
static void set_shows(bool on) { g_shows_enabled = on; prefs.putBool("shows", on); }
static void set_night_mode(bool on) { g_night_mode = on; prefs.putBool("night", on); }
static void set_volume(int v) { g_volume = (uint8_t)constrain(v, 0, 255); prefs.putUChar("vol", g_volume); }

static bool settings_gear_hit(int x, int y) { return x >= PX + PW - 100 && x < PX + PW && y >= SC_Y && y < SC_Y + 60; }

// A tap while the card is open: a row toggles its switch, the volume bar sets the volume (and plays the bell),
// anywhere else closes the card.
static void settings_tap(int x, int y) {
  x -= SC_X; y -= SC_Y;
  if (x < 0 || x >= SC_W || y < SC_ROW0 || y >= SC_H) { settings_close(); return; }
  const int row = min(3, (y - SC_ROW0) / SC_ROW_H);
  if (row == 0) set_chime(!g_chime_enabled);
  else if (row == 1) {
    if (x < SC_VOL_X - 30) return;
    set_volume((x - SC_VOL_X) * 255 / SC_VOL_W);
    announce_start(-1);
  } else if (row == 2) set_shows(!g_shows_enabled);
  else set_night_mode(!g_night_mode);
  settings_refresh();
}

static void settings_load() {
  g_chime_enabled = prefs.getBool("chime", true);
  g_shows_enabled = prefs.getBool("shows", true);
  g_night_mode = prefs.getBool("night", true);   // on by default: 1.0.0 already kept quiet at night
  g_volume = prefs.getUChar("vol", DEFAULT_VOLUME);
  g_bri_night = (uint8_t)constrain((int)prefs.getUChar("bri_n", DEFAULT_NIGHT_BRIGHTNESS), MIN_BRIGHTNESS, 255);
}
