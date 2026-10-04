#pragma once
// Time zone screen: tap the greeting. Major cities in alphabetical order, each with its current local time and UTC
// offset (worked out from the zone's rule, so daylight saving is included). A tap applies the zone and closes.
// Lives on LVGL's top layer like the WiFi screen; the clock animation pauses while it is open.

struct TzEntry { const char* city; const char* tz; };
static const TzEntry TZ_LIST[] = {   // alphabetical; POSIX TZ strings
  {"Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"Bangkok", "<+07>-7"},
  {"Beijing", "CST-8"},
  {"Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Bogota", "<-05>5"},
  {"Brisbane", "AEST-10"},
  {"Buenos Aires", "<-03>3"},
  {"Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
  {"Chicago", "CST6CDT,M3.2.0,M11.1.0"},
  {"Denver", "MST7MDT,M3.2.0,M11.1.0"},
  {"Dubai", "<+04>-4"},
  {"Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
  {"Halifax", "AST4ADT,M3.2.0,M11.1.0"},
  {"Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Hong Kong", "HKT-8"},
  {"Honolulu", "HST10"},
  {"Istanbul", "<+03>-3"},
  {"Jakarta", "WIB-7"},
  {"Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
  {"Johannesburg", "SAST-2"},
  {"Karachi", "PKT-5"},
  {"Kathmandu", "<+0545>-5:45"},
  {"Kolkata", "IST-5:30"},
  {"Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Lagos", "WAT-1"},
  {"Lima", "<-05>5"},
  {"Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
  {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
  {"Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Manila", "PST-8"},
  {"Mexico City", "CST6"},
  {"Moscow", "MSK-3"},
  {"Nairobi", "EAT-3"},
  {"New York", "EST5EDT,M3.2.0,M11.1.0"},
  {"Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Perth", "AWST-8"},
  {"Phoenix", "MST7"},
  {"Reykjavik", "GMT0"},
  {"Riyadh", "<+03>-3"},
  {"Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
  {"Sao Paulo", "<-03>3"},
  {"Seoul", "KST-9"},
  {"Shanghai", "CST-8"},
  {"Singapore", "<+08>-8"},
  {"Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Taipei", "CST-8"},
  {"Tehran", "<+0330>-3:30"},
  {"Tokyo", "JST-9"},
  {"Toronto", "EST5EDT,M3.2.0,M11.1.0"},
  {"UTC", "UTC0"},
  {"Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
  {"Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
};
static constexpr int N_TZ = sizeof(TZ_LIST) / sizeof(TZ_LIST[0]);

static lv_obj_t *tz_ui, *tz_list, *tz_col[2], *tz_status;
static bool g_tz_open = false;
static String g_tz_name;            // the city picked last (several cities share a rule)

static int64_t now_utc_s() { return ((ntp_active() ? g_utc_base_ms : g_base_ms) + mono_ms()) / 1000; }

// Local time and UTC offset (seconds) of `tz` at UTC `utc`. Switches the process time zone briefly.
static void tz_eval(const char* tz, int64_t utc, struct tm& lt, int& offset) {
  setenv("TZ", tz, 1);
  tzset();
  const time_t t = (time_t)utc;
  localtime_r(&t, &lt);
  struct tm gt;
  gmtime_r(&t, &gt);
  offset = (int)((days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) -
                  days_from_civil(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday)) * 86400 +
                 (lt.tm_hour - gt.tm_hour) * 3600 + (lt.tm_min - gt.tm_min) * 60);
}
static void tz_restore() { setenv("TZ", g_tz.c_str(), 1); tzset(); }

static void fmt_offset(char* b, size_t n, int off) {
  const int a = abs(off) / 60;
  if (a % 60) snprintf(b, n, "UTC%c%d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
  else if (a) snprintf(b, n, "UTC%c%d", off < 0 ? '-' : '+', a / 60);
  else snprintf(b, n, "UTC");
}

static int tz_current_index() {     // the picked city if it still matches the rule, else the first city with it
  for (int i = 0; i < N_TZ; i++) if (g_tz_name == TZ_LIST[i].city && g_tz == TZ_LIST[i].tz) return i;
  for (int i = 0; i < N_TZ; i++) if (g_tz == TZ_LIST[i].tz) return i;
  return -1;
}

static void tz_apply(const char* tz, const char* city) {
  g_tz = tz;
  g_tz_name = city ? city : "";
  prefs.putString("tz", g_tz);
  prefs.putString("tzname", g_tz_name);
  tz_restore();
  g_dirty_all = true;               // every label and hand follows on the next frame
}

static void tz_close() {
  if (!g_tz_open) return;
  g_tz_open = false;
  lv_obj_add_flag(tz_ui, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(tz_col[0]);          // rows are rebuilt (with fresh times) on every open
  lv_obj_clean(tz_col[1]);
  lv_timer_pause(lv_indev_get_read_timer(g_indev));
  lv_timer_resume(g_fast_timer);
  g_dirty_all = true;
  lv_obj_invalidate(lv_screen_active());
}

static void tz_select(int i) {
  if (i < 0 || i >= N_TZ) return;
  tz_apply(TZ_LIST[i].tz, TZ_LIST[i].city);
  Serial.printf("tz: %s (%s)\n", TZ_LIST[i].city, TZ_LIST[i].tz);
  tz_close();
}
static void tz_row_cb(lv_event_t* e) { tz_select((int)(intptr_t)lv_event_get_user_data(e)); }
static void tz_close_cb(lv_event_t*) { tz_close(); }

static void tz_open() {
  if (g_tz_open || g_wifi_open) return;
  g_tz_open = true;
  const Theme& th = THEMES[g_theme];
  const int cur = tz_current_index();
  const int64_t utc = now_utc_s();
  lv_obj_t* cur_row = nullptr;
  for (int i = 0; i < N_TZ; i++) {
    struct tm lt;
    int off;
    tz_eval(TZ_LIST[i].tz, utc, lt, off);
    lv_obj_t* row = lv_button_create(tz_col[i < (N_TZ + 1) / 2 ? 0 : 1]);   // read down the left column, then the right
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 590, 64);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(i == cur ? 0x24305C : 0x1C2340), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x33407A), LV_STATE_PRESSED);
    if (i == cur) {
      lv_obj_set_style_border_width(row, 2, 0);
      lv_obj_set_style_border_color(row, lv_color_hex(th.acc1), 0);
      cur_row = row;
    }
    lv_obj_add_event_cb(row, tz_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    char nm[48];
    snprintf(nm, sizeof(nm), "%s%s", i == cur ? LV_SYMBOL_OK "  " : "", TZ_LIST[i].city);
    lv_obj_t* name = lv_label_create(row);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(name, i == cur ? lv_color_hex(th.acc1) : lv_color_white(), 0);
    lv_label_set_text(name, nm);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 22, 0);
    char info[32], ofs[16];
    fmt_offset(ofs, sizeof(ofs), off);
    if (g_24h) snprintf(info, sizeof(info), "%02d:%02d   %s", lt.tm_hour, lt.tm_min, ofs);
    else snprintf(info, sizeof(info), "%d:%02d %s   %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min,
                  lt.tm_hour < 12 ? "am" : "pm", ofs);
    lv_obj_t* inf = lv_label_create(row);
    lv_obj_set_style_text_font(inf, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(inf, lv_color_hex(0x9AA3C8), 0);
    lv_label_set_text(inf, info);
    lv_obj_align(inf, LV_ALIGN_RIGHT_MID, -22, 0);
  }
  tz_restore();
  {
    char b[128], ofs[16];
    struct tm lt;
    int off;
    tz_eval(g_tz.c_str(), utc, lt, off);
    tz_restore();
    fmt_offset(ofs, sizeof(ofs), off);
    if (cur >= 0) snprintf(b, sizeof(b), "Now: %s, %s. Tap a city to change.", TZ_LIST[cur].city, ofs);
    else snprintf(b, sizeof(b), "Now: custom rule %s (%s). Tap a city to change.", g_tz.c_str(), ofs);
    lv_label_set_text(tz_status, b);
  }
  lv_timer_pause(g_fast_timer);
  lv_obj_remove_flag(tz_ui, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(tz_ui);
  lv_obj_update_layout(tz_ui);
  lv_obj_scroll_to_y(tz_list, 0, LV_ANIM_OFF);
  if (cur_row) lv_obj_scroll_to_view(cur_row, LV_ANIM_OFF);
  lv_timer_resume(lv_indev_get_read_timer(g_indev));
}

static void tz_build() {
  tz_ui = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(tz_ui);
  lv_obj_set_size(tz_ui, SCR_W, SCR_H);
  lv_obj_set_style_bg_opa(tz_ui, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tz_ui, lv_color_hex(0x0C1233), 0);
  lv_obj_remove_flag(tz_ui, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tz_ui, LV_OBJ_FLAG_HIDDEN);
  mk_label(tz_ui, f_date, 0xFFFFFF, LV_OPA_COVER, "Time zone", 40, 22, 600);
  tz_status = mk_label(tz_ui, &lv_font_montserrat_20, 0xB8C0E0, LV_OPA_COVER, "", 40, 86, 980);
  wl_button(tz_ui, "Close", 1050, 22, 190, 62, tz_close_cb, 0x5B2A45);
  tz_list = lv_obj_create(tz_ui);
  lv_obj_remove_style_all(tz_list);
  lv_obj_set_pos(tz_list, 40, 130);
  lv_obj_set_size(tz_list, 1200, 580);
  lv_obj_set_flex_flow(tz_list, LV_FLEX_FLOW_ROW);         // two columns scrolling together
  lv_obj_set_style_pad_column(tz_list, 20, 0);
  lv_obj_set_style_pad_bottom(tz_list, 20, 0);
  lv_obj_add_flag(tz_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(tz_list, LV_DIR_VER);
  for (lv_obj_t*& c : tz_col) {
    c = lv_obj_create(tz_list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 590, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
  }
}
