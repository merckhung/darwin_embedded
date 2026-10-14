#!/usr/bin/env python3
"""Check unchanged Fontconfig sources, native objects and real dylib bindings."""
import hashlib
import json
import re
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path("userland/apps/x11").resolve()))
from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports


def sha(data):
    return hashlib.sha256(data).hexdigest()


class FontconfigArtifacts(unittest.TestCase):
    def test_exact_sources_native_cache_abi_and_objects(self):
        directory = Path("src/Applications/fonts/fontconfig/2.18.3")
        upstream = json.loads((directory / "UPSTREAM.json").read_text())
        self.assertTrue(upstream["publisher_digest_verified"])
        for name, expected in upstream["upstream_files_sha256"].items():
            name = upstream["packaging_renames"].get(name, name)
            self.assertEqual(sha((directory / name).read_bytes()), expected)
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/fonts/fontconfig.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/fonts/fontconfig.{arch}.json").read_text())
            archive = files["usr/lib/libfontconfig.a"]
            self.assertEqual(len(report["sources"]), 30)
            declarations = set(re.findall(r"^FcPublic\s+[^;]+?\b(Fc\w+)\s*\(",
                files["usr/include/fontconfig/fontconfig.h"].decode(), re.M))
            self.assertGreater(len(declarations), 200)
            self.assertLessEqual({"_" + name for name in declarations}, set(report["exports"]))
            self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in report["sources"]])
            for entry in report["sources"]:
                self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
            self.assertTrue(report["sdk_features"]["Darwin_LP64"])
            self.assertTrue(report["sdk_features"]["HAVE_MMAP"])
            self.assertTrue(report["sdk_features"]["stdatomic-primitives"])
            self.assertEqual(report["configuration"]["FONTCONFIG_PATH"], '"/etc/fonts"')
            self.assertIn(b"/usr/share/fonts", files["etc/fonts/fonts.conf"])
            self.assertNotIn(b"@CONFIGDIR@", files["etc/fonts/fonts.conf"])
            self.assertEqual(files["usr/share/licenses/fontconfig/COPYING"], (directory / "COPYING").read_bytes())
            with self.assertRaises(ValueError):
                archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError):
                archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)

    def test_every_signed_public_api_and_actual_import(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/fonts/fontconfig-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/fonts/fontconfig-runtime.{arch}.json").read_text())
            compiled = json.loads(Path(f"userland/apps/fonts/fontconfig.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 10)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for record in report["libraries"].values():
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
            record = report["libraries"]["libfontconfig"]
            self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(compiled["exports"]))
            path = record["install_path"]
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged = bytearray(files[path.lstrip("/")])
            description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
            struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__":
    unittest.main()
