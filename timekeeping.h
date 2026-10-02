#pragma once
// All clock state is owned by the loop task. RTC contents and both bases are UTC.
static int64_t g_base_ms = 0;
static bool g_synced = false, g_resync = true;
static int g_corr_n = 0, g_corr_max_ms = 0, g_reject_n = 0;
static String g_tz = "PST8PDT,M3.2.0,M11.1.0";
static constexpr int64_t NTP_VALID_MS = 24LL * 60 * 60 * 1000;
static bool g_ntp_valid = false;
static int64_t g_utc_base_ms = 0, g_ntp_last_mono = 0, g_rtc_write_utc_s = 0;
static int g_ntp_count = 0;

static inline int64_t mono_ms() { return esp_timer_get_time() / 1000; }
static int64_t rtc_epoch_s(const m5::rtc_datetime_t& dt) {
  return days_from_civil(dt.date.year, dt.date.month, dt.date.date) * 86400LL +
         dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
}
static bool ntp_active() {
  if (g_ntp_valid && mono_ms() - g_ntp_last_mono >= NTP_VALID_MS) {
    g_base_ms = g_utc_base_ms; // hold the last good UTC estimate until the RTC locks
    g_ntp_valid = false;
    g_resync = true;
  }
  return g_ntp_valid;
}

static bool write_rtc_utc(time_t sec) {
  if (!M5.Rtc.isEnabled()) return false;
  struct tm t;
  gmtime_r(&sec, &t);
  if (t.tm_year < 100 || t.tm_year > 199) return false;
  m5::rtc_datetime_t dt(t), check;
  M5.Rtc.setDateTime(dt);
  if (!M5.Rtc.getDateTime(&check)) return false;
  const int64_t delta = rtc_epoch_s(check) - sec;
  return delta >= 0 && delta <= 1;
}

static void adopt_ntp(int64_t utc_ms, int64_t sampled_mono) {
  const int64_t old_base = g_utc_base_ms;
  const bool had = g_ntp_valid;
  g_utc_base_ms = utc_ms - sampled_mono;
  g_base_ms = g_utc_base_ms;
  g_ntp_valid = true;
  g_ntp_last_mono = sampled_mono;
  ++g_ntp_count;
  g_dirty_all = true;
  g_rtc_write_utc_s = (g_utc_base_ms + mono_ms()) / 1000 + 2;
  if (had) Serial.printf("ntp: clock corrected by %lld ms\n", (long long)(g_utc_base_ms - old_base));
}

static Now now_local() {
  return local_from_utc_ms((ntp_active() ? g_utc_base_ms : g_base_ms) + mono_ms());
}

static bool set_utc_time(time_t utc) {
  if (!write_rtc_utc(utc)) return false;
  g_ntp_valid = false;
  g_rtc_write_utc_s = 0;
  g_base_ms = (int64_t)utc * 1000 - mono_ms();
  g_synced = false; g_resync = true; g_dirty_all = true;
  return true;
}
static bool set_local_time(int y, int mo, int d, int h, int mi, int s) {
  time_t utc;
  return local_epoch(y, mo, d, h, mi, s, utc) && set_utc_time(utc);
}

static void rtc_backup_write() {
  if (!M5.Rtc.isEnabled()) { g_rtc_write_utc_s = 0; return; }
  const int64_t utc = g_utc_base_ms + mono_ms();
  if (!rtc_write_due(utc, g_rtc_write_utc_s)) return;
  if (write_rtc_utc((time_t)g_rtc_write_utc_s)) {
    Serial.printf("rtc: written UTC %lld\n", (long long)g_rtc_write_utc_s);
    g_rtc_write_utc_s = 0;
  } else {
    g_rtc_write_utc_s = utc / 1000 + 2;
  }
}

