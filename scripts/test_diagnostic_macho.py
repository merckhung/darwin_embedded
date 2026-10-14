"""Check diagnostic signing and the launcher's rejection of unsupported images."""
import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from sign_diagnostic_macho import sign

ROOT = Path(__file__).resolve().parents[1]


def image(extra=b""):
    segment = struct.pack("<II16s4Q4I", 0x19, 72, b"__TEXT", 0x100000000, 4096, 0, 4096, 5, 5, 0, 0)
    linkedit = struct.pack("<II16s4Q4I", 0x19, 72, b"__LINKEDIT", 0x100001000, 4096, 4096, 0, 1, 1, 0, 0)
    entry = struct.pack("<IIQQ", 0x80000028, 24, 512, 0)
    signature = struct.pack("<IIII", 0x1d, 16, 4096, 0)
    commands = segment + linkedit + entry + signature + extra
    header = struct.pack("<8I", 0xfeedfacf, 0x100000c, 0, 2, 4 + bool(extra), len(commands), 0x200004, 0)
    return (header + commands).ljust(4096, b"\0")


class DiagnosticMachOTests(unittest.TestCase):
    def test_signature_covers_modified_header_and_code_pages(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "diagnostic"
            path.write_bytes(image())
            sign(path)
            data = path.read_bytes()
            magic, total, count, slot, offset = struct.unpack_from(">5I", data, 4096)
            self.assertEqual((magic, total, count, slot, offset), (0xfade0cc0, len(data) - 4096, 1, 0, 20))
            cd = data[4096 + offset:]
            fields = struct.unpack_from(">9I4B4I4Q", cd)
            self.assertEqual(fields[:4], (0xfade0c02, len(cd), 0x20400, 0x20302))
            self.assertEqual(fields[6:13], (0, 1, 4096, 32, 2, 0, 12))
            digest = cd[fields[4]:fields[4] + 32]
            self.assertEqual(digest, hashlib.sha256(data[:4096]).digest())
            changed = bytearray(data[:4096]); changed[512] ^= 1
            self.assertNotEqual(digest, hashlib.sha256(changed).digest())
            self.assertEqual(struct.unpack_from("<I", data, 32 + 72 + 72 + 24 + 12)[0], total)
            self.assertEqual(struct.unpack_from("<Q", data, 32 + 72 + 48)[0], total)
            # Signing an already signed file must still produce correct hashes.
            sign(path)
            self.assertEqual(data, path.read_bytes())

    def test_signature_rejects_overlapping_or_missing_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "diagnostic"
            bad = bytearray(image())
            struct.pack_into("<I", bad, 32 + 72 + 72 + 24 + 8, 32)
            path.write_bytes(bad)
            with self.assertRaises(ValueError): sign(path)
            path.write_bytes(b"not a Mach-O")
            with self.assertRaises(ValueError): sign(path)

    def test_only_executable_signature_grants_main_executable_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            for kind in (2, 6, 7, 8):
                with self.subTest(filetype=kind):
                    path = Path(directory) / "native-image"
                    data = bytearray(image())
                    struct.pack_into("<I", data, 12, kind)
                    path.write_bytes(data)
                    sign(path)
                    signed = path.read_bytes()
                    fields = struct.unpack_from(">9I4B4I4Q", signed, 4096 + 20)
                    self.assertEqual(fields[-1], int(kind == 2))
                    self.assertEqual(signed[4096 + 20 + fields[4]:][:32],
                                     hashlib.sha256(signed[:4096]).digest())

    def test_launcher_rejects_dependencies_fixups_and_non_executable_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "launcher.so"
            subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Werror",
                            str(ROOT / "userland/diagnostic/dyld.c"), "-o", str(library)], check=True)
            entry = ctypes.CDLL(str(library)).diagnostic_entry
            entry.argtypes, entry.restype = [ctypes.c_void_p], ctypes.c_size_t
            def check(data):
                buffer = ctypes.create_string_buffer(data)
                return entry(buffer), ctypes.addressof(buffer)
            target, base = check(image())
            self.assertEqual(target, base + 512)
            dependency = struct.pack("<II", 0xc, 8)
            self.assertEqual(check(image(dependency))[0], 0)
            fixups = struct.pack("<12I", 0x80000022, 48, 4096, 1, 0, 0, 0, 0, 0, 0, 0, 0)
            self.assertEqual(check(image(fixups))[0], 0)
            bad = bytearray(image()); struct.pack_into("<I", bad, 32 + 60, 1)
            self.assertEqual(check(bytes(bad))[0], 0)
            bad = bytearray(image()); struct.pack_into("<I", bad, 32 + 4, 7)
            self.assertEqual(check(bytes(bad))[0], 0)


if __name__ == "__main__":
    unittest.main()
