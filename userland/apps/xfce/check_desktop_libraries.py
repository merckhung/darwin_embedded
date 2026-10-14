#!/usr/bin/env python3
"""Audit original sources, native object inventories and signed desktop bindings."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

from check_wm import tar_files, archive_objects, verify_code_signature, image_metadata, verify_imports

PACKAGE = Path("userland/apps/xfce")
def sha(data):
    return hashlib.sha256(data).hexdigest()

class NativeDesktopLibraries(unittest.TestCase):
    def test_original_sources_and_complete_native_archives(self):
        for base, project, version in (("xfce", "exo", "4.20.0"), ("xfce", "garcon", "4.20.0"),
            ("xfce", "libxfce4windowing", "4.20.7"), ("xfce-prerequisites", "libdisplay-info", "0.4.0"), ("xfce-prerequisites", "hwdata", "0.412")):
            source = Path("src/Applications") / base / project / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            actual = {str(file.relative_to(source)): sha(file.read_bytes()) for file in source.rglob("*") if file.is_file() and file.name != "UPSTREAM.json"}
            self.assertEqual(actual, manifest["upstream_files_sha256"], project)
        expected = {"libdisplay-info": 15, "libexo-2": 20, "libgarcon-1": 16, "libgarcon-gtk3-1": 1, "libxfce4windowing-0": 25, "libxfce4windowingui-0": 3}
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-desktop-libraries.{arch}.json").read_text())
            bundle = PACKAGE / f"xfce-desktop-libraries.{arch}.tar"; files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual({name: len(record["sources"]) for name, record in report["libraries"].items()}, expected)
            for name, record in report["libraries"].items():
                data = files[f"usr/lib/{name}.a"]; self.assertEqual(sha(data), record["archive_sha256"])
                self.assertEqual(archive_objects(data, cpu), [item["member"] for item in record["sources"]])
                for item in record["sources"]:
                    source = files["build-metadata/generated/" + ("libdisplay-info" if name=="libdisplay-info" else "libxfce4windowing") + "/" + item["source"]] if item["generated"] else Path(item["source"]).read_bytes()
                    self.assertEqual(sha(source), item["source_sha256"])
                with self.assertRaises(ValueError):
                    archive_objects(data, 0x1000007 if arch=="arm64" else 0x100000c)
            self.assertFalse(report["runtime_checked"]); self.assertFalse(report["desktop_checked"])

    def test_signed_graph_and_rejected_corruption(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-desktop-library-runtime.{arch}.json").read_text())
            bundle = PACKAGE / f"xfce-desktop-library-runtime.{arch}.tar"; files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            records = {**report["libraries"], **report["programs"], **report["modules"], "gio-helper": report["launch_desktop"]}
            images = {}
            for name, data in files.items():
                self.assertFalse(data.startswith(b"\x7fELF"), name)
                if data.startswith(b"\xcf\xfa\xed\xfe"):
                    self.assertEqual(struct.unpack_from("<I", data, 4)[0], cpu, name); verify_code_signature(data)
                    images["/" + name] = image_metadata(data)
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in records.items():
                path = record["install_path"]
                self.assertEqual(sha(files[path.lstrip("/")]), record["sha256"], name)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"], name)
            previous = json.loads((PACKAGE / f"xfce-wm-runtime.{arch}.json").read_text())
            for category in ("libraries", "programs", "modules"):
                for name, record in previous[category].items():
                    self.assertEqual(record["sha256"], report[category][name]["sha256"], name)
            self.assertEqual(len(report["libraries"]), 97); self.assertEqual(len(report["programs"]), 21)
            self.assertFalse(report["desktop_checked"])
            damaged = bytearray(files["usr/lib/libxfce4windowing-0.0.dylib"]); damaged[-1] ^= 1
            with self.assertRaises(ValueError):
                verify_code_signature(damaged)

if __name__ == "__main__":
    unittest.main()
