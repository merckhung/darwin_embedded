#!/usr/bin/env python3
"""Audit original X server dependencies, native APIs and the retained full stack."""
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
from build_server_dependencies import RELEASES, inventories, replacement_sources


def sha(data): return hashlib.sha256(data).hexdigest()


def public_api(arch, files, headers):
    with tempfile.TemporaryDirectory(prefix="xserver-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as sdk: sdk.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/"):
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        include = root / "usr/include"; resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        flags = ["clang-20", f"--target={arch}-apple-macos26.5", "-x", "c", "-", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-isystem", str(root / "sdk/usr/include"), "-isystem", resource + "/include", "-I", str(include)]
        translation_unit = '#include <stdio.h>\n#include <X11/Xlib.h>\n#include <X11/XKBlib.h>\n#include <X11/fonts/fontstruct.h>\n' + "".join(f"#include <{h}>\n" for h in headers)
        preprocessed = subprocess.check_output([*flags, "-E"], input=translation_unit, text=True)
        selected = {str(include / header) for header in headers}; current, lines = None, []
        for line in preprocessed.splitlines():
            marker = re.match(r'^#\s+\d+\s+"([^\"]+)"', line)
            if marker: current = marker[1]
            elif current in selected: lines.append(line)
        names = set(re.findall(r"\b((?:FontEnc|FontMap|xfont2_|Xkb|Xkm)[A-Za-z0-9_]*)\s*\(", "\n".join(lines)))
        ast = json.loads(subprocess.check_output([*flags, "-fsyntax-only", "-Xclang", "-ast-dump=json"], input=translation_unit, text=True))
        external = {node.get("name") for node in ast["inner"] if node.get("kind") == "FunctionDecl" and node.get("storageClass") != "static"}
        return {"_" + name for name in names & external}


class ServerDependencyArtifacts(unittest.TestCase):
    def test_original_publisher_sources_including_current_server(self):
        counts = {"libfontenc": 33, "libXfont2": 123, "libxkbfile": 28, "xorg-server": 1661}
        for name, version in {**RELEASES, "xorg-server": "21.1.25"}.items():
            source = Path("src/Applications/xserver") / name / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            self.assertTrue(manifest["publisher_digest_verified"])
            self.assertEqual(len(manifest["upstream_files_sha256"]), counts[name])
            for path, expected in manifest["upstream_files_sha256"].items():
                self.assertEqual(sha((source / path).read_bytes()), expected, str(source / path))

    def test_original_inventories_native_headers_and_allocator_replacements(self):
        base = Path("src/Applications/xserver"); inventory = inventories(base)
        headers = {"libfontenc": ["X11/fonts/fontenc.h"], "libXfont2": ["X11/fonts/libxfont2.h"],
                   "libxkbfile": ["X11/extensions/" + h for h in ("XKM.h", "XKBrules.h", "XKBbells.h", "XKBconfig.h", "XKMformat.h", "XKBfile.h")]}
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/x11/server_dependencies.{arch}.tar"); files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/x11/server_dependencies.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            for name, record in report["libraries"].items():
                source = base / name / RELEASES[name]
                replacements = replacement_sources(name, report["sdk_features"][name])
                self.assertEqual(record["original_replacement_sources"], replacements)
                expected = [str(source / path) for path in inventory[name] + replacements]
                self.assertEqual([entry["source"] for entry in record["sources"]], expected)
                for entry in record["sources"]: self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                archive = files["usr/lib/" + name + ".a"]
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                api = public_api(arch, files, headers[name]); self.assertGreater(len(api), 8)
                self.assertEqual(api - set(record["exports"]), set())
                with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            self.assertIn("_XkbCFDflts", report["libraries"]["libxkbfile"]["exports"])
            for name in ("XFONT_FREETYPE", "XFONT_BUILTINS", "XFONT_BITMAP", "XFONT_BDFFORMAT", "XFONT_PCFFORMAT", "X_GZIP_FONT_COMPRESSION"):
                self.assertEqual(report["configuration"]["libXfont2"][name], 1)
            for name in ("XFONT_SNFFORMAT", "XFONT_FC", "X_BZIP2_FONT_COMPRESSION"):
                self.assertNotIn(name, report["configuration"]["libXfont2"])

    def test_full_signed_stack_retains_services_and_real_gtk_module(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/x11/server-dependencies-runtime.{arch}.tar"); files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/x11/server-dependencies-runtime.{arch}.json").read_text())
            build = json.loads(Path(f"userland/apps/x11/server_dependencies.{arch}.json").read_text())
            previous = tar_files(Path(f"userland/apps/gtk/gtk-runtime.{arch}.tar"))
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(report["libraries"]), 82); self.assertEqual(len(report["programs"]), 12)
            self.assertEqual(set(report["modules"]), {"printbackend-file"})
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in {**report["libraries"], **report["programs"], **report["modules"], "gio-launch-desktop": report["launch_desktop"]}.items():
                path = record["install_path"]; data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6 if name in report["libraries"] else 8 if name in report["modules"] else 2))
                self.assertGreater(verify_code_signature(data), 0); self.assertEqual(sha(data), record["sha256"])
                images[path] = image_metadata(data)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])
                if name in report["programs"] or name in report["modules"]: self.assertEqual(data, previous[path.lstrip("/")])
                if name in build["libraries"]:
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(build["libraries"][name]["exports"]))
                    imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                    damaged = bytearray(data); description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
                    struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
                    with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                        verify_imports(path, {**images, path: image_metadata(damaged)}, loader)
            module = report["modules"]["printbackend-file"]; path = module["install_path"]
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged = bytearray(files[path.lstrip("/")]); description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
            struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(damaged)}, loader, flat_namespace_root=path)


if __name__ == "__main__": unittest.main()
