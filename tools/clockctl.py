#!/usr/bin/env python3
"""Control the clock over USB serial. Run with --help for commands."""
import argparse
import os
import re
import time
import zlib


def open_port(port="/dev/ttyACM0"):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.write_timeout = 2
    s.dtr = s.rts = True
    s.open()
    return s


def read_for(s, secs):
    end = time.monotonic() + secs
    chunks = []
    while time.monotonic() < end:
        chunks.append(s.read(4096))
    return b"".join(chunks).decode(errors="replace")


def send(s, line, wait=1.0):
    if "\n" in line or "\r" in line:
        raise ValueError("Commands must fit on one line")
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    return read_for(s, wait)


def read_screenshot(s, framebuffer=False, now=time.monotonic):
    """Return (width, height, RGB565 bytes) only after validating the trailer CRC."""
    s.reset_input_buffer()
    s.write(b"F\n" if framebuffer else b"P\n")
    buf = bytearray()
    deadline = now() + 10
    while True:
        buf.extend(s.read(4096))
        match = re.search(rb"(?:^|\n)SNAP (\d+) (\d+)\r?\n", buf)
        if match:
            w, h = map(int, match.groups())
            if not (1 <= w <= 1280 and 1 <= h <= 1280 and w * h <= 1280 * 720):
                raise ValueError("Invalid screenshot dimensions")
            del buf[:match.end()]
            break
        if b"SNAPFAIL" in buf:
            raise RuntimeError("Device could not capture screenshot")
        if len(buf) > 65536 or now() >= deadline:
            raise TimeoutError("No screenshot header received")
    need = w * h * 2
    deadline = now() + 20
    while len(buf) < need or b"\n" not in buf[need + 1:]:
        buf.extend(s.read(65536))
        if now() >= deadline:
            raise TimeoutError(f"Incomplete screenshot: received {len(buf)}/{need} payload bytes")
    trailer_end = buf.find(b"\n", need + 1)
    trailer = bytes(buf[need:trailer_end + 1])
    match = re.fullmatch(rb"\nENDSNAP ([0-9a-fA-F]{8})\n", trailer)
    if not match:
        raise ValueError("Invalid screenshot trailer (update firmware if using the old protocol)")
    pixels = bytes(buf[:need])
    if zlib.crc32(pixels) != int(match[1], 16):
        raise ValueError("Screenshot checksum mismatch; image was not saved")
    return w, h, pixels


def shot(s, path):
    w, h, pixels = read_screenshot(s, path.endswith("_fb.png"))
    import numpy as np
    from PIL import Image
    a = np.frombuffer(pixels, dtype="<u2").reshape(h, w).astype(np.uint32)
    r = ((a >> 11) & 31) * 255 // 31
    g = ((a >> 5) & 63) * 255 // 63
    b = (a & 31) * 255 // 31
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(path)
    print(f"saved {path} ({w}x{h}, CRC verified)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=os.environ.get("CLOCK_PORT", "/dev/ttyACM0"))
    sub = parser.add_subparsers(dest="command", required=True)
    log = sub.add_parser("log", help="read device logs")
    log.add_argument("seconds", nargs="?", type=float, default=3)
    sub.add_parser("time", help="set RTC from this computer's UTC time")
    screenshot = sub.add_parser("shot", help="capture and verify a screenshot")
    screenshot.add_argument("path", nargs="?", default="shot.png")
    cmd = sub.add_parser("cmd", help="send a raw command")
    cmd.add_argument("line")
    sub.add_parser("scan")
    wifi = sub.add_parser("wifi")
    wifi.add_argument("ssid")
    wifi.add_argument("password", nargs="?", default="")
    tz = sub.add_parser("tz")
    tz.add_argument("timezone", nargs="?", default="")
    sub.add_parser("sync")
    args = parser.parse_args()
    if args.command == "wifi":
        if "|" in args.ssid or args.ssid.startswith(" ") or any(c in args.ssid + args.password for c in "\r\n"):
            parser.error("This serial protocol cannot encode that SSID/password; use the device UI")
        if len(args.ssid.encode()) > 32 or len(args.password.encode()) > 64:
            parser.error("SSID/password exceeds the WiFi byte limit")
    try:
        with open_port(args.port) as s:
            if args.command == "log":
                print(read_for(s, args.seconds), end="")
            elif args.command == "time":
                print(send(s, f"E {int(time.time())}"), end="")
            elif args.command == "shot":
                shot(s, args.path)
            elif args.command == "cmd":
                print(send(s, args.line), end="")
            elif args.command == "scan":
                print(send(s, "Q", 40), end="")
            elif args.command == "wifi":
                print(send(s, f"W {args.ssid}|{args.password}", 40), end="")
            elif args.command == "tz":
                print(send(s, "Z " + args.timezone), end="")
            elif args.command == "sync":
                print(send(s, "N", 40), end="")
    except (OSError, ValueError, RuntimeError, TimeoutError, ImportError) as exc:
        parser.exit(1, f"clockctl: {exc}\n")


if __name__ == "__main__":
    main()
