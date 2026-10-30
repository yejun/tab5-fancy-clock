# Changelog

Versions follow [semantic versioning](https://semver.org). The firmware reports its version at boot and in the
serial `S` status.

## Unreleased

- Aurora show: its per-pixel light on the P4's SIMD unit (PIE, `pie.h`, checked against the C version at start-up),
  and core-0 shows start each frame as soon as the last one is done: aurora 18 -> 31-35 fps.
- Faster LVGL drawing, same pixels: custom RGB565 blend hooks for translucent fills and sprites (`lv_blend.c`),
  a fast path for additive blending (`patches/lvgl-9.6.0-additive-rgb565.patch`, applied by `tools/build.sh` to a
  copy of LVGL in `lvgl-patched/`), and the whole build at -O2 instead of -Os. Numerals 15-16 -> 18-19 fps, flap
  19 -> 25, fireworks 22-25 -> 25-26; the CPU while the clock ticks 19.4% -> 17.8%.
- Serial: `R` (PIE check / stress test), `A 1|3` / `A auto` (force a rotation for testing).

## 1.1.0 - 2026-10-28

- Hourly announcement: a two-note bell and the spoken hour (clips from the Halo watch project), with a volume
  setting; codec and amplifier are powered only while it plays.
- Night mode (23:00-07:00, on by default): the normal layout as a faint red outline at a separate backlight level,
  no second hand or seconds bar, ~2% CPU; the hourly announcement and show stay quiet. A tap shows the normal clock
  for 30 s. With night mode off, both hourly features run around the clock (1.0.0 always skipped 23:00-06:00).
- Settings card (gear in the calendar header): hourly announcement, volume, hourly show, night mode.
- Serial: `I` (announcement, volume), `X` (settings).
- The LICENSE file is the plain MIT text, so GitHub recognises it; the fonts' OFL note is in the README.
- Firmware images no longer contain this computer's directory names.

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
