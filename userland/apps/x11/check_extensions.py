#!/usr/bin/env python3
"""Audit native desktop extension APIs, publisher sources and signed bindings."""
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

from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports
from build_extensions import RELEASES, LIBRARIES, make_inventory


def sha(data):
    return hashlib.sha256(data).hexdigest()


def target_api(arch, files, headers):
    with tempfile.TemporaryDirectory(prefix="extension-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as archive:
            archive.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/"):
                destination = root / "libraries" / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
        include = root / "libraries/usr/include"
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        text = subprocess.check_output(["clang-20", f"--target={arch}-apple-macos26.5", "-E", "-x", "c", "-",
            "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-isystem", str(root / "sdk/usr/include"),
            "-isystem", resource + "/include", "-I", str(include)],
            input="".join(f"#include <{header}>\n" for header in headers), text=True)
        # Keep declarations from this library's actual target-selected public
        # headers. Transitively included Xlib APIs belong to the base library.
        selected = {str(include / header) for header in headers}
        lines, current = [], None
        for line in text.splitlines():
            marker = re.match(r'^#\s+\d+\s+"([^\"]+)"', line)
            if marker:
                current = marker[1]
            elif current in selected:
                lines.append(line)
        return set(re.findall(r"\b(X[A-Za-z0-9_]+)\s*\(", "\n".join(lines)))


class ExtensionArtifacts(unittest.TestCase):
    def test_exact_publisher_source_bytes(self):
        for name, release in RELEASES.items():
            source = Path("src/Applications/desktop-x11") / name / release
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            self.assertTrue(manifest["publisher_digest_verified"])
            self.assertTrue(manifest["digest_url"].startswith("https://lists.x.org/archives/xorg-announce/"))
            for path, expected in manifest["upstream_files_sha256"].items():
                self.assertEqual(sha((source / path).read_bytes()), expected, str(source / path))

    def test_original_complete_inventories_and_native_public_apis(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/x11/extensions.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/x11/extensions.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(set(report["libraries"]), set(LIBRARIES.values()))
            self.assertEqual(sum(len(record["sources"]) for record in report["libraries"].values()), 85)
            for name, release in RELEASES.items():
                source = Path("src/Applications/desktop-x11") / name / release
                library = LIBRARIES[name]
                record = report["libraries"][library]
                expected = {str(source / "src" / path) for path in make_inventory(source / "src/Makefile.am", library + "_la_SOURCES")}
                self.assertEqual({entry["source"] for entry in record["sources"]}, expected)
                for entry in record["sources"]:
                    self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                archive = files["usr/lib/" + library + ".a"]
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                headers = [str(path.relative_to(source / "include")) for path in (source / "include").rglob("*.h")]
                if name == "libXcursor":
                    headers.append("X11/Xcursor/Xcursor.h")
                declared = target_api(arch, files, headers)
                self.assertGreater(len(declared), 3, library)
                self.assertEqual({"_" + value for value in declared} - set(record["exports"]), set(), library)
                with self.assertRaises(ValueError):
                    archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError):
                    archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            header = files["usr/include/X11/Xcursor/Xcursor.h"]
            for name, value in zip(("MAJOR", "MINOR", "REVISION"), (1, 2, 3)):
                self.assertIn(f"#define XCURSOR_LIB_{name} {value}".encode(), header)
            self.assertEqual(sha(header), report["configured_headers"]["usr/include/X11/Xcursor/Xcursor.h"])
            self.assertEqual(report["libraries"]["libXcursor"]["configuration"]["HAVE_XFIXES"], 1)

    def test_complete_signed_stack_and_actual_provider_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/x11/extensions-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/x11/extensions-runtime.{arch}.json").read_text())
            compiled = json.loads(Path(f"userland/apps/x11/extensions.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 71)
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
            for library in LIBRARIES.values():
                path = report["libraries"][library]["install_path"]
                imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                damaged = bytearray(files[path.lstrip("/")])
                description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
                struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
                with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                    verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__":
    unittest.main()
