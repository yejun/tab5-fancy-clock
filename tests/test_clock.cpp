#include "clock_logic.h"
#include "serial_transfer.h"
#include "dirty_regions.h"
#include "battery_state.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static time_t utc(int y, int m, int d, int h, int mi, int s) {
  return days_from_civil(y, m, d) * 86400 + h * 3600 + mi * 60 + s;
}
struct FakeClock {
  int64_t ms = 0;
  int64_t now_ms() { return ms; }
  void wait_ms(int n) { ms += n; }
};
struct FakeStream {
  FakeClock& clock;
  size_t limit;
  int delay_ms;
  std::vector<uint8_t> bytes;
  size_t write(const uint8_t* p, size_t n) {
    clock.ms += delay_ms;
    n = n < limit ? n : limit;
    bytes.insert(bytes.end(), p, p + n);
    return n;
  }
};

int main() {
  // Gregorian round trips across century/leap boundaries.
  for (int year : {1900, 1970, 2000, 2024, 2026, 2099, 2100}) {
    for (int month = 1; month <= 12; ++month) {
      for (int day = 1; day <= days_in_month(year, month); ++day) {
        int y, m, d;
        civil_from_days(days_from_civil(year, month, day), y, m, d);
        assert(y == year && m == month && d == day);
      }
    }
  }
  assert(weekday_of(1970, 1, 1) == 4);
  assert(days_in_month(2000, 2) == 29 && days_in_month(2100, 2) == 28);
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1); tzset();
  // RTC fallback and NTP both use this same UTC -> local conversion.
  Now before = local_from_utc_ms(utc(2026, 3, 8, 9, 59, 59) * 1000 + 999);
  Now after = local_from_utc_ms(utc(2026, 3, 8, 10, 0, 0) * 1000);
  assert(before.h == 1 && before.mi == 59 && before.ms == 999);
  assert(after.h == 3 && after.mi == 0);
  before = local_from_utc_ms(utc(2026, 11, 1, 8, 59, 59) * 1000);
  after = local_from_utc_ms(utc(2026, 11, 1, 9, 0, 0) * 1000);
  assert(before.h == 1 && before.mi == 59 && after.h == 1 && after.mi == 0);
  // Legacy RTC migration uses automatic DST detection.
  time_t epoch;
  assert(local_epoch(2026, 7, 1, 12, 0, 0, epoch) && epoch == utc(2026, 7, 1, 19, 0, 0));
  assert(local_epoch(2026, 1, 1, 12, 0, 0, epoch) && epoch == utc(2026, 1, 1, 20, 0, 0));
  assert(!local_epoch(2026, 3, 8, 2, 30, 0, epoch));
  assert(!local_epoch(2026, 2, 29, 12, 0, 0, epoch));
  assert(!local_epoch(2026, 13, 1, 12, 0, 0, epoch));
  assert(!local_epoch(2026, 1, 1, 24, 0, 0, epoch));
  // Same minute/day numbers must still refresh after timezone/month/year changes.
  Now a = local_from_utc_ms(utc(2026, 10, 1, 12, 34, 0) * 1000);
  setenv("TZ", "MST7", 1); tzset();
  Now b = local_from_utc_ms(utc(2026, 10, 1, 12, 34, 0) * 1000);
  setenv("TZ", "EST5", 1); tzset();
  b = local_from_utc_ms(utc(2026, 10, 1, 12, 34, 0) * 1000);
  assert(a.mi == b.mi && minute_key(a) != minute_key(b));
  b = a; ++b.mo; assert(date_key(a) != date_key(b));
  b = a; ++b.y; assert(date_key(a) != date_key(b));
  setenv("TZ", "UTC0", 1); tzset();
  a = local_from_utc_ms(-1); assert(a.y == 1969 && a.mo == 12 && a.d == 31 && a.ms == 999);
  // Missed deadline due to display off/rendering is retried, never discarded.
  int64_t target = 100;
  assert(!rtc_write_due(99999, target) && target == 100);
  assert(rtc_write_due(100000, target));
  assert(rtc_write_due(100099, target));
  assert(!rtc_write_due(100100, target) && target == 101);
  assert(!rtc_write_due(900555, target) && target == 901);
  assert(rtc_write_due(901004, target));
  target = 0; assert(!rtc_write_due(901004, target));
  // Published CRC32 check vector and split-chunk equivalence.
  const uint8_t* digits = (const uint8_t*)"123456789";
  assert((crc32_update(0xFFFFFFFFu, digits, 9) ^ 0xFFFFFFFFu) == 0xCBF43926u);
  uint32_t crc = crc32_update(0xFFFFFFFFu, digits, 4);
  assert((crc32_update(crc, digits + 4, 5) ^ 0xFFFFFFFFu) == 0xCBF43926u);
  FakeClock clock;
  FakeStream stream{clock, 2, 0, {}};
  assert(serial_write_all(stream, clock, digits, 9, 15000));
  assert(stream.bytes.size() == 9 && memcmp(stream.bytes.data(), digits, 9) == 0);
  FakeStream stalled{clock, 0, 0, {}};
  assert(!serial_write_all(stalled, clock, digits, 9, 15000));
  assert(clock.ms == 1000);
  clock.ms = 0;
  FakeStream slow{clock, 1, 500, {}};
  assert(!serial_write_all(slow, clock, digits, 9, 2000));
  assert(clock.ms == 2000); // absolute deadline also bounds a slowly progressing host
  BatteryPresence battery;
  for (int ms = 0; ms < 30000; ms += 500) {
    battery.update(ms % 3000 < 1000 ? 4250 : 8400, ms);
    assert(battery.state != BatteryPresence::Present); // charger with no pack
  }
  for (int ms = 30000; ms <= 39000; ms += 500) battery.update(7600, ms);
  assert(battery.state == BatteryPresence::Present); // stable inserted pack
  battery.update(4250, 39500);
  assert(battery.state == BatteryPresence::Absent); // pack removed
  assert(battery_percent(6000) == 0 && battery_percent(8400) == 100 && battery_percent(9000) == 100);
  assert(battery_percent(8220) == 90 && battery_percent(8040) == 80);
  for (int mv = 6000; mv < 8500; mv += 10) assert(battery_percent(mv) <= battery_percent(mv + 10));
  ChargeLimiter limit;
  for (int i = 0; i < 3; ++i) assert(limit.update(8230, true));   // a brief high reading does not pause
  assert(limit.update(8100, true));                               // the confirmation count restarts
  for (int i = 0; i < 3; ++i) assert(limit.update(8230, true));
  assert(!limit.update(8230, true));                              // fourth consecutive sample pauses
  for (int i = 0; i < 20; ++i) assert(!limit.update(8100, true)); // resting between 80% and 90% stays paused
  for (int i = 0; i < 3; ++i) assert(!limit.update(8030, true));
  assert(limit.update(8030, true));                               // confirmed drop to 80% resumes
  for (int i = 0; i < 4; ++i) limit.update(8300, true);
  assert(!limit.charge_on);
  assert(limit.update(8300, false));                              // no confirmed pack: always charge
  limit.enabled = false;
  for (int i = 0; i < 8; ++i) assert(limit.update(8400, true));   // limit off: charge to 100%
  struct Area { int x1, y1, x2, y2; };
  Area boxes[] = {{0,0,9,9}, {0,5,9,14}, {30,30,39,39}};
  int count = 3;
  merge_dirty_regions(boxes, count);
  assert(count == 2 && boxes[0].y2 == 14);
  // Randomized coverage/property check: every old pixel survives, total work never grows.
  unsigned random = 42;
  auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
  for (int run = 0; run < 1000; ++run) {
    Area original[24], merged[24];
    int64_t before_area = 0, after_area = 0;
    for (int i = 0; i < 24; ++i) {
      int x = next() % 80, y = next() % 80;
      original[i] = merged[i] = {x, y, x + (int)(next() % 20), y + (int)(next() % 20)};
      before_area += (original[i].x2-x+1) * (original[i].y2-y+1);
    }
    int n = 24; merge_dirty_regions(merged, n);
    for (int i = 0; i < n; ++i) after_area += (merged[i].x2-merged[i].x1+1) * (merged[i].y2-merged[i].y1+1);
    assert(after_area <= before_area);
    for (auto& a : original) {
      bool covered = false;
      for (int j = 0; j < n; ++j) {
        const Area& b = merged[j];
        covered |= b.x1 <= a.x1 && b.y1 <= a.y1 && b.x2 >= a.x2 && b.y2 >= a.y2;
      }
      assert(covered);
    }
  }
  puts("Clock/calendar, DST, migration, refresh keys, RTC deadlines, CRC and serial backpressure: PASS");
}
