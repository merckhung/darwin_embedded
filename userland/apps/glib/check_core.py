#!/usr/bin/env python3
"""Audit actual native GLib/GObject/GModule archives, signatures and bindings."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path("userland/apps/x11").resolve()))
from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports


def sha(data):
    return hashlib.sha256(data).hexdigest()


class CoreLibraries(unittest.TestCase):
    def test_exact_source_and_archives(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/glib/core.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/glib/core.{arch}.json").read_text())
            self.assertEqual({name: len(record["sources"]) for name, record in report["components"].items()},
                             {"glib": 91, "gobject": 21, "gmodule": 2})
            for name, record in report["components"].items():
                archive = files[f"usr/lib/lib{name}-2.0.a"]
                self.assertEqual(sha(archive), record["sha256"])
                self.assertEqual(archive_objects(archive, cpu), [item["member"] for item in record["sources"]])
                for item in record["sources"]:
                    if item["source"].startswith("src/"):
                        self.assertEqual(sha(Path(item["source"]).read_bytes()), item["sha256"])
                with self.assertRaises(ValueError):
                    archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError):
                    archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            self.assertTrue(report["sdk_features"]["Darwin_types"])
            self.assertTrue(report["sdk_features"]["native_poll_socket_values"])
            self.assertTrue(report["sdk_features"]["HAVE_SYSLOG_H"])
            self.assertNotIn("HAVE_SYSLOG_H", report["configuration"])
            self.assertTrue(report["configuration"]["USE_SYSTEM_PRINTF"])
            self.assertTrue(report["configuration"]["THREADS_POSIX"])
            self.assertTrue(report["sdk_features"]["HAVE_MMAP"])
            self.assertTrue(report["configuration"]["HAVE_MMAP"])
            self.assertEqual(files["usr/include/glib-2.0/glibconfig.h"].count(b"@"), 0)

    def test_every_signed_core_api_and_binding(self):
        for arch in ("arm64", "x86_64"):
            files = tar_files(Path(f"userland/apps/glib/core-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/glib/core-runtime.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 7)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            core = json.loads(Path(f"userland/apps/glib/core.{arch}.json").read_text())["components"]
            for name, record in report["libraries"].items():
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                if name in ("libglib-2.0", "libgobject-2.0", "libgmodule-2.0"):
                    project = name[3:].split("-")[0]
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(core[project]["exports"]))
            for path in ("/usr/lib/libglib-2.0.0.dylib", "/usr/lib/libgobject-2.0.0.dylib", "/usr/lib/libgmodule-2.0.0.dylib"):
                data = bytearray(files[path.lstrip("/")])
                data[32 + struct.unpack_from("<I", data, 20)[0] + 16] ^= 1
                with self.assertRaisesRegex(ValueError, "code page hash mismatch"):
                    verify_code_signature(data)
                imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                changed = bytearray(files[path.lstrip("/")])
                position = imported["nlist_offset"] + 6
                description = struct.unpack_from("<H", changed, position)[0]
                struct.pack_into("<H", changed, position, (description & 0xff) | (0xfd << 8))
                with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                    verify_imports(path, {**images, path: image_metadata(changed)}, loader)


if __name__ == "__main__":
    unittest.main()
