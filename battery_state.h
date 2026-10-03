#pragma once
#include <stdint.h>

// With no pack fitted the charger cycles its output capacitor between ~4.2 V
// and ~8.4 V. A single high voltage must not be advertised as a full battery.
struct BatteryPresence {
  enum State { Unknown, Absent, Present };
  State state = Unknown;
  int64_t high_since = -1;
  void update(int mv, int64_t now) {
    if (mv < 5500 || mv > 9000) { state = Absent; high_since = -1; return; }
    if (high_since < 0) high_since = now;
    if (now - high_since >= 8000) state = Present;
  }
};
