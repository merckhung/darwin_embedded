#!/usr/bin/env python3
"""Audit real upstream bytes, native source members, signatures and bindings."""
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


class NativeGraphics(unittest.TestCase):
    def test_exact_upstream_bytes(self):
        manifests = sorted(Path("src/Applications/graphics").glob("*/*/UPSTREAM.json"))
        self.assertEqual(len(manifests), 5)
        for path in manifests:
            manifest = json.loads(path.read_text())
            mapping = {destination: source for source, destination in manifest.get("packaging_renames", {}).items()}
            actual = {mapping.get(str(source.relative_to(path.parent)), str(source.relative_to(path.parent))): sha(source.read_bytes())
                      for source in path.parent.rglob("*") if source.is_file() and source != path}
            self.assertEqual(actual, manifest["upstream_files_sha256"], path)

    def test_complete_native_source_members(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            report = json.loads(Path(f"userland/apps/graphics/libraries.{arch}.json").read_text())
            bundle = Path(f"userland/apps/graphics/libraries.{arch}.tar")
            files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            actual = {name: archive_objects(files["usr/lib/" + name + ".a"], cpu)
                      for name in report["components"]}
            self.assertEqual(sum(len(members) for members in actual.values()), 111)
            for name, record in report["components"].items():
                self.assertEqual(actual[name], [source["member"] for source in record["sources"]])
                self.assertEqual(sha(files["usr/lib/" + name + ".a"]), record["archive_sha256"])
                for source in record["sources"]:
                    self.assertEqual(sha(Path(source["source"]).read_bytes()), source["source_sha256"])
            self.assertEqual(Path(f"userland/apps/graphics/libraries.{arch}.imports").read_text().splitlines(),
                             report["native_system_imports"])
            self.assertEqual(report["linker_supplied_imports"], ["___dso_handle"])

    def test_headers_match_both_native_ports(self):
        arm = tar_files(Path("userland/apps/graphics/libraries.arm64.headers.tar"))
        pc = tar_files(Path("userland/apps/graphics/libraries.x86_64.headers.tar"))
        self.assertEqual(arm, pc)
        self.assertTrue({"usr/include/zlib.h", "usr/include/expat.h", "usr/include/png.h",
                         "usr/include/freetype2/ft2build.h", "usr/include/pixman-1/pixman.h"} <= arm.keys())

    def test_every_signed_dylib_and_actual_binding(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/graphics/runtime.{arch}.tar")
            report = json.loads(Path(f"userland/apps/graphics/runtime.{arch}.json").read_text())
            files = tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(report["libraries"]), 5)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                path = record["install_path"]
                data = files[path[1:]]
                magic, actual_cpu, _, kind = struct.unpack_from("<4I", data)
                self.assertEqual((magic, actual_cpu, kind), (0xfeedfacf, cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(set(record["exports"]), images[path]["exports"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                changed = bytearray(data)
                changed[32 + struct.unpack_from("<I", data, 20)[0] + 16] ^= 1
                with self.assertRaisesRegex(ValueError, "code page hash mismatch"):
                    verify_code_signature(changed)
                imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                changed = bytearray(data)
                position = imported["nlist_offset"] + 6
                description = struct.unpack_from("<H", changed, position)[0]
                struct.pack_into("<H", changed, position, (description & 0xff) | (0xfd << 8))
                with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                    verify_imports(path, {**images, path: image_metadata(changed)}, loader)

    def test_foreign_archive_members_rejected(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/graphics/libraries.{arch}.tar"))
            for name, data in files.items():
                if not name.endswith(".a"):
                    continue
                with self.assertRaises(ValueError):
                    archive_objects(data, 0x1000007 if cpu == 0x100000c else 0x100000c)
                changed = data.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1)
                self.assertNotEqual(data, changed)
                with self.assertRaises(ValueError):
                    archive_objects(changed, cpu)

    def test_component_flat_imports_use_actual_loaded_namespace(self):
        for arch in ("arm64", "x86_64"):
            files = tar_files(Path(f"userland/apps/graphics/runtime.{arch}.tar"))
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            component = "/usr/lib/system/libsystem_native_rng.dylib"
            root = "/usr/lib/libpng16.16.dylib"
            with self.assertRaisesRegex(ValueError, "no provider"):
                verify_imports(component, images, loader)
            actual = verify_imports(component, images, loader, flat_namespace_root=root)
            self.assertTrue(any(entry["symbol"] == "_getentropy" and entry["provider"] ==
                                "/usr/lib/system/libsystem_native_kernel.dylib" for entry in actual))
            # Keep the real provider installed but remove the actual parent's
            # load dependency. It must not become a global-export-union pass.
            isolated = dict(images)
            isolated[root] = {**images[root], "dependencies": []}
            with self.assertRaisesRegex(ValueError, "no provider"):
                verify_imports(component, isolated, loader, flat_namespace_root=root)


if __name__ == "__main__":
    unittest.main()
