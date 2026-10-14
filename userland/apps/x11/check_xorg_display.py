#!/usr/bin/env python3
"""Audit native Xorg display objects and genuine available source providers."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

from check_clients import tar_files
from link_auth import image_metadata
sys.path.insert(0, str(Path("scripts").resolve()))
from native_components import symbols


def sha(data): return hashlib.sha256(data).hexdigest()


def validate_object(data, cpu):
    if len(data) < 32 or struct.unpack_from("<4I", data)[::3] != (0xfeedfacf, 1) or struct.unpack_from("<I", data, 4)[0] != cpu:
        raise ValueError("wrong native display object type/CPU/magic")


class NativeDisplayArtifacts(unittest.TestCase):
    def test_native_object_source_and_real_available_providers(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            path = Path(f"userland/apps/x11/xorg-display.{arch}.o"); data = path.read_bytes()
            record = json.loads(path.with_suffix(".json").read_text())
            self.assertEqual(sha(data), record["object_sha256"])
            self.assertEqual(sha(Path("userland/apps/x11/xorg_display.c").read_bytes()), record["source_sha256"])
            validate_object(data, cpu)
            with self.assertRaises(ValueError): validate_object(data, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError): validate_object(b"\x7fELF" + data[4:], cpu)
            actual = symbols(path)
            self.assertEqual(actual[0], set(record["exports"])); self.assertEqual(actual[1], set(record["imports"]))
            self.assertTrue({"_InitOutput", "_ddxGiveUp", "_OsVendorFatalError"} <= actual[0])
            self.assertTrue({"_IOConnectMapMemory64", "_IOConnectUnmapMemory64", "_IOConnectCallStructMethod", "_fbScreenInit", "_fbPictureInit", "_RRCrtcNotify", "_miDCInitialize"} <= actual[1])
            core = json.loads(Path(f"userland/apps/x11/xorg-core.{arch}.json").read_text())
            available = set(core["definitions"])
            available |= symbols(Path(f"userland/native/iokit/libnative_iokit.{arch}.a"))[0]
            # The framebuffer probe's signed System selects fewer exports
            # than Xorg. Audit additional declarations against real Libc
            # source objects; final server linkage must select them explicitly.
            available |= symbols(Path(f"userland/native/libc/libc_base.{arch}.a"))[0]
            runtime = tar_files(Path(f"userland/native/framebuffer/system_{arch}.tar"))
            for name, contents in runtime.items():
                if name.endswith(".dylib"): available |= image_metadata(contents)["exports"]
            self.assertEqual(actual[1] - available, set())
            self.assertFalse(record["input_backend_supplied"]); self.assertFalse(record["server_program_linked"]); self.assertFalse(record["runtime_checked"])


if __name__ == "__main__": unittest.main()
