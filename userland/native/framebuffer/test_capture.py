"""Host fixtures verify screenshot validation; none are guest display evidence."""
import importlib.util
import os
import re
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("capture", Path(__file__).with_name("verify.py"))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureTests(unittest.TestCase):
    def test_real_console_line_endings_and_proof_boundaries(self):
        pattern = capture.serial_line("NATIVE_FRAMEBUFFER_OK:" + re.escape("fresh:25.5.0:x86_64"))
        for text in ("\r\r\nNATIVE_FRAMEBUFFER_OK:fresh:25.5.0:x86_64\r\r\n",
                     "kPEEnableScreen -1\r\n\rNATIVE_FRAMEBUFFER_OK:fresh:25.5.0:x86_64\r\r\r\n"):
            self.assertIsNotNone(re.search(pattern, text))
        for text in ('printf "\\nNATIVE_FRAMEBUFFER_OK:fresh:25.5.0:x86_64\\n"\n',
                     "\nNATIVE_FRAMEBUFFER_OK:old:25.5.0:x86_64\n", "\nNATIVE_FRAMEBUFFER_OK:fresh:25.5.0:arm64\n",
                     "\nNATIVE_FRAMEBUFFER_OK:fresh:25.5.0:x86_64:unexpected\n"):
            self.assertIsNone(re.search(pattern, text))
        stream = "\r\nNativeBootFramebuffer: console claimed\r\n\rNATIVE_FRAMEBUFFER_OK:fresh:25.5.0:x86_64\r\r\n"
        first = re.search(capture.serial_line("NativeBootFramebuffer: console claimed"), stream)
        self.assertIsNotNone(first)
        self.assertIsNotNone(re.search(pattern, stream[first.end():]))

    def test_pixel_and_geometry_rejections(self):
        width, height = 112, 64
        colors = [bytes(rgb) for rgb in ((255,0,0), (0,255,0), (0,0,255), (255,255,0),
                                        (0,255,255), (255,0,255), (255,255,255))]
        raw = bytearray(width * height * 3)
        for y in range(height // 4, 3 * height // 4):
            for x in range(width // 2):
                offset = (y * width + width // 4 + x) * 3
                raw[offset:offset + 3] = colors[x * 7 // (width // 2)]
        header = f"P6\n{width} {height}\n255\n".encode()
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as directory:
            path = Path(directory) / "host-fixture.ppm"
            path.write_bytes(header + raw)
            self.assertEqual(len(capture.check_screenshot(path, width, height)), 21)
            with self.assertRaises(RuntimeError):
                capture.check_screenshot(path, width + 1, height)
            path.write_bytes(header + raw[:-1])
            with self.assertRaises(RuntimeError):
                capture.check_screenshot(path, width, height)
            path.write_bytes(header + bytes(len(raw)))
            with self.assertRaises(RuntimeError):
                capture.check_screenshot(path, width, height)
            # A channel-swapped image must not pass a grayscale-only comparison.
            swapped = bytearray(raw)
            for offset in range(0, len(swapped), 3):
                swapped[offset], swapped[offset + 2] = swapped[offset + 2], swapped[offset]
            path.write_bytes(header + swapped)
            with self.assertRaises(RuntimeError):
                capture.check_screenshot(path, width, height)


if __name__ == "__main__":
    unittest.main()