static bool poll_rtc() {   // returns true while phase-locking (wants a 4 ms poll)
  static bool syncing = false, have_prev = false;
  static int64_t last_poll = 0, prev_done = 0, next_sync = 0, sync_start = 0;
  static int prev_sec = -1;
  const int64_t t0 = mono_ms();
  if (!M5.Rtc.isEnabled() || ntp_active()) return false;                  // NTP is the master clock

  if (g_resync) { syncing = false; have_prev = false; }
  if (!syncing) {
    if (!g_resync && t0 < next_sync) return false;
    syncing = true; have_prev = false; g_resync = false; sync_start = t0;
  }
  if (t0 - last_poll < 4) return true;
  last_poll = t0;

  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) {
    if (mono_ms() - sync_start > 4000) { syncing = false; next_sync = mono_ms() + 5000; }
    return syncing;
  }
  const int64_t t1 = mono_ms();
  const int64_t sec = rtc_epoch_s(dt);
  const int64_t read_at = (t0 + t1) / 2;

  if (!g_synced && !have_prev) g_base_ms = sec * 1000 + 500 - t1;  // provisional, until we see a boundary

  if (have_prev && dt.time.seconds != prev_sec) {
    if (read_at - prev_done <= 15) {  // tight window -> boundary is known to within ~8 ms
      const int64_t cand = sec * 1000 - (prev_done + read_at) / 2;
      const int d = (int)(cand - g_base_ms);
      if (g_synced) { g_corr_n++; if (abs(d) > g_corr_max_ms) g_corr_max_ms = abs(d); }
      if (llabs(cand - g_base_ms) > 100) g_dirty_all = true;
      g_base_ms = cand;
      g_synced = true;
      syncing = false;
      next_sync = t1 + 10 * 60 * 1000;
      return false;
    }
    g_reject_n++;  // a poll was delayed; wait for the next boundary
  }
  have_prev = true;
  prev_sec = dt.time.seconds;
  prev_done = t1;
  if (t1 - sync_start > 4000) { syncing = false; next_sync = t1 + 5000; }  // give up for now
  return syncing;
}

// Preserve a valid RTC on every firmware update. Migrate legacy local wall time once.
// A pending UTC value makes interrupted migration retryable without converting twice.
static void init_clock(Preferences& prefs) {
  time_t build_utc = 0;
#ifdef CLOCK_BUILD_UTC
  build_utc = (time_t)CLOCK_BUILD_UTC;
#else
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  int d, y, h, mi, sec;
  sscanf(__DATE__ + 4, "%d %d", &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &h, &mi, &sec);
  local_epoch(y, (strstr(months, mon) - months) / 3 + 1, d, h, mi, sec, build_utc);
#endif
  g_base_ms = (int64_t)build_utc * 1000 - mono_ms();
  m5::rtc_datetime_t cur;
  const bool valid = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&cur) && cur.date.year >= 2025;
  const bool is_utc = prefs.getBool("rtc_utc", false);
  if (valid && is_utc) {
    if (prefs.isKey("rtc_migrate")) prefs.remove("rtc_migrate");
    g_base_ms = rtc_epoch_s(cur) * 1000 - mono_ms();
    return;
  }
  time_t seed = build_utc;
  const int64_t pending = prefs.getLong64("rtc_migrate", 0);
  if (pending) seed = (time_t)pending;
  else if (valid && !local_epoch(cur.date.year, cur.date.month, cur.date.date,
                                cur.time.hours, cur.time.minutes, cur.time.seconds, seed)) {
    Serial.println("rtc: invalid legacy local time; using build UTC");
    seed = build_utc;
  }
  g_base_ms = (int64_t)seed * 1000 - mono_ms();
  if (!M5.Rtc.isEnabled()) { Serial.println("WARNING: no RTC; using build UTC"); return; }
  if (prefs.putLong64("rtc_migrate", seed) && write_rtc_utc(seed) && prefs.putBool("rtc_utc", true)) {
    prefs.remove("rtc_migrate");
    Serial.println(valid ? "rtc: migrated local time to UTC" : "rtc: seeded from build UTC");
  } else {
    Serial.println("WARNING: RTC initialization failed");
  }
}
