#!/usr/bin/env python3
"""Audit native original JPEG/GdkPixbuf objects and the complete desktop graph."""
import hashlib
import json
from pathlib import Path
import struct
import unittest
from check_wm import tar_files, archive_objects, verify_code_signature, image_metadata
from link_auth import verify_closure, verify_imports

PACKAGE = Path("userland/apps/xfce")
def sha(data):
    return hashlib.sha256(data).hexdigest()

class NativeJpeg(unittest.TestCase):
    def test_original_sources_and_native_objects(self):
        source = Path("src/Applications/xfce-prerequisites/libjpeg-turbo/3.2.0")
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        actual = {str(p.relative_to(source)): sha(p.read_bytes()) for p in source.rglob("*") if p.is_file() and p.name != "UPSTREAM.json"}
        self.assertEqual(actual, manifest["upstream_files_sha256"])
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            for prefix, count in (("jpeg", 101), ("jpeg-pixbuf", 29)):
                report = json.loads((PACKAGE / f"{prefix}.{arch}.json").read_text())
                bundle = PACKAGE / f"{prefix}.{arch}.tar"; files = tar_files(bundle)
                self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
                self.assertEqual(sum(len(r["sources"]) for r in report["libraries"].values()), count)
                for name, record in report["libraries"].items():
                    data = files[f"usr/lib/{name}.a"]
                    if "archive_sha256" in record:
                        self.assertEqual(sha(data), record["archive_sha256"])
                    self.assertEqual(archive_objects(data, cpu), [r["member"] for r in record["sources"]])
                    for item in record["sources"]:
                        if item["source"].startswith("src/"):
                            self.assertEqual(sha(Path(item["source"]).read_bytes()), item["source_sha256"])
                    with self.assertRaises(ValueError):
                        archive_objects(data, 0x1000007 if arch == "arm64" else 0x100000c)
                if prefix == "jpeg-pixbuf":
                    self.assertIn("jpeg", report["builtin_loaders"])

    def test_signed_desktop_startup_graph(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-jpeg-runtime.{arch}.json").read_text())
            bundle = PACKAGE / f"xfce-jpeg-runtime.{arch}.tar"; files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual([len(report[k]) for k in ("libraries", "programs", "modules")], [100, 31, 12])
            images = {}
            for path, data in files.items():
                self.assertFalse(data.startswith(b"\x7fELF"), path)
                if data.startswith(b"\xcf\xfa\xed\xfe"):
                    self.assertEqual(struct.unpack_from("<I", data, 4)[0], cpu)
                    verify_code_signature(data); images["/" + path] = image_metadata(data)
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for category in ("libraries", "programs", "modules"):
                for record in report[category].values():
                    path = record["install_path"]
                    self.assertEqual(sha(files[path.lstrip("/")]), record["sha256"])
                    self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])
                    if "startup_closure" in record:
                        self.assertEqual(verify_closure(path, images, loader), record["startup_closure"])
                    elif category != "libraries":
                        self.fail("executable/module startup closure missing: " + path)
            damaged = bytearray(files["usr/lib/libjpeg.62.dylib"]); damaged[-1] ^= 1
            with self.assertRaises(ValueError):
                verify_code_signature(damaged)

if __name__ == "__main__":
    unittest.main()
