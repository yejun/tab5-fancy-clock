# Fancy Clock for M5Stack Tab5

LVGL 9.5 + M5GFX/M5Unified, 1280x720 landscape.

![Fancy Clock on the Tab5](docs/screenshot.png)

Requires: Arduino CLI, the `m5stack:esp32` core (3.3.x), and the libraries `lvgl` (9.5), `M5GFX`, `M5Unified`.

- Analog dial with sweeping second hand (~15 fps), day-progress ring, frosted-glass calendar card, big digital time,
  seconds bar, battery.
- Five themes (Aurora, Sunset, Ocean, Jade, Graphite): deep tinted backgrounds, saturated-but-soft accents, off-white
  "ink" instead of pure white. Fine detail: guilloche sub-dial, sunburst, railway minute track, applied indices with a
  lit edge, translucent lens on the second-hand tip, aurora ribbons and bokeh behind a blurred glass card.
- Touch controls (all remembered across reboots except display on/off):
  - **Drag left/right** anywhere → brightness (right = brighter). A small bar shows the level; minimum ~3% so the screen can't be lost.
  - **Double-tap** → display off (backlight 0; all drawing and automatic WiFi syncs paused, only touch + serial stay
    active; the clock keeps time). While off, any tap wakes it - the picture is refreshed *before* the backlight
    comes on, so the second hand doesn't jump. (Panel sleep and CPU down-clocking were tried and don't work on the
    Tab5: panel sleep also disables touch, and 40 MHz starves the MIPI-DSI controller. See comments in the code.)
  - **Tap the big digits** → 12h/24h. **Tap the WiFi icon** (top right) → WiFi setup. **Tap the battery icon** →
    a card with level, charging state, voltage (pack and per cell), current, power and a rough time-left /
    time-to-full estimate (2000 mAh pack, smoothed current); refreshed every second, the next tap anywhere closes it. **Tap anywhere else** → next theme
    (Aurora, Sunset, Ocean, Jade, Graphite). Single taps act after a ~0.35 s pause so they can't be confused with a double-tap.

- **Auto-rotate**: turn the Tab5 upside-down and the picture follows (landscape and landscape upside-down only).
  The new orientation has to be held for 1.2 s, and lying flat, standing on a short side, being moved or a finger on
  the glass never turns it. Only the accelerometer runs (the gyro is switched off); it is read 5x a second while the
  display is on. A turn is one full redraw - LVGL keeps drawing in landscape, only the PPA flush rotates differently.

## Rendering / power
With the display on, the CPU is busy ~20% of the time (it was ~80% at 27 fps before this pipeline):
- **Static artwork** per theme is rendered once into an RGB565 image: the smooth background and radial dial parts are
  painted per pixel in float (the other themes on core 0 in the background after boot), LVGL draws the crisp details
  on top in 32 bit, then it is dithered (4x4 Bayer) to RGB565.
- **Composite**: static artwork + day ring + hour/minute hands, re-drawn only where those move (every 3 s / 30 s / 60 s).
- **Per frame** (66 ms) only the second hand, its tip and the hub are drawn on top of the composite, and only inside
  short boxes that follow the hand (not its whole bounding box). Glows are pre-computed sprites, not LVGL box shadows.
- **Flush**: the rotation into the panel's portrait frame buffer is done by the P4's PPA (DMA), not the CPU.
- **Idle**: `loop()` sleeps until LVGL's next timer is due and polls touch every 25 ms; LVGL's own touch input is only
  polled while the WiFi screen is open.
- `S` prints `cpu busy N% | fps` since the previous `S`. Note newlib's float trig goes through soft double on the P4,
  so per-pixel code avoids `sinf`/`atan2f`.

## Build / flash
    tools/build.sh            # compile
    tools/build.sh upload     # compile + flash /dev/ttyACM0

Board options are set in `tools/build.sh`. `ChipVariant=prev3` is for ESP32-P4 silicon older than v3.00
(esptool prints `revision v1.x`); use `postv3` for newer units. `lv_conf.h` lives next to the sketch.

## Time
Three sources, best first:
1. **Network time (NTP)** - set up WiFi by tapping the WiFi icon in the top-right corner (scan, pick a network,
   type the password). The clock then connects in the background (core 0), syncs over SNTP, and disconnects
   (the WiFi stack stays initialised and the radio idles). It repeats every 6 hours; after a failed attempt it backs
   off (1, 2, 5, 10, 30, 60 min). Switching WiFi fully off and on again between syncs was tried and is NOT reliable
   on the Tab5 (every second re-init runs out of internal RAM and reboots) - see the comment in the code. The WiFi icon lights up while the
   time is NTP-synced. Daylight saving is automatic because the timezone is a POSIX TZ string.
2. **RTC** - every NTP sync also writes the Tab5's RX8130 (on a whole-second boundary), so an offline reboot
   still starts within milliseconds. Without NTP for 24 h the clock falls back to the RTC.
3. **Build time** - the RTC is seeded from the build time when new firmware boots.

    tools/clockctl.py scan                     # list WiFi networks (checks the radio works)
    tools/clockctl.py wifi "SSID" "PASSWORD"   # store credentials in flash and sync now
    tools/clockctl.py tz "PST8PDT,M3.2.0,M11.1.0"   # default US Pacific; other examples below
    tools/clockctl.py sync                     # sync now
    tools/clockctl.py time                     # set the RTC from this computer (offline use)

POSIX TZ examples: `EST5EDT,M3.2.0,M11.1.0` (US Eastern), `GMT0BST,M3.5.0/1,M10.5.0` (UK),
`CET-1CEST,M3.5.0,M10.5.0/3` (Central Europe), `CST-8` (China), `JST-9` (Japan), `UTC0`.

## Other helpers
    tools/clockctl.py shot out.png   # screenshot over USB serial (out_fb.png: read back from the panel frame buffer)
    tools/clockctl.py cmd S|C|M      # status+timing stats / next theme / toggle 12-24h
    tools/clockctl.py cmd A          # accelerometer reading and the orientation it asks for
    tools/clockctl.py cmd G          # open/close the battery card
    tools/clockctl.py cmd K          # stress test: 20 automatic theme switches
    tools/clockctl.py cmd H<mask>    # debug: 1=hide day ring 2=hand shadows 4=soft shadow penumbra
    tools/clockctl.py log 5          # device log
    tools/make_fonts.sh              # regenerate fonts.h (Noto Sans subset, OFL)

`ROTATION` (start-up orientation when the IMU can't tell, 1 or 3), `ROT_HOLD_MS`, `BRIGHTNESS`, `FRAME_MS` (animation period) and `TOUCH_MS` are constants at the top of
`fancy_clock.ino`. Themes are the `THEMES` table next to them.

## Credits
- [LVGL](https://lvgl.io) (MIT), [M5GFX / M5Unified](https://github.com/m5stack) (MIT), Arduino-ESP32 / ESP-Hosted (Apache-2.0).
- `fonts.h` embeds a subset of [Noto Sans](https://fonts.google.com/noto) Light and Medium, licensed under the
  SIL Open Font License 1.1.
- No license has been chosen for this project's own code yet.
