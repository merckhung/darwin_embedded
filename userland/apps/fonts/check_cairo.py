#!/usr/bin/env python3
"""Check original Cairo/X11/compiler sources and actual native dylib bindings."""
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
from build_cairo import SELECTED, X11_RELEASES, list_inventory, feature_inventory


def sha(data):
    return hashlib.sha256(data).hexdigest()


class CairoArtifacts(unittest.TestCase):
    def test_unchanged_publisher_sources_and_compiler_builtins(self):
        directories = [Path("src/Applications/fonts/cairo/1.18.6"), Path("src/Toolchains/compiler-rt/20.1.8")]
        directories += [Path("src/Applications/x11") / name / release for name, release in X11_RELEASES.items()]
        for directory in directories:
            upstream = json.loads((directory / "UPSTREAM.json").read_text())
            self.assertTrue(upstream["publisher_digest_verified"])
            for name, expected in upstream["upstream_files_sha256"].items():
                name = upstream.get("packaging_renames", {}).get(name, name)
                self.assertEqual(sha((directory / name).read_bytes()), expected, str(directory / name))

    def test_native_selected_inventories_headers_threads_and_wide_math(self):
        source = Path("src/Applications/fonts/cairo/1.18.6")
        recipe = (source / "src/meson.build").read_text()
        expected = list_inventory(recipe, "cairo_sources")
        headers = list_inventory(recipe, "cairo_headers")
        additional = feature_inventory(recipe, "cairo_feature_sources")
        additional_headers = feature_inventory(recipe, "cairo_feature_headers")
        for feature in SELECTED:
            expected += additional.get(feature, []); headers += additional_headers.get(feature, [])
        inventories = {"libcairo": {str(source / "src" / path) for path in expected} |
                       {"src/Toolchains/compiler-rt/20.1.8/lib/builtins/" + name for name in ("udivti3.c", "udivmodti4.c")}}
        public_headers = {"libcairo": headers}
        for name, subdir, variable in (("libcairo-gobject", "cairo-gobject", "cairo_gobject"),
                                      ("libcairo-script-interpreter", "cairo-script", "cairoscript_interpreter")):
            directory = source / "util" / subdir
            text = (directory / "meson.build").read_text()
            inventories[name] = {str(directory / path) for path in list_inventory(text, variable + "_sources")}
            public_headers[name] = list_inventory(text, variable + "_headers")
        for name, release in X11_RELEASES.items():
            directory = Path("src/Applications/x11") / name / release
            recipe = (directory / "src/Makefile.am").read_text().replace("\\\n", " ")
            files = re.search(r"^" + name + r"_la_SOURCES\s*=\s*(.*)$", recipe, re.M)[1].split()
            inventories[name] = {str(directory / "src" / path) for path in files if path.endswith(".c")}
            if name == "libXext": inventories[name].add(str(directory / "src/reallocarray.c"))
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/fonts/cairo.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/fonts/cairo.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(set(report["libraries"]), set(inventories))
            self.assertEqual(sum(len(record["sources"]) for record in report["libraries"].values()), 188)
            for name, record in report["libraries"].items():
                archive = files["usr/lib/" + name + ".a"]
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                self.assertEqual({entry["source"] for entry in record["sources"]}, inventories[name])
                for entry in record["sources"]:
                    self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                if name in public_headers:
                    text = "\n".join(files["usr/include/cairo/" + header].decode() for header in public_headers[name])
                    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
                    declared = set(re.findall(r"^cairo_public\s+[^;]+?\b(cairo_\w+)\s*\(", text, re.M))
                    self.assertGreaterEqual(len(declared), 10)
                    self.assertEqual({"_" + entry for entry in declared} - set(record["exports"]), set(), name)
                with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            configuration = report["configuration"]
            for key in ("CAIRO_HAS_PTHREAD", "CAIRO_HAS_REAL_PTHREAD", "HAVE_C11_ATOMIC_PRIMITIVES", "HAVE___UINT128_T", "HAVE_MMAP"):
                self.assertEqual(configuration[key], 1)
            self.assertEqual(configuration["HAVE_MKDIR"], 2)
            self.assertEqual(configuration["IPC_RMID_DEFERRED_RELEASE"], 0)
            self.assertNotIn("___udivti3", report["system_requests"])
            for key in ("CAIRO_HAS_XLIB_SURFACE", "CAIRO_HAS_XCB_SURFACE", "CAIRO_HAS_XLIB_XRENDER_SURFACE",
                        "CAIRO_HAS_FT_FONT", "CAIRO_HAS_FC_FONT", "CAIRO_HAS_PNG_FUNCTIONS", "CAIRO_HAS_PDF_SURFACE", "CAIRO_HAS_SVG_SURFACE"):
                self.assertEqual(report["feature_configuration"][key], 1)
            self.assertNotIn("CAIRO_HAS_QUARTZ_SURFACE", report["feature_configuration"])
            self.assertIn(b"/usr/share/fonts", files["etc/fonts/fonts.conf"])

    def test_complete_signed_stack_and_actual_ordinals(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/fonts/cairo-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/fonts/cairo-runtime.{arch}.json").read_text())
            compiled = json.loads(Path(f"userland/apps/fonts/cairo.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 53)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                self.assertFalse(any("linux" in dependency or "libc++" in dependency or "libstdc++" in dependency
                                     for _, dependency in images[path]["dependencies"]))
                if name in compiled["libraries"]:
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(compiled["libraries"][name]["exports"]))
            for name in ("libcairo", "libcairo-gobject", "libcairo-script-interpreter"):
                record = report["libraries"][name]
                self.assertEqual(record["version_metadata"]["compatibility"], "1.0.0")
                self.assertEqual(record["version_metadata"]["current"], "1.18.6")
                path = record["install_path"]
                imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                damaged = bytearray(files[path.lstrip("/")])
                description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
                struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
                with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                    verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__":
    unittest.main()
