#!/usr/bin/env python3
"""Audit complete GIO source inventories and actual signed Mach-O bindings."""
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


class GioLibraries(unittest.TestCase):
    def test_exact_native_sources_and_configuration(self):
        for manifest in Path("src/Applications/resolver").rglob("UPSTREAM.json"):
            for name, expected in json.loads(manifest.read_text())["files_sha256"].items():
                self.assertEqual(sha((manifest.parent / name).read_bytes()), expected)
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/glib/gio.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/glib/gio.{arch}.json").read_text())
            archive = files["usr/lib/libgio-2.0.a"]
            self.assertEqual(len(report["sources"]), 240)
            self.assertEqual(sha(archive), report["archive_sha256"])
            self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in report["sources"]])
            self.assertEqual(len({entry["source"] for entry in report["sources"]}), 240)
            adapted = set()
            for entry in report["sources"]:
                if entry["source"].startswith("src/"):
                    self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["sha256"])
                    if entry["sha256"] != entry["compiled_sha256"]:
                        adapted.add(Path(entry["source"]).name)
            self.assertEqual(adapted, {"giomodule.c", "gthreadedresolver.c"})
            self.assertEqual(sum("/gio/kqueue/" in entry["source"] for entry in report["sources"]), 4)
            self.assertEqual(sum("/gio/xdgmime/" in entry["source"] for entry in report["sources"]), 8)
            self.assertTrue(report["configuration"]["FILE_MONITOR_BACKEND_KQUEUE"])
            self.assertTrue(report["configuration"]["HAVE_MMAP"])
            self.assertTrue(report["configuration"]["HAVE_XATTR_NOFOLLOW"])
            self.assertNotIn("HAVE_RES_NQUERY", report["configuration"])
            self.assertNotIn("HAVE_COCOA", report["configuration"])
            self.assertNotIn("FILE_MONITOR_BACKEND_INOTIFY", report["configuration"])
            self.assertIn("_native_resolver_query", report["system_requests"])
            self.assertIn("_native_resolver_expand", report["system_requests"])
            self.assertNotIn("_h_errno", report["system_requests"])
            self.assertNotIn("_res_9_query", report["system_requests"])
            self.assertEqual(struct.unpack_from("<4I", files["usr/libexec/gio-launch-desktop.o"])[::3], (0xfeedfacf, 1))
            for generator in report["generators"]:
                for name, expected in generator["inputs"].items():
                    self.assertEqual(sha(Path(name).read_bytes()), expected)
            for name in ("gnetworking.h", "gioenumtypes.h", "gio-visibility.h"):
                self.assertNotIn(b"@NAMESER_COMPAT_INCLUDE@", files["usr/include/glib-2.0/gio/" + name])
            with self.assertRaises(ValueError):
                archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError):
                archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)

    def test_full_signed_gio_apis_and_actual_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/glib/gio-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/glib/gio-runtime.{arch}.json").read_text())
            source = json.loads(Path(f"userland/apps/glib/gio.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 9)
            images = {"/" + name: image_metadata(data) for name, data in files.items()
                      if name.endswith(".dylib") or name == "usr/libexec/gio-launch-desktop"}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for record in [*report["libraries"].values(), report["launch_desktop"]]:
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<I", data, 4)[0], cpu)
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
            path = "/usr/lib/libgio-2.0.0.dylib"
            self.assertEqual(set(report["libraries"]["libgio-2.0"]["exports"]) - {"___dso_handle"}, set(source["exports"]))
            self.assertGreater(len(source["exports"]), 1500)
            data = bytearray(files[path.lstrip("/")])
            data[32 + struct.unpack_from("<I", data, 20)[0] + 16] ^= 1
            with self.assertRaisesRegex(ValueError, "code page hash mismatch"):
                verify_code_signature(data)
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            data = bytearray(files[path.lstrip("/")])
            position = imported["nlist_offset"] + 6
            description = struct.unpack_from("<H", data, position)[0]
            struct.pack_into("<H", data, position, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(data)}, loader)


if __name__ == "__main__":
    unittest.main()
