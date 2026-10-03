// Exercise the actual firmware timekeeper against a simulated RTC and NVS.
#include "clock_logic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include <algorithm>
using String = std::string;
static int64_t fake_ms = 123456;
static int64_t esp_timer_get_time() { return fake_ms * 1000; }
static bool g_dirty_all = false;
namespace m5 {
struct rtc_datetime_t {
  struct { int year = 2000, month = 1, date = 1, weekDay = 0; } date;
  struct { int hours = 0, minutes = 0, seconds = 0; } time;
  rtc_datetime_t() = default;
  explicit rtc_datetime_t(const tm& t) {
    date = {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_wday};
    time = {t.tm_hour, t.tm_min, t.tm_sec};
  }
};
}
struct FakeRTC {
  bool enabled = true, read_ok = true, write_ok = true;
  int writes = 0;
  int64_t epoch = 0, written_at = fake_ms;
  bool isEnabled() { return enabled; }
  bool getDateTime(m5::rtc_datetime_t* dt) {
    if (!enabled || !read_ok) return false;
    time_t sec = epoch + (fake_ms - written_at) / 1000;
    tm t; gmtime_r(&sec, &t); *dt = m5::rtc_datetime_t(t); return true;
  }
  void setDateTime(const m5::rtc_datetime_t& dt) {
    ++writes;
    if (!write_ok) return;
    epoch = days_from_civil(dt.date.year, dt.date.month, dt.date.date) * 86400
          + dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
    written_at = fake_ms;
  }
};
static struct { FakeRTC Rtc; } M5;
static struct {
  template<class... Args> void printf(const char*, Args...) {}
  void println(const char*) {}
} Serial;
struct Preferences {
  std::map<std::string, int64_t> values;
  bool writes_ok = true, marker_ok = true;
  bool getBool(const char* key, bool fallback) { return values.count(key) ? values[key] != 0 : fallback; }
  int64_t getLong64(const char* key, int64_t fallback) { return values.count(key) ? values[key] : fallback; }
  size_t putLong64(const char* key, int64_t value) { if (!writes_ok) return 0; values[key] = value; return 8; }
  size_t putBool(const char* key, bool value) { if (!writes_ok || !marker_ok) return 0; values[key] = value; return 1; }
  bool isKey(const char* key) { return values.count(key); }
  void remove(const char* key) { values.erase(key); }
};
#define CLOCK_BUILD_UTC 1790852400LL
#include "timekeeping.h"
static time_t utc(int y, int m, int d, int h, int mi = 0, int s = 0) {
  return days_from_civil(y, m, d) * 86400 + h * 3600 + mi * 60 + s;
}
int main() {
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1); tzset();
  Preferences prefs;
  M5.Rtc.epoch = utc(2026, 10, 1, 12); // old RTC holds local wall time
  init_clock(prefs);
  assert(M5.Rtc.epoch == utc(2026, 10, 1, 19));
  assert(prefs.getBool("rtc_utc", false) && !prefs.isKey("rtc_migrate"));
  assert(now_local().h == 12);
  int writes = M5.Rtc.writes;
  fake_ms += 15000;
  init_clock(prefs); // firmware update/reboot preserves valid UTC instead of build time
  assert(M5.Rtc.writes == writes && now_local().h == 12 && now_local().s == 15);
  // Power loss after writing UTC but before committing the format marker.
  Preferences interrupted;
  interrupted.putLong64("rtc_migrate", utc(2026, 10, 1, 19));
  init_clock(interrupted);
  assert(M5.Rtc.epoch == utc(2026, 10, 1, 19));
  assert(interrupted.getBool("rtc_utc", false));
  // Failed reads must never turn into writes of build time.
  M5.Rtc.epoch = utc(2026, 10, 2, 21); M5.Rtc.written_at = fake_ms;
  M5.Rtc.read_ok = false;
  writes = M5.Rtc.writes;
  init_clock(prefs);
  assert(!g_rtc_ready && M5.Rtc.writes == writes);
  fake_ms += 1000;
  assert(!poll_rtc() && M5.Rtc.writes == writes);
  M5.Rtc.read_ok = true;
  fake_ms += 1000;
  rtc_init_tick();
  assert(g_rtc_ready && M5.Rtc.writes == writes && now_local().h == 14);
  // Failed legacy conversion must not expose the still-local registers as UTC.
  Preferences failed_migration;
  M5.Rtc.epoch = utc(2026, 10, 1, 12); M5.Rtc.written_at = fake_ms;
  M5.Rtc.write_ok = false;
  init_clock(failed_migration);
  assert(!g_rtc_ready && now_local().h == 12);
  assert(!poll_rtc() && now_local().h == 12);
  fake_ms += 2000;
  M5.Rtc.write_ok = true;
  rtc_init_tick();
  assert(g_rtc_ready && now_local().h == 12 && failed_migration.getBool("rtc_utc", false));
  // Failure to persist the migration journal must leave the legacy RTC untouched.
  Preferences failed_nvs; failed_nvs.writes_ok = false;
  M5.Rtc.epoch = utc(2026, 10, 1, 12); M5.Rtc.written_at = fake_ms;
  writes = M5.Rtc.writes;
  init_clock(failed_nvs);
  assert(!g_rtc_ready && M5.Rtc.writes == writes && now_local().h == 12);
  fake_ms += 1000; failed_nvs.writes_ok = true;
  rtc_init_tick();
  assert(g_rtc_ready && now_local().h == 12);
  // RTC may already contain UTC when committing the NVS format marker fails.
  // Retrying must use the journal, not convert those registers for a second time.
  Preferences failed_marker; failed_marker.marker_ok = false;
  M5.Rtc.epoch = utc(2026, 10, 1, 12); M5.Rtc.written_at = fake_ms;
  init_clock(failed_marker);
  assert(!g_rtc_ready && now_local().h == 12 && M5.Rtc.epoch == utc(2026, 10, 1, 19));
  assert(!poll_rtc());
  fake_ms += 1000; failed_marker.marker_ok = true;
  rtc_init_tick();
  assert(g_rtc_ready && now_local().h == 12 && M5.Rtc.epoch == utc(2026, 10, 1, 19, 0, 1));
  // Valid dates accepted by the console also survive reboot (including before 2025).
  assert(set_utc_time(utc(2024, 2, 29, 20)));
  writes = M5.Rtc.writes;
  init_clock(failed_nvs);
  assert(g_rtc_ready && M5.Rtc.writes == writes && now_local().y == 2024);
  // No RTC still has a useful build-time clock.
  M5.Rtc.enabled = false;
  init_clock(prefs);
  assert(g_base_ms + mono_ms() == CLOCK_BUILD_UTC * 1000);
  assert(!poll_rtc());
  M5.Rtc.enabled = true;
  adopt_ntp(utc(2026, 10, 1, 19) * 1000, fake_ms);
  assert(ntp_active() && g_dirty_all);
  // Missed write (e.g. display off or long render) stays pending and retries.
  fake_ms += 2500;
  writes = M5.Rtc.writes;
  rtc_backup_write();
  assert(M5.Rtc.writes == writes && g_rtc_write_utc_s == utc(2026, 10, 1, 19, 0, 3));
  fake_ms += 504;
  rtc_backup_write();
  assert(M5.Rtc.writes == writes + 1 && g_rtc_write_utc_s == 0);
  // An I2C write failure also retries rather than claiming a backup happened.
  adopt_ntp(utc(2026, 10, 2, 19) * 1000, fake_ms);
  M5.Rtc.write_ok = false;
  fake_ms += 2000;
  rtc_backup_write();
  assert(g_rtc_write_utc_s != 0);
  M5.Rtc.write_ok = true;
  fake_ms += 2000;
  rtc_backup_write();
  assert(g_rtc_write_utc_s == 0 && M5.Rtc.epoch == utc(2026, 10, 2, 19, 0, 4));
  fake_ms += NTP_VALID_MS;
  const int64_t expected = g_utc_base_ms + fake_ms;
  assert(!ntp_active() && g_base_ms + fake_ms == expected && g_resync);
  // Offline clock crosses DST via the same conversion as the NTP clock.
  assert(set_utc_time(utc(2026, 3, 8, 9, 59, 59)));
  assert(now_local().h == 1);
  fake_ms += 1000;
  assert(now_local().h == 3 && now_local().mi == 0);
  assert(set_local_time(2026, 7, 1, 12, 0, 0));
  assert(M5.Rtc.epoch == utc(2026, 7, 1, 19));
  assert(!set_local_time(2026, 2, 29, 12, 0, 0));
  // Read failures stop tight polling after four seconds and back off.
  M5.Rtc.read_ok = false;
  assert(poll_rtc());
  fake_ms += 4001;
  assert(!poll_rtc());
  puts("Firmware timekeeper with simulated RTC/NVS: PASS");
}
