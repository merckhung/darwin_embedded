#!/usr/bin/env python3
"""Audit publisher sources, native image APIs and every actual signed binding."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path("userland/apps/x11").resolve()))
from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path("userland/apps/fonts").resolve()))
from build_pango import meson_list
from build_pixbuf import VERSION, LOADERS, loader_sources


def sha(data):
    return hashlib.sha256(data).hexdigest()


def target_headers(arch, files):
    # Header APIs differ on Windows. Ask the real Darwin preprocessor to
    # select declarations instead of dropping names from a missing-API list.
    with tempfile.TemporaryDirectory(prefix="pixbuf-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as archive:
            archive.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/"):
                destination = root / "libraries" / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        return subprocess.check_output(["clang-20", f"--target={arch}-apple-macos26.5", "-E", "-P", "-x", "c", "-",
            "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-DGDK_PIXBUF_ENABLE_BACKEND", "-isystem", str(root / "sdk/usr/include"),
            "-isystem", resource + "/include", "-I", str(root / "libraries/usr/include"),
            "-I", str(root / "libraries/usr/include/glib-2.0"), "-I", str(root / "libraries/usr/include/gdk-pixbuf-2.0")],
            input="#include <gdk-pixbuf/gdk-pixbuf.h>\n#include <gdk-pixbuf/gdk-pixdata.h>\n", text=True)


class PixbufArtifacts(unittest.TestCase):
    def test_exact_publisher_release_bytes(self):
        source = Path("src/Applications/gtk/gdk-pixbuf") / VERSION
        upstream = json.loads((source / "UPSTREAM.json").read_text())
        self.assertTrue(upstream["publisher_digest_verified"])
        for name, expected in upstream["upstream_files_sha256"].items():
            self.assertEqual(sha((source / name).read_bytes()), expected, name)

    def test_original_inventory_native_objects_and_public_header_apis(self):
        source = Path("src/Applications/gtk/gdk-pixbuf") / VERSION
        recipe = (source / "gdk-pixbuf/meson.build").read_text()
        expected = {str(source / "gdk-pixbuf" / name) for name in
                    meson_list(recipe, "gdkpixbuf_sources") + meson_list(recipe, "gdkpixdata_sources")}
        expected |= {str(source / "gdk-pixbuf" / name) for names in loader_sources(recipe).values() for name in names}
        expected.add(str(source / "gdk-pixbuf/pixops/pixops.c"))
        expected |= {"generated/gdk-pixbuf-enum-types.c", "generated/gdk-pixbuf-marshal.c"}
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/pixbuf.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/pixbuf.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            record = report["libraries"]["libgdk_pixbuf-2.0"]
            self.assertEqual({entry["source"] for entry in record["sources"]}, expected)
            archive = files["usr/lib/libgdk_pixbuf-2.0.a"]
            self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
            for entry in record["sources"]:
                actual = report["generated_sources"][Path(entry["source"]).name] if entry["source"].startswith("generated/") else sha(Path(entry["source"]).read_bytes())
                self.assertEqual(actual, entry["source_sha256"])
            headers = target_headers(arch, files)
            declarations = set(re.findall(r"\b(gdk_pix(?:buf|data)_\w+)\s*\(", headers))
            self.assertGreater(len(declarations), 100)
            declarations |= {"gdk_pixbuf_" + name + "_version" for name in ("major", "minor", "micro")}
            declarations.add("gdk_pixbuf_version")
            self.assertEqual({"_" + name for name in declarations} - set(record["exports"]), set())
            self.assertEqual(report["builtin_loaders"], list(LOADERS))
            self.assertEqual(report["configuration"]["OS_DARWIN"], 1)
            self.assertEqual(report["configuration"]["USE_GMODULE"], 1)
            self.assertNotIn("OS_LINUX", report["configuration"])
            self.assertTrue(all(report["sdk_features"].values()))
            with self.assertRaises(ValueError):
                archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError):
                archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)

    def test_complete_signed_stack_and_actual_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/gtk/pixbuf-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/gtk/pixbuf-runtime.{arch}.json").read_text())
            compiled = json.loads(Path(f"userland/apps/gtk/pixbuf.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 61)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                if name in compiled["libraries"]:
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(compiled["libraries"][name]["exports"]))
            record = report["launch_desktop"]
            path = record["install_path"]
            data = files[path.lstrip("/")]
            self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 2))
            self.assertGreater(verify_code_signature(data), 0)
            images[path] = image_metadata(data)
            self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
            record = report["libraries"]["libgdk_pixbuf-2.0"]
            self.assertEqual(record["version_metadata"]["compatibility"], "4401.0.0")
            self.assertEqual(record["version_metadata"]["current"], "4401.8.0")
            path = record["install_path"]
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged = bytearray(files[path.lstrip("/")])
            description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
            struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__":
    unittest.main()
