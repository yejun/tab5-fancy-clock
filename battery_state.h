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

// Resting-voltage state of charge for one Li-ion cell (typical curve), pack = 2 cells in series.
// Under charge current the reading is a little high, under load a little low.
static int battery_percent(int pack_mv) {
  static const int16_t curve[][2] = {
    {3270, 0}, {3610, 5}, {3690, 10}, {3710, 15}, {3730, 20}, {3750, 25}, {3770, 30},
    {3790, 35}, {3800, 40}, {3820, 45}, {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65},
    {3950, 70}, {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95}, {4200, 100}};
  const int n = sizeof(curve) / sizeof(curve[0]);
  const int mv = pack_mv / 2;
  if (mv <= curve[0][0]) return 0;
  if (mv >= curve[n - 1][0]) return 100;
  int i = 1;
  while (mv > curve[i][0]) ++i;
  return curve[i - 1][1] + (mv - curve[i - 1][0]) * (curve[i][1] - curve[i - 1][1]) / (curve[i][0] - curve[i - 1][0]);
}

// Charge current makes the terminal voltage read high and load current low. Measured on this pack:
// starting a 665 mA charge jumped 8083 -> 8186 mV at once and crept up ~55 mV more over 2 min;
// stopping dropped 8241 -> 8130 mV and settled to 8095 mV over 5 min. 0.16 ohm of that is instant;
// 0.22 ohm also covers the slower part and predicted the settled voltage within ~6 mV.
static constexpr int PACK_R_MOHM = 220;
static int battery_rest_mv(int pack_mv, int ma) { return pack_mv - (int)((int64_t)ma * PACK_R_MOHM / 1000); }

// Hold a plugged-in pack between ~80% and ~90% to slow calendar ageing at full charge. With charging
// disabled the Tab5 runs from USB and the pack rests (measured 0 mA). Enabling charging restarts a
// charge cycle even when full, so decide on the estimated rest voltage (battery_rest_mv), only after
// the threshold has held for several samples, and never switch twice within a minute.
struct ChargeLimiter {
  static constexpr int STOP_MV = 8220, RESUME_MV = 8040;  // 2 x 4.11 V (90%), 2 x 4.02 V (80%)
  static constexpr int CONFIRM = 4;                       // consecutive samples (500 ms apart)
  static constexpr int64_t MIN_DWELL_MS = 60000;
  bool enabled = true;
  bool charge_on = true;  // M5Unified enables charging at boot
  int above = 0, below = 0;
  int64_t switched_at = -MIN_DWELL_MS;
  // Returns the wanted charge-enable state. Without a confirmed pack, or with the limit off, charge.
  bool update(int rest_mv, bool pack_present, int64_t now) {
    if (!enabled || !pack_present) { above = below = 0; return charge_on = true; }
    above = rest_mv >= STOP_MV ? above + 1 : 0;
    below = rest_mv <= RESUME_MV ? below + 1 : 0;
    const bool may_switch = now - switched_at >= MIN_DWELL_MS;
    if (charge_on && above >= CONFIRM && may_switch) { charge_on = false; switched_at = now; }
    else if (!charge_on && below >= CONFIRM && may_switch) { charge_on = true; switched_at = now; }
    return charge_on;
  }
};
