#pragma once
#include "serial_transfer.h"

// Only loop-task code writes application logs. ESP-IDF logs from other tasks use
// a separate gate so they cannot interleave with a binary screenshot.
static SemaphoreHandle_t g_serial_log_gate;
static vprintf_like_t g_original_log;
static int serial_log(const char* fmt, va_list args) {
  if (xSemaphoreTake(g_serial_log_gate, 0) != pdTRUE) return 0;
  const int result = g_original_log ? g_original_log(fmt, args) : 0;
  xSemaphoreGive(g_serial_log_gate);
  return result;
}
static void serial_logging_setup() {
  g_serial_log_gate = xSemaphoreCreateMutex();
  if (g_serial_log_gate) g_original_log = esp_log_set_vprintf(serial_log);
}
struct SerialClock {
  int64_t now_ms() { return mono_ms(); }
  void wait_ms(int ms) { delay(ms); }
};
// While HWCDC thinks the host is gone (e.g. after logging with no reader), write() overwrites the
// oldest queued bytes yet reports success. Report no progress instead, so the stall timer waits.
struct SerialStream {
  size_t write(const uint8_t* data, size_t size) { return Serial.isConnected() ? Serial.write(data, size) : 0; }
};
struct ScreenshotGuard {
  bool locked;
  ScreenshotGuard() : locked(g_serial_log_gate && xSemaphoreTake(g_serial_log_gate, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (locked) g_screenshot_active = true;
  }
  ~ScreenshotGuard() {
    if (locked) { g_screenshot_active = false; xSemaphoreGive(g_serial_log_gate); }
  }
};

static void send_screenshot(bool framebuffer = false) {
  ScreenshotGuard guard;
  if (!guard.locked) { Serial.println("SNAPFAIL logging busy"); return; }
  lv_draw_buf_t* snap = nullptr;
  if (!framebuffer) snap = lv_snapshot_take(g_wifi_open ? wifi_ui : lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if ((framebuffer && !g_fb) || (!framebuffer && !snap)) { Serial.println("SNAPFAIL"); return; }
  const uint32_t w = framebuffer ? SCR_W : snap->header.w;
  const uint32_t h = framebuffer ? SCR_H : snap->header.h;
  if (framebuffer) esp_cache_msync(g_fb, FB_W * FB_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  SerialClock clock;
  SerialStream out;
  const int64_t deadline = mono_ms() + 15000;
  char header[64];
  int len = snprintf(header, sizeof(header), "SNAP %u %u\n", (unsigned)w, (unsigned)h);
  bool ok = serial_write_all(out, clock, (const uint8_t*)header, len, deadline);
  uint32_t crc = 0xFFFFFFFFu;
  static uint16_t fb_row[SCR_W];
  for (uint32_t y = 0; ok && y < h; ++y) {
    const uint8_t* row;
    if (framebuffer) {
      for (int x = 0; x < SCR_W; ++x)
        fb_row[x] = g_rot == 1 ? g_fb[x * FB_W + (FB_W - 1 - y)] : g_fb[(FB_H - 1 - x) * FB_W + y];
      row = (const uint8_t*)fb_row;
    } else row = (const uint8_t*)snap->data + y * snap->header.stride;
    crc = crc32_update(crc, row, w * 2);
    ok = serial_write_all(out, clock, row, w * 2, deadline);
  }
  if (ok) {
    len = snprintf(header, sizeof(header), "\nENDSNAP %08lx\n", (unsigned long)(crc ^ 0xFFFFFFFFu));
    ok = serial_write_all(out, clock, (const uint8_t*)header, len, deadline);
  }
  if (snap) lv_draw_buf_destroy(snap);
  if (!ok) Serial.println("\nSNAPFAIL transfer timeout");
}

static void handle_serial() {
  static char line[192];
  static size_t len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c; else overflow = true;
      continue;
    }
    if (overflow) { overflow = false; len = 0; Serial.println("ERR command too long"); continue; }
    line[len] = 0;
    len = 0;
    if (!line[0]) continue;
    int y, mo, d, h, mi, s;
    switch (line[0]) {
      case 'E': {
        char* end;
        const long long epoch = strtoll(line + 1, &end, 10);
        if (end == line + 1 || *end || epoch < 946684800LL || epoch > 4102444799LL || !set_utc_time((time_t)epoch)) {
          Serial.println("ERR usage: E <UTC epoch seconds 2000..2099>, requires working RTC"); break;
        }
        ++g_net_generation;
        net_request(0);
        Serial.printf("OK UTC time set %lld\n", epoch);
        break;
      }
      case 'T':
        if (sscanf(line + 1, " %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
          if (!set_local_time(y, mo, d, h, mi, s)) { Serial.println("ERR invalid local time or RTC write failed"); break; }
          ++g_net_generation;
          net_request(0);
          Serial.printf("OK time set %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
        } else {
          Serial.println("ERR usage: T YYYY-MM-DD HH:MM:SS");
        }
        break;
      case 'W': {  // "W ssid|password" stores WiFi credentials and syncs now; "W" alone clears them
        const char* arg = line + 1;
        while (*arg == ' ') arg++;
        const char* bar = strchr(arg, '|');
        if ((bar ? bar - arg : (ptrdiff_t)strlen(arg)) > 32 || (bar && strlen(bar + 1) > 64)) {
          Serial.println("ERR SSID or password too long"); break;
        }
        ++g_net_generation;
        g_ssid = bar ? String(arg).substring(0, bar - arg) : String(arg);
        g_pass = bar ? String(bar + 1) : String("");
        prefs.putString("ssid", g_ssid);
        prefs.putString("pass", g_pass);
        net_request(1);
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
          g_dirty_all = true;
        }
        Serial.printf("OK tz=%s\n", g_tz.c_str());
        break;
      }
      case 'U': if (g_wifi_open) wifi_close(); else wifi_open(); Serial.printf("OK wifi screen %s\n", g_wifi_open ? "open" : "closed"); break;
      case 'Y': wl_select(atoi(line + 1)); Serial.println("OK select"); break;  // test: pick scan result n
      case 'B': set_brightness(atoi(line + 1), true); g_bri_hide_at = mono_ms() + 60000; Serial.printf("OK brightness %d\n", g_bri); break;  // test: B <8-255>
      case 'D': display_set(!g_disp_on); Serial.printf("OK display %s\n", g_disp_on.load() ? "on" : "off"); break;              // test: toggle display
      case 'N': if (net_request(1)) Serial.println("OK syncing"); break;
      case 'Q': if (net_request(2)) Serial.println("OK scanning"); break;
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
        Serial.printf("display=%d wifi_ui=%d brightness=%u rtc_pending=%lld\n", (int)g_disp_on.load(), (int)g_wifi_open,
                      (unsigned)g_bri, (long long)g_rtc_write_utc_s);
        Serial.printf("ui time=%s | date=%s | month=%s\n", lv_label_get_text(lbl_time), lv_label_get_text(lbl_date), lv_label_get_text(lbl_month));
        Serial.printf("uptime_ms=%lld reset_reason=%d rtc_ready=%d rtc_lost=%d battery_presence=%d usb_only=%d\n", (long long)mono_ms(),
                      (int)esp_reset_reason(), (int)g_rtc_ready, [] { bool l = false; return rtc_time_lost(l) ? (int)l : -1; }(), (int)g_battery_presence.state, (int)g_usb_only);
        Serial.printf("battery: level=%d voltage=%dmV current=%dmA charging=%d\n", (int)M5.Power.getBatteryLevel(),
                      (int)M5.Power.getBatteryVoltage(), (int)M5.Power.getBatteryCurrent(), (int)M5.Power.isCharging());
        Serial.printf("heap int=%u psram=%u | flushes=%u avg=%uus/flush %uus/kpx\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)g_flush_n,
                      g_flush_n ? g_flush_us / g_flush_n : 0, g_flush_px ? (uint32_t)((uint64_t)g_flush_us * 1000 / g_flush_px) : 0);
        {
          const int64_t win = esp_timer_get_time() - g_stat_t0;
          Serial.printf("cpu busy %.1f%% | %.1f fps over %.1f s\n", win ? 100.0 * g_busy_us / win : 0.0,
                        win ? g_frames * 1e6 / win : 0.0, win / 1e6);
          g_busy_us = 0; g_frames = 0; g_stat_t0 = esp_timer_get_time();
        }
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
      case 'H': {  // debug: bitmask of things to disable: 1 day ring, 2 hand shadows, 4 soft shadow penumbra
        const int m = atoi(line + 1);
        g_show_ring = !(m & 1);
        g_show_shadows = !(m & 2);
        g_soft_shadows = !(m & 4);
        g_merge_regions = !(m & 8);
        g_comp_full = true;
        lv_obj_invalidate(dial_obj);
        Serial.printf("OK mask %d\n", m);
        break;
      }
      case 'F': send_screenshot(true); break;
      case 'A': {  // accelerometer + orientation (for checking ORIENT_AXIS / ORIENT_SIGN)
        float x = 0, y = 0, z = 0;
        if (g_imu_ok) M5.Imu.getAccel(&x, &y, &z);
        Serial.printf("OK accel x=%.2f y=%.2f z=%.2f g | wants rotation %d, showing %d\n", x, y, z, orient_sample(), g_rot);
        break;
      }
      case 'V':
        if (!strcmp(line + 1, " usb")) set_battery_mode(true);
        else if (!strcmp(line + 1, " auto")) set_battery_mode(false);
        else { Serial.println("ERR usage: V usb|auto"); break; }
        Serial.printf("OK battery display %s\n", g_usb_only ? "USB only" : "Auto");
        break;
      case 'G': if (g_batt_open) batt_close(); else batt_open(); Serial.printf("OK battery card %s\n", g_batt_open ? "open" : "closed"); break;
      default: Serial.println("ERR unknown command"); break;
    }
  }
}

