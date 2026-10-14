#!/usr/bin/env python3
"""Verify native Xfce UI/session binaries, real RPC source and installed font data."""
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

class NativeXfceUI(unittest.TestCase):
    def test_original_objects_and_real_bootstrap_rpc(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-ui.{arch}.json").read_text()); bundle = PACKAGE / f"xfce-ui.{arch}.tar"
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"]); files = tar_files(bundle); count = 0
            for name, record in report["libraries"].items():
                data = files[f"usr/lib/{name}.a"]
                self.assertEqual(sha(data), record["archive_sha256"])
                self.assertEqual(archive_objects(data, cpu), [item["member"] for item in record["sources"]])
                for item in record["sources"]:
                    self.assertEqual(sha(Path(item["source"]).read_bytes()), item["source_sha256"])
                count += len(record["sources"])
            self.assertEqual(count, 22)
            self.assertEqual((report["configuration"]["ENABLE_X11"], report["configuration"]["ENABLE_LIBSM"]), (1, 1))
            rpc_package = Path("userland/native/system")
            rpc = json.loads((rpc_package / f"bootstrap_parent.{arch}.json").read_text())
            archive = (rpc_package / f"libbootstrap_parent.{arch}.a").read_bytes()
            self.assertEqual(sha(archive), rpc["archive_sha256"]); self.assertEqual(len(archive_objects(archive, cpu)), 2)
            self.assertEqual(rpc["message_id"], 406)
            self.assertEqual(set(rpc["exports"]), {"_bootstrap_parent", "_vproc_mig_parent"})
            for source, expected in rpc["original_sources"].items():
                self.assertEqual(sha(Path(source).read_bytes()), expected)

    def test_signed_dependency_graph_and_original_font_data(self):
        font_source = Path("src/Applications/xfce-prerequisites/dejavu-fonts/2.37")
        manifest = json.loads((font_source / "UPSTREAM.json").read_text())
        self.assertTrue(manifest["publisher_digest_verified"])
        for relative, expected in manifest["upstream_files_sha256"].items():
            self.assertEqual(sha((font_source / relative).read_bytes()), expected)
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads((PACKAGE / f"xfce-ui-runtime.{arch}.json").read_text())
            bundle = PACKAGE / f"xfce-ui-runtime.{arch}.tar"; files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            records = {**report["libraries"], **report["programs"], **report["modules"], "gio-helper": report["launch_desktop"]}
            images = {}
            for name, data in files.items():
                self.assertFalse(data.startswith(b"\x7fELF"), name)
                if data.startswith(b"\xcf\xfa\xed\xfe"):
                    self.assertEqual(struct.unpack_from("<I", data, 4)[0], cpu, name); verify_code_signature(data)
                    images["/" + name] = image_metadata(data)
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            previous = json.loads((PACKAGE / f"xfce-base-runtime.{arch}.json").read_text())
            for name, record in records.items():
                path = record["install_path"]
                self.assertEqual(sha(files[path.lstrip("/")]), record["sha256"], name)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"], name)
                for category in ("libraries", "programs", "modules"):
                    if name in previous[category]:
                        self.assertEqual(record["sha256"], previous[category][name]["sha256"])
            self.assertEqual(len(report["libraries"]), 88); self.assertEqual(len(report["programs"]), 16)
            for path, record in report["xfce_ui"]["desktop_font_data"].items():
                self.assertEqual(sha(files[path.lstrip("/")]), record["sha256"])
                self.assertEqual(sha(Path(record["source"]).read_bytes()), record["sha256"])
            self.assertIn("usr/share/fonts/dejavu/DejaVuSans.ttf", files)
            self.assertIn("usr/share/fonts/dejavu/DejaVuSansMono.ttf", files)
            self.assertIn("etc/fonts/conf.d/60-latin.conf", files)
            self.assertFalse(report["desktop_checked"])

if __name__ == "__main__":
    unittest.main()
