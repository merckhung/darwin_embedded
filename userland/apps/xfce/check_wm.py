#!/usr/bin/env python3
"""Audit original xfwm4/windowing sources and every native image in its runtime."""
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

class NativeWindowManager(unittest.TestCase):
    def test_original_sources_and_complete_native_archives(self):
        for base, project, version in (("xfce", "xfwm4", "4.20.0"), ("xfce-prerequisites", "libwnck", "43.3"),
            ("xfce-prerequisites", "xcb-util", "0.4.1"), ("xfce-prerequisites", "startup-notification", "0.12"),
            ("xfce-prerequisites", "hicolor-icon-theme", "0.18")):
            source = Path("src/Applications") / base / project / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            actual = {str(file.relative_to(source)): sha(file.read_bytes()) for file in source.rglob("*") if file.is_file() and file.name != "UPSTREAM.json"}
            self.assertEqual(actual, manifest["upstream_files_sha256"], project)
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            for stage, category, expected in (("windowing", "libraries", 33), ("wm", "programs", 46)):
                report = json.loads((PACKAGE / f"xfce-{stage}.{arch}.json").read_text())
                bundle = PACKAGE / f"xfce-{stage}.{arch}.tar"; files = tar_files(bundle)
                self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"]); count = 0
                for name, record in report[category].items():
                    prefix = "usr/lib" if category == "libraries" else "usr/libexec/xfce"
                    data = files[f"{prefix}/{name}.a"]
                    self.assertEqual(sha(data), record["archive_sha256"])
                    self.assertEqual(archive_objects(data, cpu), [item["member"] for item in record["sources"]])
                    count += len(record["sources"])
                    for item in record["sources"]:
                        if item["source"].startswith("generated/"):
                            self.assertEqual(sha(files["build-metadata/" + item["source"]]), item["source_sha256"])
                        else:
                            self.assertEqual(sha(Path(item["source"]).read_bytes()), item["source_sha256"])
                    with self.assertRaises(ValueError):
                        archive_objects(data, 0x1000007 if arch == "arm64" else 0x100000c)
                self.assertEqual(count, expected)
                for name, item in report.get("installed_data", {}).items():
                    self.assertEqual(sha(files[name.lstrip("/")]), item["sha256"])
                    self.assertEqual(sha(Path(item["source"]).read_bytes()), item["sha256"])

    def test_signed_runtime_preserves_actual_graph_and_theme_bytes(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-wm-runtime.{arch}.json").read_text())
            bundle = PACKAGE / f"xfce-wm-runtime.{arch}.tar"; files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            records = {**report["libraries"], **report["programs"], **report["modules"], "gio-helper": report["launch_desktop"]}
            previous = json.loads((PACKAGE / f"xfce-ui-runtime.{arch}.json").read_text()); images = {}
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
                for category in ("libraries", "programs", "modules"):
                    if name in previous[category]:
                        self.assertEqual(record["sha256"], previous[category][name]["sha256"])
            self.assertEqual(len(report["libraries"]), 91); self.assertEqual(len(report["programs"]), 21)
            compiled = json.loads((PACKAGE / f"xfce-wm.{arch}.json").read_text())
            for name, item in compiled["installed_data"].items():
                self.assertEqual(sha(files[name.lstrip("/")]), item["sha256"])
            self.assertIn("usr/share/themes/Default/xfwm4/themerc", files)
            self.assertIn("usr/share/icons/hicolor/index.theme", files)
            self.assertFalse(report["desktop_checked"])
            damaged = bytearray(files["usr/bin/xfwm4"]); damaged[-1] ^= 1
            with self.assertRaises(ValueError):
                verify_code_signature(damaged)

if __name__ == "__main__":
    unittest.main()
