#pragma once
#include <stdint.h>
#include <time.h>

struct Now { int y, mo, d, h, mi, s, ms, wd; };

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


static Now local_from_utc_ms(int64_t utc) {
  time_t sec = utc / 1000;
  int ms = utc % 1000;
  if (ms < 0) { ms += 1000; --sec; }
  struct tm t;
  localtime_r(&sec, &t);
  return {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ms, t.tm_wday};
}

static bool local_epoch(int y, int mo, int d, int h, int mi, int sec, time_t& out) {
  if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo)
      || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) return false;
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = sec; t.tm_isdst = -1;
  out = mktime(&t);
  // Reject nonexistent local times in the spring DST gap instead of silently normalizing them.
  return out != (time_t)-1 && t.tm_year == y - 1900 && t.tm_mon == mo - 1 && t.tm_mday == d
      && t.tm_hour == h && t.tm_min == mi && t.tm_sec == sec;
}

static int64_t date_key(const Now& n) { return (int64_t)n.y * 10000 + n.mo * 100 + n.d; }
static int64_t minute_key(const Now& n) { return (date_key(n) * 24 + n.h) * 60 + n.mi; }

// Keep a missed RTC write pending at the next boundary. Never write an old target second.
static bool rtc_write_due(int64_t utc_ms, int64_t& target_s) {
  if (!target_s || utc_ms < target_s * 1000) return false;
  if (utc_ms >= target_s * 1000 + 100) { target_s = utc_ms / 1000 + 1; return false; }
  return true;
}
