import importlib.util
from pathlib import Path
import unittest
import zlib

spec = importlib.util.spec_from_file_location("clockctl", Path(__file__).parents[1] / "tools/clockctl.py")
clockctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clockctl)


class Serial:
    def __init__(self, data, chunk=1):
        self.data, self.chunk, self.sent = data, chunk, b""

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.sent += data
        return len(data)

    def read(self, count):
        n = min(count, self.chunk)
        data, self.data = self.data[:n], self.data[n:]
        return data


class Clock:
    def __init__(self):
        self.t = 0

    def __call__(self):
        self.t += 0.1
        return self.t


def packet(pixels=b"\x00\xf8\xe0\x07", trailer=None):
    trailer = trailer or f"\nENDSNAP {zlib.crc32(pixels):08x}\n".encode()
    return b"net: time synced\nSNAP 2 1\n" + pixels + trailer


class Screenshots(unittest.TestCase):
    def test_fragmented_packet(self):
        s = Serial(packet())
        self.assertEqual(clockctl.read_screenshot(s), (2, 1, b"\x00\xf8\xe0\x07"))
        self.assertEqual(s.sent, b"P\n")

    def test_single_read_and_framebuffer(self):
        s = Serial(packet() + b"net: next log\n", 65536)
        self.assertEqual(clockctl.read_screenshot(s, True)[0:2], (2, 1))
        self.assertEqual(s.sent, b"F\n")

    def test_corrupted_payload(self):
        s = Serial(packet().replace(b"\x00\xf8", b"\x01\xf8"), 65536)
        with self.assertRaisesRegex(ValueError, "checksum"):
            clockctl.read_screenshot(s)

    def test_interleaved_log(self):
        s = Serial(packet().replace(b"\x00\xf8", b"\x00net: scanning\n\xf8"), 65536)
        with self.assertRaises(ValueError):
            clockctl.read_screenshot(s)

    def test_truncated_payload_times_out(self):
        with self.assertRaises(TimeoutError):
            clockctl.read_screenshot(Serial(b"SNAP 2 1\n\x00", 65536), now=Clock())

    def test_missing_header_times_out(self):
        with self.assertRaises(TimeoutError):
            clockctl.read_screenshot(Serial(b""), now=Clock())

    def test_failed_capture(self):
        with self.assertRaises(RuntimeError):
            clockctl.read_screenshot(Serial(b"SNAPFAIL\n", 65536))

    def test_dimensions_bounded(self):
        with self.assertRaises(ValueError):
            clockctl.read_screenshot(Serial(b"SNAP 99999999 99999999\n", 65536))

    def test_old_protocol_rejected(self):
        with self.assertRaisesRegex(ValueError, "update firmware"):
            clockctl.read_screenshot(Serial(packet(trailer=b"\nENDSNAP\n"), 65536))


if __name__ == "__main__":
    unittest.main()
