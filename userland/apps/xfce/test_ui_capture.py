#!/usr/bin/env python3
"""Reject invalid geometry, truncation and wrong pixels in Xfce UI captures."""
import os
import sys
import pexpect
from pathlib import Path
import tempfile
import unittest
from verify_ui import check_area

class XfceScanout(unittest.TestCase):
    def test_interactive_serial_handoff_preserves_raw_output_and_input(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as directory:
            logfile = Path(directory) / "serial.log"
            # Run the real console bridge inside an outer PTY so interact()
            # receives a controlling terminal, just as in gnome-terminal.
            guest = "import sys; print('SERIAL_READY',flush=True); line=input(); print('SERIAL_REPLY:π:'+line,flush=True); input()"
            bridge = "\n".join([
                "import pexpect,sys",
                "sys.path.insert(0," + repr(str(Path(__file__).resolve().parent)) + ")",
                "from verify_ui import interactive_console",
                "with open(sys.argv[1],'w') as log:",
                "    child=pexpect.spawn(sys.executable,['-u','-c'," + repr(guest) + "],encoding='utf-8',echo=False)",
                "    child.logfile_read=log",
                "    child.expect('SERIAL_READY')",
                "    print('BRIDGE_READY',flush=True)",
                "    try:",
                "        interactive_console(child,log)",
                "        assert child.logfile_read is log",
                "        print('BRIDGE_EXIT',flush=True)",
                "    finally: child.terminate(force=True)",
            ])
            child=pexpect.spawn(sys.executable,['-u','-c',bridge,str(logfile)],encoding='utf-8',echo=False,timeout=10)
            try:
                child.expect('BRIDGE_READY')
                child.sendline('hardware-console-check')
                child.expect('SERIAL_REPLY:π:hardware-console-check')
                child.send(chr(29)); child.expect('BRIDGE_EXIT'); child.expect(pexpect.EOF)
                child.close(); self.assertEqual(child.exitstatus,0)
                contents=logfile.read_bytes()
                self.assertIn(b'SERIAL_READY',contents)
                self.assertIn('SERIAL_REPLY:π:hardware-console-check'.encode(),contents)
            finally:
                if child.isalive():child.terminate(force=True)

    def test_actual_area_geometry_and_rejected_corruption(self):
        width, height, left, top, w, h = 640, 480, 47, 91, 320, 200
        pixels = bytearray(width * height * 3)
        colors = [bytes((255, 0, 0)), bytes((0, 255, 0)), bytes((0, 0, 255)), bytes((255, 255, 255))]
        for y in range(top, top + h):
            for x in range(left, left + w):
                pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] = colors[(x - left) * 4 // w]
        header = f"P6\n{width} {height}\n255\n".encode()
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as directory:
            path = Path(directory) / "scanout.ppm"; path.write_bytes(header + pixels)
            samples = check_area(path, width, height, left, top, w, h); self.assertEqual(len(samples), 12)
            for bounds in ((-1, top, w, h), (left, top, width, h), (left, top, w, 3)):
                with self.assertRaises(RuntimeError):
                    check_area(path, width, height, *bounds)
            with self.assertRaises(RuntimeError):
                check_area(path, width + 1, height, left, top, w, h)
            sample = samples[0]; pixels[(sample["y"] * width + sample["x"]) * 3] ^= 1; path.write_bytes(header + pixels)
            with self.assertRaises(RuntimeError):
                check_area(path, width, height, left, top, w, h)
            path.write_bytes(header + pixels[:-1])
            with self.assertRaises(RuntimeError):
                check_area(path, width, height, left, top, w, h)

if __name__ == "__main__":
    unittest.main()
