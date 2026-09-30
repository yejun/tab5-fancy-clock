// Fancy Clock for the M5Stack Tab5 (ESP32-P4, 1280x720 MIPI-DSI)
//
//  * LVGL 9 does all the drawing, M5GFX/M5Unified provide the display, touch, RTC and battery.
//  * The heavy, static artwork (gradient, glow, dial, ticks, numerals, calendar card) is rendered once
//    into an LVGL snapshot image, so the 30 fps sweeping second hand only has to blit + redraw hands.
//  * Tap the clock face / background to cycle themes, tap the big digits to switch 12h/24h.
//  * Serial (115200, USB-C port): "T YYYY-MM-DD HH:MM:SS" sets the RTC, "P" sends a screenshot,
//    "C" next theme, "M" toggle 12/24h, "S" status.  See tools/*.py.

#include <M5Unified.h>
#include <lvgl.h>
#include <misc/cache/instance/lv_image_cache.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include "esp32-hal-hosted.h"
#include "fonts.h"
#include "types.h"

// ----------------------------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------------------------
static constexpr int      ROTATION   = 1;    // 1 = landscape, 3 = landscape upside-down
static constexpr uint8_t  BRIGHTNESS = 200;  // 0..255

static constexpr int SCR_W = 1280, SCR_H = 720;
static constexpr int CX = 340, CY = 360;     // dial centre
static constexpr int PX = 724;               // left edge of the right-hand panel
static constexpr int PW = 516;               // width of the right-hand panel

static const Theme THEMES[] = {
  {0x0A1030, 0x24104E, 0x2EE6C5, 0xFF6AD5},  // Aurora
  {0x1E0A1E, 0x4A1530, 0xFF8A5B, 0xFFD166},  // Sunset
  {0x04121F, 0x0B3E5C, 0x4CC9F0, 0xA7C0FF},  // Ocean
  {0x0B0B0D, 0x1E1E24, 0xF2F2F7, 0xFF453A},  // Graphite
};
static constexpr int N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);

static const char* const MONTHS[]   = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
static const char* const WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// ----------------------------------------------------------------------------------------------
// Calendar maths (proleptic Gregorian, days since 1970-01-01)
// ----------------------------------------------------------------------------------------------
static int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

static int days_in_month(int y, int m) {
  static const uint8_t dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return dm[m - 1];
}

static int weekday_of(int y, int m, int d) {  // 0 = Sunday
  int64_t z = days_from_civil(y, m, d);
  return (int)(((z % 7) + 11) % 7);  // 1970-01-01 was a Thursday (4)
}

// ----------------------------------------------------------------------------------------------
// Time keeping: the RTC only has 1 s resolution, so we phase-lock a millisecond clock to its ticks.
// ----------------------------------------------------------------------------------------------
static int64_t g_base_ms = 0;      // local-time epoch ms at mono_ms() == 0
static bool    g_synced  = false;
static bool    g_resync  = true;   // request a phase-lock to the RTC's next second boundary
static int     g_corr_n = 0, g_corr_max_ms = 0, g_reject_n = 0;

static inline int64_t mono_ms() { return esp_timer_get_time() / 1000; }

static int64_t rtc_epoch_s(const m5::rtc_datetime_t& dt) {
  return days_from_civil(dt.date.year, dt.date.month, dt.date.date) * 86400LL +
         dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
}

static bool ntp_active();

// The RTC only has 1 s resolution. To phase-lock our ms clock to it we poll quickly until the seconds
// field changes.  A boundary is only trusted if the two polls around it were close together, so a
// stalled loop can never move the clock.  After a lock we leave the RTC alone for 10 minutes.
static void poll_rtc() {
  static bool syncing = false, have_prev = false;
  static int64_t last_poll = 0, prev_done = 0, next_sync = 0, sync_start = 0;
  static int prev_sec = -1;
  const int64_t t0 = mono_ms();
  if (ntp_active()) return;                  // NTP is the master clock

  if (!syncing) {
    if (!g_resync && t0 < next_sync) return;
    syncing = true; have_prev = false; g_resync = false; sync_start = t0;
  }
  if (t0 - last_poll < 4) return;
  last_poll = t0;

  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) return;
  const int64_t t1 = mono_ms();
  const int64_t sec = rtc_epoch_s(dt);
  const int64_t read_at = (t0 + t1) / 2;

  if (!g_synced && !have_prev) g_base_ms = sec * 1000 + 500 - t1;  // provisional, until we see a boundary

  if (have_prev && dt.time.seconds != prev_sec) {
    if (read_at - prev_done <= 15) {  // tight window -> boundary is known to within ~8 ms
      const int64_t cand = sec * 1000 - (prev_done + read_at) / 2;
      const int d = (int)(cand - g_base_ms);
      if (g_synced) { g_corr_n++; if (abs(d) > g_corr_max_ms) g_corr_max_ms = abs(d); }
      g_base_ms = cand;
      g_synced = true;
      syncing = false;
      next_sync = t1 + 10 * 60 * 1000;
      return;
    }
    g_reject_n++;  // a poll was delayed; wait for the next boundary
  }
  have_prev = true;
  prev_sec = dt.time.seconds;
  prev_done = t1;
  if (t1 - sync_start > 4000) { syncing = false; next_sync = t1 + 5000; }  // give up for now
}

// ----------------------------------------------------------------------------------------------
// Network time (WiFi + SNTP).  A background task on core 0 connects, syncs, and disconnects again;
// the UI thread only ever sees a (utc_ms, mono_ms) pair handed over through a flag.
// ----------------------------------------------------------------------------------------------
static constexpr int64_t NTP_RESYNC_MS = 6LL * 60 * 60 * 1000;   // re-sync every 6 hours (WiFi is off in between)
static constexpr int64_t NTP_VALID_MS  = 24LL * 60 * 60 * 1000;  // fall back to the RTC if no sync for 24 h

static String g_tz = "PST8PDT,M3.2.0,M11.1.0";  // POSIX TZ string (US Pacific); change with the Z command
static String g_ssid, g_pass;
static char g_net_state[64] = "no wifi configured";
static volatile int  g_net_cmd = 0;          // 1 = sync now, 2 = scan (consumed by the network task)
static volatile bool g_sntp_done = false;
static volatile bool g_ntp_ready = false;    // task -> loop hand-over
static volatile int64_t g_ntp_utc_ms = 0, g_ntp_mono_ms = 0;
static bool    g_ntp_valid = false;
static int64_t g_utc_base_ms = 0;            // UTC epoch ms at mono_ms() == 0
static int64_t g_ntp_last_mono = 0;
static int64_t g_rtc_write_utc_s = 0;        // whole UTC second at which to write the RTC (0 = none)
static int     g_ntp_count = 0;

