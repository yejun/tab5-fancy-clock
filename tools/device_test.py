#!/usr/bin/env python3
"""Exercise updated firmware on a connected Tab5; restores timezone, time and display state.

Requires pyserial. Does not change WiFi credentials. NTP checks are skipped if no
SSID is configured. Checks serial-visible UI state and screenshot CRCs; actual
touch gestures and physical rotation still require hands-on testing.
"""
import argparse
import datetime
import re
import time

from clockctl import open_port, read_for, read_screenshot, send


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


last_uptime = None


def status(s):
    global last_uptime
    out = send(s, "S")
    require(re.search(r"time \d{4}-", out), "Device did not return status")
    uptime = re.search(r"uptime_ms=(\d+)", out)
    if uptime:
        value = int(uptime[1])
        if last_uptime is not None:
            require(value >= last_uptime, "Device rebooted during the test")
        last_uptime = value
    return out


def check_ui(s, expected_time, expected_month=None):
    out = status(s)
    require(f"ui time={expected_time} |" in out, f"Stale time label: expected {expected_time}")
    if expected_month:
        require(f"| month={expected_month}" in out, "Stale calendar month")


def main():
    global last_uptime
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    args = parser.parse_args()
    with open_port(args.port) as s:
        original = status(s)
        timezone = re.search(r"\| tz=(.*?) \| ssid=", original).group(1)
        configured = "| ssid=(none)" not in original
        was_ntp = "ntp ACTIVE" in original
        on = "display=1" in original
        h24 = "24h=1" in original
        wifi_open = "wifi_ui=1" in original
        stress_started = None
        try:
            if wifi_open:
                send(s, "U")
            if not on:
                send(s, "D")
            if not h24:
                send(s, "M")
            for framebuffer in (False, True):
                w, h, _ = read_screenshot(s, framebuffer)
                require((w, h) == (1280, 720), "Unexpected screenshot dimensions")
            print("PASS both screenshot paths and checksums", flush=True)
            # WiFi setup and scans were previously absent from this runner.
            # Assert the actual screen state so a reset cannot look like a pass.
            for cycle in range(3):
                out = send(s, "U", 12)
                require(not any(x in out for x in ("Guru Meditation", "assert failed", "Fancy clock: display")),
                        "Reset while opening WiFi setup")
                require("wifi_ui=1" in status(s), "WiFi screen did not stay open")
                read_screenshot(s, True)
                send(s, "U")
                require("wifi_ui=0" in status(s), "WiFi screen did not close")
            print("PASS repeated WiFi setup/scan/close cycles", flush=True)
            # Allow any startup NTP attempt to settle before deliberate time changes.
            read_for(s, 36)
            send(s, "Z UTC0")
            sample_epoch = int(datetime.datetime(2026, 10, 1, 9, 14, tzinfo=datetime.timezone.utc).timestamp())
            require("OK UTC" in send(s, f"E {sample_epoch}"), "UTC time command failed")
            expected = datetime.datetime.fromtimestamp(sample_epoch, datetime.timezone.utc).strftime("%H:%M")
            check_ui(s, expected, "OCTOBER 2026")
            send(s, "Z EST5")
            expected = (datetime.datetime.fromtimestamp(sample_epoch, datetime.timezone.utc) - datetime.timedelta(hours=5)).strftime("%H:%M")
            check_ui(s, expected, "OCTOBER 2026")
            send(s, "Z UTC0")
            require("OK time" in send(s, "T 2026-11-01 09:14:00"), "Local time command failed")
            check_ui(s, "09:14", "NOVEMBER 2026")
            require("ERR" in send(s, "T 2026-02-30 12:00:00"), "Invalid date was accepted")
            print("PASS timezone, same-day month correction and invalid date", flush=True)
            send(s, "Z PST8PDT,M3.2.0,M11.1.0")
            # Offline UTC ticks must advance local time across DST without NTP.
            epoch = int(datetime.datetime(2026, 3, 8, 9, 59, 58, tzinfo=datetime.timezone.utc).timestamp())
            require("OK UTC" in send(s, f"E {epoch}"), "UTC time command failed")
            read_for(s, 3)
            check_ui(s, "03:00", "MARCH 2026")
            print("PASS offline DST transition", flush=True)
            # Stop reading a binary transfer to exercise the device's stall deadline.
            s.reset_input_buffer()
            s.write(b"P\n")
            time.sleep(3)
            # Drain by reading: tcflush on a throttled Linux cdc-acm tty can leave input stalled.
            read_for(s, 3)
            status(s)
            read_screenshot(s)
            print("PASS stalled screenshot recovers and next capture succeeds", flush=True)
            send(s, "D")
            require("display=0" in status(s), "Display did not sleep")
            if configured:
                out = send(s, "N", 40)
                require("rtc: written UTC" in out, "NTP/RTC backup while asleep was not confirmed")
                require("rtc_pending=0" in status(s), "RTC write remains pending")
                print("PASS NTP backup while display is off", flush=True)
            else:
                print("SKIP NTP test: no saved WiFi credentials", flush=True)
            send(s, "D")
            require("display=1" in status(s), "Display did not wake")
            read_screenshot(s, True)
            print("PASS display sleep/wake and framebuffer capture", flush=True)
            stress_started = time.monotonic()
            send(s, "K")
            for _ in range(13):
                log = read_for(s, 5)
                require(not any(x in log for x in ("Guru Meditation", "assert failed", "Fancy clock: display")), "Reset during theme stress test")
            read_screenshot(s, True)
            status(s)
            print("PASS 20 theme switches without reset", flush=True)
            # Exercise more font-cache entries before asking WiFi for memory.
            # A short clock-only soak missed internal-RAM pressure after browsing
            # dates and opening other screens, despite plentiful free PSRAM.
            for month in range(1, 13):
                send(s, f"T 2026-{month:02d}-15 {month:02d}:{month * 5 % 60:02d}:00")
                status(s)
            out = send(s, "U", 12)
            require(not any(x in out for x in ("Guru Meditation", "assert failed", "Fancy clock: display")),
                    "Reset during WiFi scan after calendar stress")
            current = status(s)
            require("wifi_ui=1" in current, "WiFi failed after calendar stress")
            memory = re.search(r"heap int=(\d+)", current)
            require(memory and int(memory[1]) >= 32768, "Insufficient internal RAM after calendar/WiFi stress")
            read_screenshot(s, True)
            send(s, "U")
            print("PASS twelve calendar months followed by WiFi scan; internal RAM reserve >=32 KiB", flush=True)
        finally:
            last_uptime = None  # allow recovery/restore after a detected reboot
            # Let an already-started 20-switch cycle finish before restoring its
            # starting theme (theme changes in the stress cycle aren't persisted).
            if stress_started is not None:
                read_for(s, max(0, stress_started + 63 - time.monotonic()))
            # Restore durable user preferences even when an assertion fails.
            send(s, "Z " + timezone)
            send(s, f"E {int(time.time())}")
            current = status(s)
            original_theme = int(re.search(r"theme=(\d+)", original).group(1))
            current_theme = int(re.search(r"theme=(\d+)", current).group(1))
            for _ in range((original_theme - current_theme) % 5):
                send(s, "C")
            if ("24h=1" in current) != h24:
                send(s, "M")
            if ("wifi_ui=1" in current) != wifi_open:
                send(s, "U")
            if ("display=1" in current) != on:
                send(s, "D")
            if configured and was_ntp:
                result = send(s, "N", 40)
                if "time synced" not in result:
                    print("WARNING: restored computer time, but could not restore NTP sync")
    print("Device automation passed. Check touch gestures and physical rotation by hand.")


if __name__ == "__main__":
    main()
