#!/usr/bin/env python3
"""Audit original sources, Mach-O HarfBuzz APIs and actual Darwin bindings."""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0, str(Path("userland/apps/x11").resolve()))
from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports
from build_harfbuzz import HEADERS, LIBRARIES, inventory


def sha(data):
    return hashlib.sha256(data).hexdigest()


class HarfBuzzArtifacts(unittest.TestCase):
    def test_unchanged_release_sources_and_pinned_headers(self):
        for directory in (Path("src/Applications/fonts/harfbuzz/14.6.0"), Path("src/Toolchains/libcxx/20.1.8")):
            upstream = json.loads((directory / "UPSTREAM.json").read_text())
            self.assertTrue(upstream["publisher_digest_verified"])
            for name, expected in upstream["upstream_files_sha256"].items():
                path = upstream.get("packaging_renames", {}).get(name, name)
                self.assertEqual(sha((directory / path).read_bytes()), expected, str(directory / path))

    def test_native_inventories_and_independent_public_headers(self):
        source = Path("src/Applications/fonts/harfbuzz/14.6.0")
        recipe = (source / "src/meson.build").read_text()
        experimental = set(re.search(r'"""(hb_shape_justify.*?)"""\.splitlines',
                                     (source / "src/gen-def.py").read_text(), re.S)[1].splitlines())
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/fonts/harfbuzz.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/fonts/harfbuzz.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(set(report["libraries"]), set(LIBRARIES))
            self.assertEqual(sum(len(entry["sources"]) for entry in report["libraries"].values()), 99)
            self.assertTrue(report["sdk_features"]["Darwin_LP64_libcxx"])
            for feature in ("HAVE_MMAP", "HAVE_MPROTECT", "HAVE_PTHREAD", "HAVE_FREETYPE", "HAVE_GLIB", "HAVE_GOBJECT", "HAVE_SUBSET"):
                self.assertEqual(report["configuration"][feature], 1)
            self.assertNotIn("HB_NO_MT", report["configuration"])
            self.assertIn(b"#define HB_HAS_SUBSET 1", files["usr/include/harfbuzz/hb-features.h"])
            self.assertIn(b"#define HB_HAS_CORETEXT 0", files["usr/include/harfbuzz/hb-features.h"])
            for name, record in report["libraries"].items():
                archive = files["usr/lib/" + name + ".a"]
                expected = {str(source / "src" / path) for variable in LIBRARIES[name]
                            for path in inventory(recipe, variable) if path.endswith(".cc")}
                if name == "libharfbuzz-gobject": expected.add("generated/hb-gobject-enums.cc")
                self.assertEqual({entry["source"] for entry in record["sources"]}, expected)
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                for entry in record["sources"]:
                    if entry["source"].startswith("generated/"):
                        self.assertEqual(entry["source_sha256"], report["generated_sources"][Path(entry["source"]).name])
                    else:
                        self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                headers = [files["usr/include/harfbuzz/" + path].decode() for variable in HEADERS[name]
                           for path in inventory(recipe, variable) if path.endswith(".h")]
                if name == "libharfbuzz": headers.append(files["usr/include/harfbuzz/hb-version.h"].decode())
                if name == "libharfbuzz-gobject": headers.append(files["usr/include/harfbuzz/hb-gobject-enums.h"].decode())
                declared = set(re.findall(r"^hb_\w+(?= \()", "\n".join(headers), re.M))
                # Experimental declarations are upstream guarded APIs, absent
                # from this stable configuration and from its original .def.
                exported = set(record["exports"])
                stable = set(record["header_api"])
                self.assertGreater(len(declared), 0)
                self.assertEqual(stable, declared - experimental)
                self.assertLessEqual({"_" + entry for entry in stable}, exported)
                self.assertFalse(any("__cxa_" in entry or entry.startswith("__Z") for entry in record["imports"]))
                with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)

    def test_complete_signed_dylibs_and_actual_provider_ordinals(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/fonts/harfbuzz-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/fonts/harfbuzz-runtime.{arch}.json").read_text())
            compiled = json.loads(Path(f"userland/apps/fonts/harfbuzz.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 18)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for record in report["libraries"].values():
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                self.assertFalse(any("libc++" in dependency or "libstdc++" in dependency or "linux" in dependency
                                     for _, dependency in images[path]["dependencies"]))
            for name in LIBRARIES:
                record = report["libraries"][name]
                self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(compiled["libraries"][name]["exports"]))
                self.assertEqual(record["version_metadata"]["current"], "61460.0.0")
                path = record["install_path"]
                imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                damaged = bytearray(files[path.lstrip("/")])
                description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
                struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
                with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                    verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__":
    unittest.main()