static bool ntp_active() {
  static bool was = false;
  const bool now = g_ntp_valid && (mono_ms() - g_ntp_last_mono) < NTP_VALID_MS;
  if (was && !now) g_resync = true;          // NTP went stale: re-lock to the RTC
  was = now;
  return now;
}

static void sntp_cb(struct timeval*) { g_sntp_done = true; }

static void net_state(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void net_state(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_net_state, sizeof(g_net_state), fmt, ap);
  va_end(ap);
  Serial.printf("net: %s\n", g_net_state);
}

// Switch the radio off completely: drop the connection and shut down the ESP-Hosted link to the C6 chip,
// which also gives its internal-RAM buffer pool back.  The next WiFi.mode() brings it up again.
static void net_radio_off() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  hostedDeinitWiFi();
}

static constexpr int MAX_SCAN = 24;
static ScanEntry g_scan[MAX_SCAN];           // filled by the network task, read by the UI after g_scan_ready
static volatile int  g_scan_n = 0;
static volatile bool g_scan_ready = false;

static void net_scan() {
  net_state("scanning...");
  WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  int cnt = 0;
  static ScanEntry tmp[MAX_SCAN];
  for (int i = 0; i < n; i++) {         // de-duplicate by SSID (keep the strongest), skip hidden networks
    const String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    int j = 0;
    while (j < cnt && strcmp(tmp[j].ssid, ss.c_str()) != 0) j++;
    const int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (j == cnt) {
      if (cnt >= MAX_SCAN) continue;
      strlcpy(tmp[cnt].ssid, ss.c_str(), sizeof(tmp[cnt].ssid));
      tmp[cnt].rssi = rssi;
      tmp[cnt].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      cnt++;
    } else if (rssi > tmp[j].rssi) {
      tmp[j].rssi = rssi;
    }
  }
  for (int a = 1; a < cnt; a++)         // strongest first
    for (int b = a; b > 0 && tmp[b].rssi > tmp[b - 1].rssi; b--) { ScanEntry x = tmp[b]; tmp[b] = tmp[b - 1]; tmp[b - 1] = x; }
  memcpy(g_scan, tmp, sizeof(ScanEntry) * cnt);
  g_scan_n = cnt;
  for (int i = 0; i < cnt; i++) Serial.printf("  %-32s %ddBm %s\n", g_scan[i].ssid, (int)g_scan[i].rssi, g_scan[i].secure ? "secured" : "open");
  WiFi.scanDelete();
  net_radio_off();
  g_scan_ready = true;
  net_state("scan done: %d networks", cnt);
}

