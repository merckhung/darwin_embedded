#!/usr/bin/env python3
"""Independent native archive, upstream byte, code-signature and binding audits."""
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


class NativeDependencies(unittest.TestCase):
    def test_upstream_inventory(self):
        manifests = sorted(Path("src/Applications/glib").glob("*/*/UPSTREAM.json"))
        self.assertEqual(len(manifests), 5)
        for path in manifests:
            manifest = json.loads(path.read_text())
            renames = {new: old for old, new in manifest["packaging_renames"].items()}
            links = manifest.get("upstream_symlinks", {})
            actual = {renames.get(str(item.relative_to(path.parent)), str(item.relative_to(path.parent))): sha(item.read_bytes())
                      for item in path.parent.rglob("*") if item.is_file() and item != path and
                      str(item.relative_to(path.parent)) not in links}
            self.assertEqual(actual, manifest["upstream_files_sha256"], path)
            for name, target in links.items():
                self.assertEqual((path.parent / name).read_bytes(), (path.parent / name).parent.joinpath(target).read_bytes())
            self.assertEqual(manifest["local_changes"], [])
            self.assertEqual(len(manifest["archive_sha256"]), 64)

    def test_native_source_archives(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/glib/dependencies.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/glib/dependencies.{arch}.json").read_text())
            self.assertEqual(report["bundle_sha256"], sha(bundle.read_bytes()))
            expected = {"libpcre2-8": 31, "libffi": 8 if arch == "arm64" else 10, "libiconv": 3, "libintl": 34}
            self.assertEqual(set(report["components"]), set(expected))
            for name, count in expected.items():
                record = report["components"][name]
                members = archive_objects(files["usr/lib/" + name + ".a"], cpu)
                self.assertEqual(len(members), count)
                self.assertEqual(members, [entry["member"] for entry in record["sources"]])
                self.assertEqual(record["archive_sha256"], sha(files["usr/lib/" + name + ".a"]))
                for source in record["sources"]:
                    self.assertEqual(source["source_sha256"], sha(Path(source["source"]).read_bytes()))
            self.assertEqual(report["imports"], Path(f"userland/apps/glib/dependencies.{arch}.imports").read_text().splitlines())
            self.assertIn("#define FFI_VERSION_NUMBER 30800", files["usr/include/ffi.h"].decode())
            self.assertTrue(report["feature_checks"])
            self.assertFalse(any(name.startswith("__pcre2_") for name in report["components"]["libpcre2-8"]["exports"]))
            self.assertFalse(any(name.startswith("__libintl_") for name in report["components"]["libintl"]["exports"]))
            self.assertTrue({"_ffi_call", "_ffi_prep_cif_var", "_ffi_closure_alloc", "_ffi_prep_closure_loc"}
                            <= set(report["components"]["libffi"]["exports"]))
            for name, expected_hash in report["installed_headers_sha256"].items():
                self.assertEqual(sha(files["usr/include/" + name]), expected_hash)

    def test_complete_signed_native_libraries(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads(Path(f"userland/apps/glib/runtime.{arch}.json").read_text())
            archives = json.loads(Path(f"userland/apps/glib/dependencies.{arch}.json").read_text())
            bundle = Path(f"userland/apps/glib/runtime.{arch}.tar")
            files = tar_files(bundle)
            self.assertEqual(report["bundle_sha256"], sha(bundle.read_bytes()))
            self.assertEqual(report["archives_sha256"], archives["bundle_sha256"])
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())
            providers = loader["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                data = files[record["install_path"].lstrip("/")]
                self.assertEqual(struct.unpack_from("<I", data, 4)[0], cpu)
                self.assertEqual(struct.unpack_from("<I", data, 12)[0], 6)
                self.assertEqual(sha(data), record["sha256"])
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(archives["components"][name]["exports"]))
                self.assertEqual(verify_imports(record["install_path"], images, providers), record["import_resolutions"])
            self.assertFalse(report["runtime_checked"])


if __name__ == "__main__":
    unittest.main()
