# Changelog

Versions follow [semantic versioning](https://semver.org). The firmware reports its version at boot and in the
serial `S` status.

## 1.0.0 - 2026-10-04

First versioned release.

- Analog dial and big digital time (fixed digit slots) for the M5Stack Tab5, LVGL 9 + M5GFX, five themes,
  ~15 fps at ~19% CPU; artwork pre-rendered per theme, composite redrawn only where the hands move, asynchronous
  PPA flush.
- Six hourly shows (07:00-22:00): light sweep, fireworks, numerals in flight, split-flap, 3D dial flip, aurora;
  15-29 fps while animating.
- Touch: brightness drag, double-tap display off, 12/24-hour, themes, WiFi setup, battery card, time zone picker
  (59 cities with live local time and offset).
- Time: NTP over WiFi, RX8130 RTC kept in UTC (power-loss detection via VLF), POSIX time zones with automatic
  daylight saving.
- Battery: charging state from the measured pack current, optional 80-90% charge limit, Li-ion voltage curve.
- Auto-rotate (landscape and upside-down).
- Serial console and Python tools (`clockctl.py`, `device_test.py`), host tests, CRC-checked screenshots.