// Returns true on success and hands the time to the UI thread.
static bool net_sync() {
  if (g_ssid.isEmpty()) { net_state("no wifi configured"); return false; }
  net_state("connecting to %s", g_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  for (int i = 0; i < 100 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(200));
  if (WiFi.status() != WL_CONNECTED) {
    net_state("wifi connect failed (status %d)", (int)WiFi.status());
    net_radio_off();
    return false;
  }
  net_state("connected %s rssi %d, syncing time", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

  g_sntp_done = false;
  sntp_set_time_sync_notification_cb(sntp_cb);
  configTzTime(g_tz.c_str(), "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 150 && !g_sntp_done; i++) vTaskDelay(pdMS_TO_TICKS(100));

  bool ok = false;
  if (g_sntp_done) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    g_ntp_mono_ms = mono_ms();
    g_ntp_utc_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    g_ntp_ready = true;
    ok = true;
    net_state("time synced");
  } else {
    net_state("NTP timeout");
  }
  net_radio_off();
  return ok;
}

static void net_task(void*) {
  int64_t next = 0;   // mono time of the next automatic attempt
  int fails = 0;
  // After a failure (wrong password, network away) back off so we don't burn battery retrying.
  static const int64_t BACKOFF_MS[] = {60000, 120000, 300000, 600000, 1800000, 3600000};
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    const int cmd = g_net_cmd;
    if (cmd == 2) { g_net_cmd = 0; net_scan(); continue; }
    if (cmd == 1 || (!g_ssid.isEmpty() && mono_ms() >= next)) {
      g_net_cmd = 0;
      if (net_sync()) { fails = 0; next = mono_ms() + NTP_RESYNC_MS; }
      else { next = mono_ms() + BACKOFF_MS[fails < 5 ? fails : 5]; fails++; }
    }
  }
}

// Loop-thread side: adopt a new NTP fix and schedule the RTC backup write for a whole-second boundary.
static void net_apply() {
  if (!g_ntp_ready) return;
  g_ntp_ready = false;
  const int64_t old_base = g_utc_base_ms;
  const bool had = g_ntp_valid;
  g_utc_base_ms = g_ntp_utc_ms - g_ntp_mono_ms;
  g_ntp_valid = true;
  g_ntp_last_mono = mono_ms();
  g_ntp_count++;
  g_rtc_write_utc_s = (g_utc_base_ms + mono_ms()) / 1000 + 2;
  if (had) Serial.printf("ntp: clock corrected by %d ms\n", (int)(g_utc_base_ms - old_base));
}

static Now now_local() {
  if (ntp_active()) {
    const int64_t utc = g_utc_base_ms + mono_ms();
    const time_t sec = (time_t)(utc / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    Now n;
    n.y = tmv.tm_year + 1900; n.mo = tmv.tm_mon + 1; n.d = tmv.tm_mday;
    n.h = tmv.tm_hour; n.mi = tmv.tm_min; n.s = tmv.tm_sec; n.ms = (int)(utc % 1000); n.wd = tmv.tm_wday;
    return n;
  }
  const int64_t t = g_base_ms + mono_ms();
  const int64_t days = t / 86400000LL;
  const int64_t rem = t - days * 86400000LL;
  Now n;
  civil_from_days(days, n.y, n.mo, n.d);
  n.h = (int)(rem / 3600000);
  n.mi = (int)((rem / 60000) % 60);
  n.s = (int)((rem / 1000) % 60);
  n.ms = (int)(rem % 1000);
  n.wd = (int)(((days % 7) + 11) % 7);
  return n;
}

static void set_rtc(int y, int mo, int d, int h, int mi, int s) {
  m5::rtc_datetime_t dt;
  dt.date.year = y; dt.date.month = mo; dt.date.date = d;
  dt.date.weekDay = weekday_of(y, mo, d);
  dt.time.hours = h; dt.time.minutes = mi; dt.time.seconds = s;
  M5.Rtc.setDateTime(dt);
  g_synced = false;
  g_resync = true;
}

// Keep the battery-backed RTC in step with NTP: write it exactly on a whole-second boundary so an
// offline reboot starts within a few ms of the right time.
static void rtc_backup_write() {
  if (!g_rtc_write_utc_s) return;
  const int64_t utc = g_utc_base_ms + mono_ms();
  if (utc < g_rtc_write_utc_s * 1000) return;
  if (utc < g_rtc_write_utc_s * 1000 + 100) {
    const time_t sec = (time_t)g_rtc_write_utc_s;
    struct tm t;
    localtime_r(&sec, &t);
    set_rtc(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    Serial.printf("rtc: written from NTP %04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
  }
  g_rtc_write_utc_s = 0;
}

// Initialise the RTC from the build time when the firmware changed (or the RTC was never set).
static void init_rtc_from_build(Preferences& prefs) {
  static const char* const mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char m[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  const int mo = (int)((strstr(mon, m) - mon) / 3) + 1;
  int d = 0, y = 0, h = 0, mi = 0, s = 0;
  sscanf(__DATE__ + 4, "%d %d", &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);

  m5::rtc_datetime_t cur;
  M5.Rtc.getDateTime(&cur);
  const String stamp = String(__DATE__) + " " + __TIME__;
  if (prefs.getString("build", "") != stamp || cur.date.year < 2025) {
    set_rtc(y, mo, d, h, mi, s);
    prefs.putString("build", stamp);
    Serial.printf("RTC set from build time %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
  }
}

// ----------------------------------------------------------------------------------------------
// LVGL <-> M5GFX glue
// ----------------------------------------------------------------------------------------------
static lv_display_t* g_disp = nullptr;
static uint32_t g_flush_us = 0, g_flush_px = 0, g_flush_n = 0;

static void flush_cb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
  const int w = lv_area_get_width(a), h = lv_area_get_height(a);
  const int64_t t0 = esp_timer_get_time();
  M5.Display.startWrite();
  M5.Display.pushImage(a->x1, a->y1, w, h, (const lgfx::rgb565_t*)px);
  M5.Display.endWrite();
  g_flush_us += (uint32_t)(esp_timer_get_time() - t0);
  g_flush_px += (uint32_t)(w * h);
  g_flush_n++;
  lv_display_flush_ready(disp);
}

static void log_cb(lv_log_level_t, const char* msg) { Serial.print(msg); }

// The ESP-Hosted driver for the WiFi chip needs a big chunk of *internal* RAM for its SDIO buffer pool,
// so the LVGL draw buffers live in PSRAM (internal RAM is the scarce resource here).
static void* alloc_draw_buf(size_t bytes) {
  void* p = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

// ----------------------------------------------------------------------------------------------
// UI state
// ----------------------------------------------------------------------------------------------
static Preferences prefs;
static int  g_theme = 0;
static bool g_24h = true;

static lv_font_t *f_digits, *f_sec, *f_date, *f_head, *f_num, *f_cal, *f_small;

// Objects that need re-colouring when the theme changes
static lv_obj_t *bg_img, *day_arc, *bar_sec, *lbl_greet, *lbl_time, *lbl_ampm, *lbl_sec, *lbl_date;
static lv_obj_t *lbl_wifi, *hub_obj, *lbl_batt, *lbl_month, *hub_in, *sec_tip, *today_mark;
static lv_obj_t *lbl_wd[7], *lbl_day[42];

static LineObj hand_h, hand_m, hand_s, lume_h, lume_m, sh_h, sh_m, sh_s;

static lv_draw_buf_t* g_static_bufs[4];  // one pre-rendered background per theme
static int g_render_theme = 0;
static lv_grad_dsc_t g_grad_glow_a, g_grad_glow_b, g_grad_face;

// ----------------------------------------------------------------------------------------------
// Small widget helpers
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

static void set_radial(lv_grad_dsc_t* g, uint32_t c0, lv_opa_t o0, uint32_t c1, lv_opa_t o1) {
  const lv_color_t cols[2] = {lv_color_hex(c0), lv_color_hex(c1)};
  const lv_opa_t opas[2] = {o0, o1};
  const uint8_t fr[2] = {0, 255};
  lv_grad_init_stops(g, cols, opas, fr, 2);
  lv_grad_radial_init(g, LV_PCT(50), LV_PCT(50), LV_PCT(100), LV_PCT(50), LV_GRAD_EXTEND_PAD);
}

// ----------------------------------------------------------------------------------------------
// Static artwork (rendered once per theme into a snapshot)
// ----------------------------------------------------------------------------------------------
static void ticks_draw_cb(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
  const Theme& th = THEMES[g_render_theme];

  for (int i = 0; i < 60; i++) {
    const bool major = (i % 5) == 0;
    const float ang = i * 6.0f * (float)M_PI / 180.0f;
    const float sx = sinf(ang), sy = -cosf(ang);
    const float r1 = 282.0f, r0 = major ? 250.0f : 268.0f;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = d.round_end = 1;
    d.width = major ? 7 : 3;
    d.color = major ? lv_color_hex(th.acc1) : lv_color_white();
    d.opa = major ? LV_OPA_COVER : LV_OPA_40;
    d.p1.x = cx + sx * r0; d.p1.y = cy + sy * r0;
    d.p2.x = cx + sx * r1; d.p2.y = cy + sy * r1;
    lv_draw_line(layer, &d);
  }
}

static lv_obj_t* build_static_screen(const Theme& th) {
  lv_obj_t* s = lv_obj_create(nullptr);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, SCR_W, SCR_H);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s, lv_color_hex(th.bg_top), 0);
  lv_obj_set_style_bg_grad_color(s, lv_color_hex(th.bg_bot), 0);
  lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);

  // Soft glows
  set_radial(&g_grad_glow_a, th.acc2, 70, th.acc2, 0);
  lv_obj_t* glow = mk(s, CX - 520, CY - 520, 1040, 1040);
  lv_obj_set_style_bg_grad(glow, &g_grad_glow_a, 0);

  set_radial(&g_grad_glow_b, th.acc1, 46, th.acc1, 0);
  lv_obj_t* glow2 = mk(s, PX + PW - 340, -260, 800, 800);
  lv_obj_set_style_bg_grad(glow2, &g_grad_glow_b, 0);

  // Dial face
  set_radial(&g_grad_face, 0xFFFFFF, 26, 0x000000, 96);
  lv_obj_t* face = mk(s, CX - 300, CY - 300, 600, 600);
  lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_grad(face, &g_grad_face, 0);
  lv_obj_set_style_border_width(face, 2, 0);
  lv_obj_set_style_border_color(face, lv_color_white(), 0);
  lv_obj_set_style_border_opa(face, LV_OPA_20, 0);
  lv_obj_set_style_shadow_width(face, 60, 0);
  lv_obj_set_style_shadow_color(face, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(face, LV_OPA_50, 0);
  lv_obj_set_style_shadow_offset_y(face, 18, 0);

  // Inner decorative rings
  lv_obj_t* ring = mk(s, CX - 130, CY - 130, 260, 260);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 1, 0);
  lv_obj_set_style_border_color(ring, lv_color_white(), 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_10, 0);

  // Ticks
  lv_obj_t* ticks = mk(s, CX - 330, CY - 330, 660, 660);
  lv_obj_add_event_cb(ticks, ticks_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // Numerals
  lv_obj_update_layout(s);
  for (int i = 1; i <= 12; i++) {
    char t[4];
    snprintf(t, sizeof(t), "%d", i);
    const float ang = i * 30.0f * (float)M_PI / 180.0f;
    const float rx = CX + sinf(ang) * 212.0f, ry = CY - cosf(ang) * 212.0f;
    lv_obj_t* l = mk_label(s, f_num, i % 3 == 0 ? th.acc1 : 0xFFFFFF, i % 3 == 0 ? LV_OPA_COVER : LV_OPA_80,
                           t, 0, 0, 100, LV_TEXT_ALIGN_CENTER);
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, (int)rx - 50, (int)ry - lv_obj_get_height(l) / 2);
  }

  // Brand
  lv_obj_t* brand = mk_label(s, f_small, 0xFFFFFF, LV_OPA_40, "TAB5", 0, CY - 128, 200, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_style_text_letter_space(brand, 8, 0);
  lv_obj_set_x(brand, CX - 100);

  // Calendar card (glass)
  lv_obj_t* card = mk(s, PX, 392, PW, 296);
  lv_obj_set_style_radius(card, 30, 0);
  lv_obj_set_style_bg_color(card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_10, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_white(), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_20, 0);

  lv_obj_update_layout(s);
  return s;
}

// RGB565 bands badly on smooth gradients, so the artwork is rendered in 32 bit and
// ordered-dithered (4x4 Bayer) down to the RGB565 image that is actually displayed.
static lv_draw_buf_t* dither_to_rgb565(const lv_draw_buf_t* src) {
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

// Render (once) the background for theme `idx`; later calls are free.
static void render_static(int idx) {
  if (g_static_bufs[idx]) return;
  const int64_t t0 = esp_timer_get_time();
  g_render_theme = idx;
  lv_obj_t* s = build_static_screen(THEMES[idx]);
  lv_draw_buf_t* full = lv_snapshot_take(s, LV_COLOR_FORMAT_XRGB8888);
  lv_obj_delete(s);
  g_static_bufs[idx] = full ? dither_to_rgb565(full) : nullptr;
  if (full) lv_draw_buf_destroy(full);
  Serial.printf("theme %d artwork rendered in %d ms%s\n", idx, (int)((esp_timer_get_time() - t0) / 1000),
                g_static_bufs[idx] ? "" : " (FAILED)");
}

static void show_static() {
  if (!g_static_bufs[g_theme]) render_static(g_theme);
  if (!g_static_bufs[g_theme]) return;
  lv_image_set_src(bg_img, g_static_bufs[g_theme]);
  lv_obj_invalidate(bg_img);
}

// ----------------------------------------------------------------------------------------------
// Dynamic widgets
// ----------------------------------------------------------------------------------------------
static void make_line(LineObj& L, lv_obj_t* parent, int width, uint32_t color, lv_opa_t opa) {
  L.o = lv_line_create(parent);
  lv_obj_remove_style_all(L.o);
  lv_obj_set_style_line_width(L.o, width, 0);
  lv_obj_set_style_line_color(L.o, lv_color_hex(color), 0);
  lv_obj_set_style_line_opa(L.o, opa, 0);
  lv_obj_set_style_line_rounded(L.o, true, 0);
  L.p[0] = {0, 0};
  L.p[1] = {1, 1};
  lv_line_set_points_mutable(L.o, L.p, 2);
}

// Place a line between two absolute points, keeping its object bounds tight so redraws stay small.
static void place_line(LineObj& L, float x1, float y1, float x2, float y2) {
  const int pad = lv_obj_get_style_line_width(L.o, LV_PART_MAIN) / 2 + 2;
  const int minx = (int)floorf(fminf(x1, x2)) - pad, miny = (int)floorf(fminf(y1, y2)) - pad;
  const int maxx = (int)ceilf(fmaxf(x1, x2)) + pad, maxy = (int)ceilf(fmaxf(y1, y2)) + pad;
  lv_obj_set_pos(L.o, minx, miny);
  lv_obj_set_size(L.o, maxx - minx, maxy - miny);
  L.p[0] = {(lv_value_precise_t)(x1 - minx), (lv_value_precise_t)(y1 - miny)};
  L.p[1] = {(lv_value_precise_t)(x2 - minx), (lv_value_precise_t)(y2 - miny)};
  lv_line_set_points_mutable(L.o, L.p, 2);
}

static void place_hand(LineObj& L, float deg, float r0, float r1, float ox = 0, float oy = 0) {
  const float a = deg * (float)M_PI / 180.0f;
  const float sx = sinf(a), sy = -cosf(a);
  place_line(L, CX + sx * r0 + ox, CY + sy * r0 + oy, CX + sx * r1 + ox, CY + sy * r1 + oy);
}

static void set_hands_hm(const Now& n, bool hour_too) {
  const float min_f = n.mi + n.s / 60.0f;
  const float hr_f = (n.h % 12) + min_f / 60.0f;
  const float ah = hr_f * 30.0f, am = min_f * 6.0f;
  if (hour_too) {
    place_hand(sh_h, ah, 0, 150, 6, 9);
    place_hand(hand_h, ah, 0, 150);
    place_hand(lume_h, ah, 42, 136);
  }
  place_hand(sh_m, am, 0, 224, 6, 9);
  place_hand(hand_m, am, 0, 224);
  place_hand(lume_m, am, 42, 210);
}

static void set_hand_s(const Now& n) {
  const float a = (n.s + n.ms / 1000.0f) * 6.0f;
  place_hand(sh_s, a, -52, 246, 8, 12);
  place_hand(hand_s, a, -52, 246);
  const float rad = a * (float)M_PI / 180.0f;
  lv_obj_set_pos(sec_tip, (int)(CX + sinf(rad) * 246) - 11, (int)(CY - cosf(rad) * 246) - 11);
}

static void apply_dynamic_theme() {
  const Theme& th = THEMES[g_theme];
  lv_obj_set_style_line_color(lume_h.o, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_line_color(lume_m.o, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_line_color(hand_s.o, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_bg_color(sec_tip, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_shadow_color(sec_tip, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_bg_color(hub_in, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_arc_color(day_arc, lv_color_hex(th.acc1), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(day_arc, lv_color_hex(th.acc1), LV_PART_KNOB);
  lv_obj_set_style_shadow_color(day_arc, lv_color_hex(th.acc1), LV_PART_KNOB);
  lv_obj_set_style_bg_color(bar_sec, lv_color_hex(th.acc1), LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_color(bar_sec, lv_color_hex(th.acc2), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(lbl_greet, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_text_color(lbl_ampm, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_text_color(lbl_sec, lv_color_hex(th.acc2), 0);
  lv_obj_set_style_bg_color(today_mark, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_shadow_color(today_mark, lv_color_hex(th.acc1), 0);
  lv_obj_set_style_text_color(lbl_month, lv_color_hex(th.acc1), 0);
}

static void build_dynamic_ui() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  bg_img = lv_image_create(scr);
  lv_obj_set_pos(bg_img, 0, 0);

  // Day-progress ring with a glowing knob
  day_arc = lv_arc_create(scr);
  lv_obj_remove_style_all(day_arc);
  lv_obj_set_size(day_arc, 660, 660);
  lv_obj_set_pos(day_arc, CX - 330, CY - 330);
  lv_obj_remove_flag(day_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_rotation(day_arc, 270);
  lv_arc_set_bg_angles(day_arc, 0, 360);
  lv_arc_set_range(day_arc, 0, 1440);
  lv_arc_set_value(day_arc, 0);
  lv_obj_set_style_arc_width(day_arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(day_arc, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(day_arc, LV_OPA_10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(day_arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(day_arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(day_arc, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(day_arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(day_arc, 6, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(day_arc, 24, LV_PART_KNOB);
  lv_obj_set_style_shadow_opa(day_arc, LV_OPA_70, LV_PART_KNOB);

  // Hands
  make_line(sh_h, scr, 20, 0x000000, LV_OPA_40);
  make_line(sh_m, scr, 14, 0x000000, LV_OPA_40);
  make_line(sh_s, scr, 5, 0x000000, LV_OPA_30);
  make_line(hand_h, scr, 18, 0xF4F6FF, LV_OPA_COVER);
  make_line(lume_h, scr, 6, 0xFFFFFF, LV_OPA_COVER);
  make_line(hand_m, scr, 12, 0xF4F6FF, LV_OPA_COVER);
  make_line(lume_m, scr, 4, 0xFFFFFF, LV_OPA_COVER);
  make_line(hand_s, scr, 4, 0xFFFFFF, LV_OPA_COVER);

  sec_tip = mk(scr, 0, 0, 22, 22);
  lv_obj_set_style_radius(sec_tip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(sec_tip, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(sec_tip, 22, 0);
  lv_obj_set_style_shadow_opa(sec_tip, LV_OPA_60, 0);

  lv_obj_t* hub = hub_obj = mk(scr, CX - 19, CY - 19, 38, 38);
  lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hub, lv_color_hex(0xF4F6FF), 0);
  lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(hub, 14, 0);
  lv_obj_set_style_shadow_opa(hub, LV_OPA_40, 0);
  lv_obj_set_style_shadow_color(hub, lv_color_black(), 0);
  lv_obj_set_style_shadow_offset_y(hub, 4, 0);
  hub_in = mk(scr, CX - 8, CY - 8, 16, 16);
  lv_obj_set_style_radius(hub_in, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(hub_in, LV_OPA_COVER, 0);

  // Right-hand panel: greeting, time, date, seconds bar
  lbl_greet = mk_label(scr, f_head, 0xFFFFFF, LV_OPA_COVER, "", PX + 4, 30, 400);
  lv_obj_set_style_text_letter_space(lbl_greet, 5, 0);
  lv_obj_set_width(lbl_greet, 330);  // keep clear of the WiFi/battery icons on the right
  lbl_batt = mk_label(scr, &lv_font_montserrat_20, 0xFFFFFF, LV_OPA_70, "", PX + PW - 190, 34, 190, LV_TEXT_ALIGN_RIGHT);

  lbl_wifi = mk_label(scr, &lv_font_montserrat_20, 0xFFFFFF, LV_OPA_30, LV_SYMBOL_WIFI, PX + PW - 172, 34, 40);

  lbl_time = mk_label(scr, f_digits, 0xFFFFFF, LV_OPA_COVER, "00:00", PX - 6, 36, 470);
  lbl_ampm = mk_label(scr, f_sec, 0xFFFFFF, LV_OPA_COVER, "", PX + PW - 60, 104, 70);
  lbl_date = mk_label(scr, f_date, 0xFFFFFF, LV_OPA_90, "", PX + 4, 252, PW);
  lbl_sec = mk_label(scr, f_small, 0xFFFFFF, LV_OPA_COVER, "", PX + PW - 90, 322, 90, LV_TEXT_ALIGN_RIGHT);

  bar_sec = lv_bar_create(scr);
  lv_obj_remove_style_all(bar_sec);
  lv_obj_set_pos(bar_sec, PX + 4, 334);
  lv_obj_set_size(bar_sec, PW - 110, 8);
  lv_bar_set_range(bar_sec, 0, 60000);
  lv_obj_set_style_radius(bar_sec, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_sec, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar_sec, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_radius(bar_sec, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar_sec, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_dir(bar_sec, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

  // Calendar
  lbl_month = mk_label(scr, f_head, 0xFFFFFF, LV_OPA_COVER, "", PX + 26, 408, PW - 52);
  lv_obj_set_style_text_letter_space(lbl_month, 4, 0);
  const int cw = (PW - 40) / 7, gx = PX + 20;
  const char* wd_short[7] = {"S", "M", "T", "W", "T", "F", "S"};
  for (int c = 0; c < 7; c++)
    lbl_wd[c] = mk_label(scr, f_small, 0xFFFFFF, LV_OPA_50, wd_short[c], gx + c * cw, 452, cw, LV_TEXT_ALIGN_CENTER);

  today_mark = mk(scr, 0, 0, 32, 32);
  lv_obj_set_style_radius(today_mark, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(today_mark, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(today_mark, 18, 0);
  lv_obj_set_style_shadow_opa(today_mark, LV_OPA_50, 0);
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
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      lv_obj_set_style_text_opa(l, LV_OPA_20, 0);
    } else if (day == n.d) {
      lv_obj_set_style_text_color(l, lv_color_hex(th.bg_top), 0);
      lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
      lv_obj_update_layout(l);
      lv_obj_set_pos(today_mark, lv_obj_get_x(l) + lv_obj_get_width(l) / 2 - 16,
                     lv_obj_get_y(l) + lv_obj_get_height(l) / 2 - 16);
      lv_obj_remove_flag(today_mark, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_set_style_text_color(l, weekend ? lv_color_hex(th.acc2) : lv_color_white(), 0);
      lv_obj_set_style_text_opa(l, weekend ? LV_OPA_90 : LV_OPA_80, 0);
    }
  }
  // keep the marker underneath its number
  lv_obj_move_foreground(today_mark);
  for (int i = 0; i < 42; i++) lv_obj_move_foreground(lbl_day[i]);
}

static void update_date(const Now& n) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s, %s %d", WEEKDAYS[n.wd], MONTHS[n.mo - 1], n.d);
  lv_label_set_text(lbl_date, buf);
  update_calendar(n);
}

static void update_minute(const Now& n) {
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
  lv_arc_set_value(day_arc, n.h * 60 + n.mi);

  const char* g = n.h < 5 ? "GOOD NIGHT" : n.h < 12 ? "GOOD MORNING" : n.h < 18 ? "GOOD AFTERNOON" : "GOOD EVENING";
  lv_label_set_text(lbl_greet, g);
}

static void update_battery() {
  // With no battery fitted the fuel gauge flips between bogus samples (~4.2 V / level 0) and a plausible one.
  // A real 2-cell pack is 6-8.4 V, so ignore anything lower and keep the last good reading on screen.
  if (M5.Power.getBatteryVoltage() < 5500) return;
  const int lvl = M5.Power.getBatteryLevel();
  // WiFi icon: bright in the accent colour while the time is NTP-synced, dim otherwise (tap it to set up WiFi)
  const bool synced = ntp_active();
  lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(synced ? THEMES[g_theme].acc1 : 0xFFFFFF), 0);
  lv_obj_set_style_text_opa(lbl_wifi, synced ? LV_OPA_COVER : LV_OPA_30, 0);
  if (lvl < 0) { lv_label_set_text(lbl_batt, ""); return; }
  const char* icon = lvl > 85 ? LV_SYMBOL_BATTERY_FULL : lvl > 60 ? LV_SYMBOL_BATTERY_3 : lvl > 35 ? LV_SYMBOL_BATTERY_2
                   : lvl > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
  const bool chg = M5.Power.isCharging() == m5::Power_Class::is_charging;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s%s  %d%%", chg ? LV_SYMBOL_CHARGE "  " : "", icon, lvl);
  lv_label_set_text(lbl_batt, buf);
}

static bool g_dirty_all = true;

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
  static int last_s = -1, last_mi = -1, last_d = -1;
  const Now n = now_local();

  set_hand_s(n);
  lv_bar_set_value(bar_sec, n.s * 1000 + n.ms, LV_ANIM_OFF);

  const bool force = g_dirty_all;
  if (g_dirty_all) { last_s = last_mi = last_d = -1; g_dirty_all = false; }
  if (n.s != last_s) {
    last_s = n.s;
    // The hour/minute hands move well under a pixel per second; repainting them (plus shadows) every second
    // costs more than a frame, so step the minute hand every 3 s and the hour hand every 30 s.
    if (force || n.s % 3 == 0) set_hands_hm(n, force || n.s % 30 == 0);
    char b[8];
    snprintf(b, sizeof(b), "%02d s", n.s);
    lv_label_set_text(lbl_sec, b);
  }
  if (n.mi != last_mi) { last_mi = n.mi; update_minute(n); }
  if (n.d != last_d) { last_d = n.d; update_date(n); }
}

// ----------------------------------------------------------------------------------------------
// WiFi settings screen: scan -> pick a network -> type the password on the on-screen keyboard.
// Lives on LVGL's top layer; while it is open the clock animation is paused.
// ----------------------------------------------------------------------------------------------
static lv_obj_t *wifi_ui, *wl_list, *wl_status, *wl_page_list, *wl_page_pw, *wl_ta, *wl_kb, *wl_pw_title;
static lv_obj_t *wl_btn_a_lbl, *wl_btn_connect, *wl_btn_show_lbl;
static lv_timer_t *g_fast_timer = nullptr, *g_wifi_timer = nullptr;
static bool    g_wifi_open = false;
static int     g_wl_page = 0;                 // 0 = network list, 1 = password entry
static int     g_wl_count_at_connect = -1;    // g_ntp_count when the user pressed Connect (-1 = not waiting)
static int64_t g_wl_close_at = 0;
static char    g_sel_ssid[33];

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
  lv_timer_resume(g_fast_timer);
  g_dirty_all = true;
  lv_obj_invalidate(lv_screen_active());
}

static void wl_connect(const char* ssid, const char* pass) {
  g_ssid = ssid;
  g_pass = pass;
  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  g_wl_count_at_connect = g_ntp_count;
  g_wl_close_at = 0;
  g_net_cmd = 1;
  char b[80];
  snprintf(b, sizeof(b), "Connecting to %s ...", ssid);
  lv_label_set_text(wl_status, b);
  wl_show_page(0);
}

static void wl_select(int idx) {
  if (idx < 0 || idx >= g_scan_n) return;
  strlcpy(g_sel_ssid, g_scan[idx].ssid, sizeof(g_sel_ssid));
  if (!g_scan[idx].secure) { wl_connect(g_sel_ssid, ""); return; }
  char b[80];
  snprintf(b, sizeof(b), "Password for  %s", g_sel_ssid);
  lv_label_set_text(wl_pw_title, b);
  lv_textarea_set_text(wl_ta, "");
  lv_textarea_set_password_mode(wl_ta, true);
  lv_label_set_text(wl_btn_show_lbl, "Show");
  wl_show_page(1);
}

static void wl_row_cb(lv_event_t* e) { wl_select((int)(intptr_t)lv_event_get_user_data(e)); }

static void wl_rebuild_list() {
  lv_obj_clean(wl_list);
  const int n = g_scan_n;
  if (n == 0) {
    lv_obj_t* l = lv_label_create(wl_list);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A93B8), 0);
    lv_label_set_text(l, "No networks found - press Scan");
    return;
  }
  for (int i = 0; i < n; i++) {
    const ScanEntry& s = g_scan[i];
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
  g_net_cmd = 2;
  lv_label_set_text(wl_status, "Scanning ...");
}
static void wl_close_cb(lv_event_t*) { wifi_close(); }
static void wl_show_cb(lv_event_t*) {
  const bool pw = !lv_textarea_get_password_mode(wl_ta);
  lv_textarea_set_password_mode(wl_ta, pw);
  lv_label_set_text(wl_btn_show_lbl, pw ? "Show" : "Hide");
}
static void wl_connect_cb(lv_event_t*) { wl_connect(g_sel_ssid, lv_textarea_get_text(wl_ta)); }
static void wl_kb_cb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) wl_connect(g_sel_ssid, lv_textarea_get_text(wl_ta));
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
  g_net_cmd = 2;  // scan right away
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
// Display power & brightness: swipe left/right to dim/brighten, double-tap to turn the display off/on.
// ----------------------------------------------------------------------------------------------
static constexpr int MIN_BRIGHTNESS = 8;   // never fully black by swiping, so the screen can't be "lost"
static uint8_t g_bri = BRIGHTNESS;
static bool    g_disp_on = true;
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

// "Off" = backlight 0 and no animation work; the clock keeps its time in the background.
static void display_set(bool on) {
  if (on == g_disp_on) return;
  g_disp_on = on;
  if (on) {
    M5.Display.setBrightness(g_bri);
    lv_timer_resume(g_fast_timer);
    g_dirty_all = true;
  } else {
    M5.Display.setBrightness(0);
    lv_timer_pause(g_fast_timer);
  }
  Serial.printf("display %s\n", on ? "on" : "off");
}

// ----------------------------------------------------------------------------------------------
// Serial console (time sync, screenshots, debugging)
// ----------------------------------------------------------------------------------------------
static void send_screenshot() {
  // The WiFi screen lives on the top layer, so snapshot it directly while it is open.
  lv_draw_buf_t* snap = lv_snapshot_take(g_wifi_open ? wifi_ui : lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (!snap) { Serial.println("SNAPFAIL"); return; }
  const uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
  Serial.printf("SNAP %u %u\n", (unsigned)w, (unsigned)h);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* row = (const uint8_t*)snap->data + y * stride;
    size_t off = 0, len = w * 2;
    while (off < len) {
      const size_t k = Serial.write(row + off, min<size_t>(len - off, 512));
      if (k == 0) delay(1);
      off += k;
    }
  }
  Serial.print("\nENDSNAP\n");
  lv_draw_buf_destroy(snap);
}

static void handle_serial() {
  static char line[192];
  static size_t len = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    len = 0;
    if (!line[0]) continue;
    int y, mo, d, h, mi, s;
    switch (line[0]) {
      case 'T':
        if (sscanf(line + 1, " %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
          set_rtc(y, mo, d, h, mi, s);
          Serial.printf("OK time set %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
        } else {
          Serial.println("ERR usage: T YYYY-MM-DD HH:MM:SS");
        }
        break;
      case 'W': {  // "W ssid|password" stores WiFi credentials and syncs now; "W" alone clears them
        const char* arg = line + 1;
        while (*arg == ' ') arg++;
        const char* bar = strchr(arg, '|');
        g_ssid = bar ? String(arg).substring(0, bar - arg) : String(arg);
        g_pass = bar ? String(bar + 1) : String("");
        prefs.putString("ssid", g_ssid);
        prefs.putString("pass", g_pass);
        g_net_cmd = 1;
        Serial.printf("OK wifi ssid='%s' (%u char password)\n", g_ssid.c_str(), (unsigned)g_pass.length());
        break;
      }
      case 'Z': {  // "Z <POSIX TZ>", e.g. "Z PST8PDT,M3.2.0,M11.1.0"
        const char* arg = line + 1;
        while (*arg == ' ') arg++;
        if (*arg) {
          g_tz = arg;
          prefs.putString("tz", g_tz);
          setenv("TZ", g_tz.c_str(), 1);
          tzset();
        }
        Serial.printf("OK tz=%s\n", g_tz.c_str());
        break;
      }
      case 'U': if (g_wifi_open) wifi_close(); else wifi_open(); Serial.printf("OK wifi screen %s\n", g_wifi_open ? "open" : "closed"); break;
      case 'Y': wl_select(atoi(line + 1)); Serial.println("OK select"); break;  // test: pick scan result n
      case 'B': set_brightness(atoi(line + 1), true); g_bri_hide_at = mono_ms() + 60000; Serial.printf("OK brightness %d\n", g_bri); break;  // test: B <8-255>
      case 'D': display_set(!g_disp_on); Serial.printf("OK display %s\n", g_disp_on ? "on" : "off"); break;              // test: toggle display
      case 'N': g_net_cmd = 1; Serial.println("OK syncing"); break;
      case 'Q': g_net_cmd = 2; Serial.println("OK scanning"); break;
      case 'P': send_screenshot(); break;
      case 'C': set_theme(g_theme + 1); Serial.printf("OK theme %d\n", g_theme); break;
      case 'M':
        g_24h = !g_24h;
        prefs.putBool("h24", g_24h);
        g_dirty_all = true;
        Serial.printf("OK 24h=%d\n", g_24h);
        break;
      case 'S': {
        const Now n = now_local();
        Serial.printf("time %04d-%02d-%02d %02d:%02d:%02d.%03d synced=%d theme=%d 24h=%d\n", n.y, n.mo, n.d, n.h,
                      n.mi, n.s, n.ms, g_synced, g_theme, g_24h);
        Serial.printf("net: '%s' | ntp %s, %d syncs, last %ds ago | tz=%s | ssid=%s\n", g_net_state,
                      ntp_active() ? "ACTIVE" : "off", g_ntp_count,
                      g_ntp_valid ? (int)((mono_ms() - g_ntp_last_mono) / 1000) : -1, g_tz.c_str(),
                      g_ssid.isEmpty() ? "(none)" : g_ssid.c_str());
        Serial.printf("battery: level=%d voltage=%dmV current=%dmA charging=%d\n", (int)M5.Power.getBatteryLevel(),
                      (int)M5.Power.getBatteryVoltage(), (int)M5.Power.getBatteryCurrent(), (int)M5.Power.isCharging());
        Serial.printf("heap int=%u psram=%u | flushes=%u avg=%uus/flush %uus/kpx\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)g_flush_n,
                      g_flush_n ? g_flush_us / g_flush_n : 0, g_flush_px ? (uint32_t)((uint64_t)g_flush_us * 1000 / g_flush_px) : 0);
        g_flush_us = g_flush_px = g_flush_n = 0;
        Serial.printf("max ms: update=%u touch=%u rtc=%u serial=%u lvgl=%u | >50ms: %u %u %u %u %u | rtc corr=%d maxjump=%dms reject=%d\n",
                      st_upd.max_us / 1000, st_tap.max_us / 1000, st_rtc.max_us / 1000, st_ser.max_us / 1000,
                      st_lv.max_us / 1000, st_upd.slow, st_tap.slow, st_rtc.slow, st_ser.slow, st_lv.slow,
                      g_corr_n, g_corr_max_ms, g_reject_n);
        Serial.print("slow frames by 5s-position (slow/total):");
        for (int i = 0; i < 12; i++) Serial.printf(" %d:%u/%u", i * 5, g_slow_by_pos[i], g_frames_by_pos[i]);
        Serial.println();
        memset(g_slow_by_pos, 0, sizeof(g_slow_by_pos));
        memset(g_frames_by_pos, 0, sizeof(g_frames_by_pos));
        st_upd = st_tap = st_rtc = st_ser = st_lv = Stat();
        g_corr_n = g_corr_max_ms = g_reject_n = 0;
        break;
      }
      case 'K': g_auto_left = 20; g_auto_next = mono_ms() + 1000; Serial.println("OK stress: 20 theme switches / 3 s"); break;
      case 'H': {  // debug: bitmask of things to disable: 1 arc, 2 hand shadows, 4 tip glow, 8 hub shadow
        const int m = atoi(line + 1);
        if (m & 1) lv_obj_add_flag(day_arc, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(day_arc, LV_OBJ_FLAG_HIDDEN);
        LineObj* sh[3] = {&sh_h, &sh_m, &sh_s};
        for (auto* l : sh) { if (m & 2) lv_obj_add_flag(l->o, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(l->o, LV_OBJ_FLAG_HIDDEN); }
        lv_obj_set_style_shadow_width(sec_tip, (m & 4) ? 0 : 22, 0);
        lv_obj_set_style_shadow_width(hub_obj, (m & 8) ? 0 : 14, 0);
        Serial.printf("OK mask %d\n", m);
        break;
      }
      default: Serial.println("ERR unknown command"); break;
    }
  }
}

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
  if (x >= PX + PW - 260 && y < 90) {               // top-right corner (WiFi icon / battery)
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
  Serial.setTxTimeoutMs(0);  // never block the UI when nobody is reading the USB port
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(ROTATION);
  M5.Display.fillScreen(TFT_BLACK);
  Serial.printf("\nFancy clock: display %dx%d, PSRAM %u bytes free\n", (int)M5.Display.width(), (int)M5.Display.height(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  prefs.begin("clock", false);
  g_theme = prefs.getInt("theme", 0) % N_THEMES;
  g_24h = prefs.getBool("h24", true);
  g_bri = (uint8_t)constrain((int)prefs.getUChar("bri", BRIGHTNESS), MIN_BRIGHTNESS, 255);
  M5.Display.setBrightness(g_bri);
  if (M5.Rtc.isEnabled()) init_rtc_from_build(prefs);
  else Serial.println("WARNING: no RTC found");

  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
  g_tz = prefs.getString("tz", g_tz);
  setenv("TZ", g_tz.c_str(), 1);
  tzset();
  if (!g_ssid.isEmpty()) snprintf(g_net_state, sizeof(g_net_state), "waiting to sync");
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, nullptr, 0);

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

  build_dynamic_ui();
  wifi_build();
  bri_build();
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
  lv_timer_set_period(lv_indev_get_read_timer(indev), 16);
  for (int i = 0; i < N_THEMES; i++) render_static(i);  // ~0.3 s each, makes theme switches instant
  set_theme(g_theme, false);
  update_battery();
  g_fast_timer = lv_timer_create(fast_cb, 33, nullptr);
  lv_timer_create([](lv_timer_t*) { update_battery(); }, 20000, nullptr);
  // the fuel gauge can report 0% right after power-up, so read it again shortly after boot
  lv_timer_set_repeat_count(lv_timer_create([](lv_timer_t*) { update_battery(); }, 3000, nullptr), 1);
  Serial.printf("UI ready, heap int=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
  int64_t t = esp_timer_get_time();
  M5.update();          stat_add(st_upd, t); t = esp_timer_get_time();
  handle_touch();       stat_add(st_tap, t); t = esp_timer_get_time();
  poll_rtc();           stat_add(st_rtc, t); t = esp_timer_get_time();
  handle_serial();      stat_add(st_ser, t); t = esp_timer_get_time();
  bri_tick();
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
  lv_timer_handler();   stat_add(st_lv, t);
  {
    const int pos = now_local().s / 5;
    g_frames_by_pos[pos]++;
    if (esp_timer_get_time() - t > 40000) g_slow_by_pos[pos]++;
  }
  delay(2);
}
