#!/usr/bin/env python3
"""Reject incorrect or truncated scanouts in the native Xorg guest verifier."""
import os
from pathlib import Path
import tempfile
import unittest
from verify_xorg import check_window

class ScanoutTests(unittest.TestCase):
    def test_real_geometry_pixels_and_corruption(self):
        width,height=640,480
        image=bytearray(width*height*3)
        colors=[bytes((255,0,0)),bytes((0,255,0)),bytes((0,0,255)),bytes((255,255,255))]
        for y in range(height//4,3*height//4):
            for x in range(width//4,3*width//4):
                image[(y*width+x)*3:(y*width+x)*3+3]=colors[(x-width//4)*4//(width//2)]
        header=f"P6\n{width} {height}\n255\n".encode()
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as temporary:
            path=Path(temporary)/"scanout.ppm";path.write_bytes(header+image)
            samples=check_window(path,width,height);self.assertEqual(len(samples),12)
            with self.assertRaises(RuntimeError):check_window(path,width+1,height)
            sample=samples[0];offset=(sample["y"]*width+sample["x"])*3
            image[offset]^=1;path.write_bytes(header+image)
            with self.assertRaises(RuntimeError):check_window(path,width,height)
            path.write_bytes(header+image[:-1])
            with self.assertRaises(RuntimeError):check_window(path,width,height)

if __name__=="__main__":unittest.main()
