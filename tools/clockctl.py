#!/usr/bin/env python3
"""Talk to the fancy clock over USB serial.

  clockctl.py log [seconds]      print the device log
  clockctl.py time               set the RTC to this computer's local time
  clockctl.py shot [out.png]     grab a screenshot
  clockctl.py cmd <C|M|S>        next theme / toggle 12-24h / status
"""
import sys, time, datetime
import serial

PORT = "/dev/ttyACM0"


def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 0.2
    s.dtr = True  # keep DTR+RTS asserted: the ESP auto-reset needs a DTR/RTS transition
    s.rts = True
    s.open()
    return s


def read_for(s, secs):
    end = time.time() + secs
    out = b""
    while time.time() < end:
        out += s.read(4096)
    return out.decode(errors="replace")


def send(s, line, wait=1.0):
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    return read_for(s, wait)


def shot(s, path):
    s.reset_input_buffer()
    s.write(b"P\n")
    buf = b""
    t0 = time.time()
    while b"SNAP " not in buf or buf.find(b"\n", buf.find(b"SNAP ")) < 0:
        buf += s.read(4096)
        if time.time() - t0 > 10:
            sys.exit("no screenshot header received:\n" + buf.decode(errors="replace"))
    i = buf.find(b"SNAP ")
    j = buf.find(b"\n", i)
    w, h = map(int, buf[i + 5:j].split())
    buf = buf[j + 1:]
    need = w * h * 2
    t0 = time.time()
    while len(buf) < need:
        buf += s.read(65536)
        if time.time() - t0 > 60:
            sys.exit(f"timeout: got {len(buf)}/{need} bytes")
    import numpy as np
    from PIL import Image
    a = np.frombuffer(buf[:need], dtype="<u2").reshape(h, w).astype(np.uint32)
    r = ((a >> 11) & 31) * 255 // 31
    g = ((a >> 5) & 63) * 255 // 63
    b = (a & 31) * 255 // 31
    Image.fromarray(np.dstack([r, g, b]).astype(np.uint8)).save(path)
    print(f"saved {path} ({w}x{h})")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "log"
    s = open_port()
    if cmd == "log":
        print(read_for(s, float(sys.argv[2]) if len(sys.argv) > 2 else 3), end="")
    elif cmd == "time":
        now = datetime.datetime.now()
        print(send(s, "T " + now.strftime("%Y-%m-%d %H:%M:%S")), end="")
    elif cmd == "shot":
        shot(s, sys.argv[2] if len(sys.argv) > 2 else "shot.png")
    elif cmd == "cmd":
        print(send(s, sys.argv[2]), end="")
    elif cmd == "scan":
        s.reset_input_buffer()
        s.write(b"Q\n")
        print(read_for(s, 12), end="")
    elif cmd == "wifi":  # clockctl.py wifi SSID [PASSWORD]  (password is never echoed back by the device)
        ssid = sys.argv[2]
        pw = sys.argv[3] if len(sys.argv) > 3 else ""
        print(send(s, f"W {ssid}|{pw}", 1.0), end="")
        print(read_for(s, 25), end="")
    elif cmd == "tz":    # clockctl.py tz "PST8PDT,M3.2.0,M11.1.0"
        print(send(s, "Z " + (sys.argv[2] if len(sys.argv) > 2 else "")), end="")
    elif cmd == "sync":
        s.reset_input_buffer()
        s.write(b"N\n")
        print(read_for(s, 25), end="")
    else:
        sys.exit(__doc__)


main()
