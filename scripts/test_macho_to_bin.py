import struct
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from macho_to_bin import flatten, parse_macho


BASE = 0xFFFFFE0007004000


def executable(architecture="arm64", main=False):
    cpu = 0x0100000C if architecture == "arm64" else 0x01000007
    def segment(name, address, memory_size, file_offset, file_size, permissions):
        return struct.pack("<II16s4Q4I", 0x19, 72, name.encode(), address,
                           memory_size, file_offset, file_size, permissions, permissions, 0, 0)
    commands = [segment("__TEXT", BASE, 0x1000, 0, 512, 5),
                segment("__DATA", BASE + 0x1000, 0x1000, 512, 4, 3)]
    if main:
        commands.append(struct.pack("<IIQQ", 0x80000028, 24, 0x180, 0))
    else:
        flavor, words, pc_offset = (6, 68, 256) if architecture == "arm64" else (4, 42, 128)
        state = bytearray(words * 4)
        struct.pack_into("<Q", state, pc_offset, BASE + 0x180)
        commands.append(struct.pack("<4I", 5, 16 + len(state), flavor, words) + state)
    header = struct.pack("<8I", 0xFEEDFACF, cpu, 0, 2, len(commands),
                         sum(map(len, commands)), 1, 0)
    contents = bytearray(header + b"".join(commands))
    contents.extend(bytes(512 - len(contents)))
    return contents + b"DATA"


class MachOTests(unittest.TestCase):
    def test_arm_entry_and_zero_fill(self):
        contents = executable()
        layout = parse_macho(contents)
        self.assertEqual(layout["virtual_entry"], BASE + 0x180)
        image = flatten(contents, layout)
        self.assertEqual(len(image), 0x2000)
        self.assertEqual(image[0x1000:0x1004], b"DATA")
        self.assertEqual(image[0x1004:], bytes(0xFFC))

    def test_x86_thread_entry(self):
        self.assertEqual(parse_macho(executable("x86_64"))["virtual_entry"], BASE + 0x180)

    def test_main_entry(self):
        self.assertEqual(parse_macho(executable("x86_64", main=True))["virtual_entry"], BASE + 0x180)

    def test_truncated_and_invalid_inputs(self):
        for data in (b"bad", executable()[:-1]):
            with self.assertRaises(ValueError):
                parse_macho(data)
        data = executable()
        struct.pack_into("<I", data, 36, 0)
        with self.assertRaises(ValueError):
            parse_macho(data)

    def test_overlap_and_unloaded_entry(self):
        data = executable()
        struct.pack_into("<Q", data, 32 + 72 + 24, BASE)
        with self.assertRaises(ValueError):
            parse_macho(data)
        data = executable()
        struct.pack_into("<Q", data, 32 + 144 + 16 + 256, BASE + 0x1800)
        with self.assertRaises(ValueError):
            parse_macho(data)

    def test_fileset_aggregate_overlap(self):
        data = executable()
        struct.pack_into("<I", data, 12, 12)
        # A constituent shares bytes already published by an aggregate segment.
        struct.pack_into("<Q", data, 32 + 72 + 24, BASE + 0x100)
        struct.pack_into("<Q", data, 32 + 72 + 40, 0x100)
        layout = parse_macho(data)
        self.assertEqual(flatten(data, layout)[:512], data[:512])
        # Mapping different initialized bytes at the same address is invalid.
        struct.pack_into("<Q", data, 32 + 72 + 40, 512)
        with self.assertRaisesRegex(ValueError, "conflicting initialized"):
            parse_macho(data)

    def test_virtual_bias_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source, output, metadata = work / "kernel", work / "flat", work / "layout.json"
            source.write_bytes(executable())
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("macho_to_bin.py")),
                                     str(source), str(output), "--virtual-bias", hex(BASE - 0x7004000),
                                     "--layout", str(metadata)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            layout = json.loads(metadata.read_text())
            self.assertEqual(layout["physical_base"], 0x7004000)
            self.assertEqual(layout["physical_entry"], 0x7004180)
            self.assertEqual(len(output.read_bytes()), layout["image_size"])


if __name__ == "__main__":
    unittest.main()
