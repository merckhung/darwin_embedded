#!/usr/bin/env python3
"""Audit original Xfce sources, native objects and the full runtime dependency graph."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports

PACKAGE = Path("userland/apps/xfce")
def sha(data):
    return hashlib.sha256(data).hexdigest()

class NativeXfceBase(unittest.TestCase):
    def test_original_source_bytes_and_service(self):
        for project, version in (("libxfce4util", "4.20.1"), ("xfconf", "4.20.0"), ("libxfce4ui", "4.20.2"),
                                 ("libICE", "1.1.2"), ("libSM", "1.2.6")):
            base = "xfce-prerequisites" if project in ("libICE", "libSM") else "xfce"
            source = Path("src/Applications") / base / project / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            self.assertTrue(manifest["publisher_digest_verified"])
            actual = {str(path.relative_to(source)): sha(path.read_bytes())
                      for path in source.rglob("*") if path.is_file() and path.name != "UPSTREAM.json"}
            self.assertEqual(actual, manifest["upstream_files_sha256"])
        for arch in ("arm64", "x86_64"):
            files = tar_files(PACKAGE / f"xfce-base-runtime.{arch}.tar")
            service = files["usr/share/dbus-1/services/org.xfce.Xfconf.service"].decode()
            self.assertIn("Name=org.xfce.Xfconf\n", service)
            self.assertIn("Exec=/usr/libexec/xfce4/xfconf/xfconfd\n", service)
            self.assertIn("usr/libexec/xfce4/xfconf/xfconfd", files)

    def test_original_native_session_objects(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-sessions.{arch}.json").read_text())
            bundle = PACKAGE / f"xfce-sessions.{arch}.tar"
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            files = tar_files(bundle)
            count = 0
            for name, record in report["libraries"].items():
                data = files[f"usr/lib/{name}.a"]
                self.assertEqual(sha(data), record["archive_sha256"])
                self.assertEqual(archive_objects(data, cpu), [item["member"] for item in record["sources"]])
                count += len(record["sources"])
                for item in record["sources"]:
                    self.assertEqual(sha(Path(item["source"]).read_bytes()), item["source_sha256"])
            self.assertEqual(count, 25)
            self.assertFalse(report["runtime_checked"])

    def test_native_archives_signed_images_and_retained_graph(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            build = json.loads((PACKAGE / f"xfce-base.{arch}.json").read_text())
            objects = tar_files(PACKAGE / f"xfce-base.{arch}.tar")
            self.assertEqual(sha((PACKAGE / f"xfce-base.{arch}.tar").read_bytes()), build["bundle_sha256"])
            count = 0
            for category in ("libraries", "programs"):
                for name, record in build[category].items():
                    prefix = "usr/lib" if category == "libraries" else "usr/libexec/xfce"
                    data = objects[f"{prefix}/{name}.a"]
                    self.assertEqual(sha(data), record["archive_sha256"])
                    self.assertEqual(archive_objects(data, cpu), [item["member"] for item in record["sources"]])
                    count += len(record["sources"])
                    for item in record["sources"]:
                        self.assertEqual(sha(Path(item["source"]).read_bytes()), item["source_sha256"])
                    with self.assertRaises(ValueError):
                        archive_objects(data, 0x1000007 if arch == "arm64" else 0x100000c)
            self.assertEqual(count, 40)
            runtime = json.loads((PACKAGE / f"xfce-base-runtime.{arch}.json").read_text())
            files = tar_files(PACKAGE / f"xfce-base-runtime.{arch}.tar")
            self.assertEqual(sha((PACKAGE / f"xfce-base-runtime.{arch}.tar").read_bytes()), runtime["bundle_sha256"])
            previous = json.loads(Path(f"userland/apps/x11/xorg-runtime.{arch}.json").read_text())
            records = {**runtime["libraries"], **runtime["programs"], **runtime["modules"], "gio-helper": runtime["launch_desktop"]}
            images = {}
            for name, data in files.items():
                self.assertFalse(data.startswith(b"\x7fELF"), name)
                if data.startswith(b"\xcf\xfa\xed\xfe"):
                    self.assertEqual(struct.unpack_from("<I", data, 4)[0], cpu, name)
                    verify_code_signature(data)
                    images["/" + name] = image_metadata(data)
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in records.items():
                path = record["install_path"]
                self.assertEqual(sha(files[path.lstrip("/")]), record["sha256"], name)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"], name)
                for category in ("libraries", "programs", "modules"):
                    if name in previous[category]:
                        self.assertEqual(record["sha256"], previous[category][name]["sha256"], name)
            self.assertEqual(len(runtime["libraries"]), 84)
            self.assertEqual(len(runtime["programs"]), 16)
            self.assertFalse(runtime["desktop_checked"])
            for name in ("libxfce4util", "libxfconf-0", "xfconfd", "xfconf-query"):
                corrupted = bytearray(files[records[name]["install_path"].lstrip("/")])
                corrupted[-1] ^= 1
                with self.assertRaises(ValueError):
                    verify_code_signature(corrupted)

if __name__ == "__main__":
    unittest.main()
