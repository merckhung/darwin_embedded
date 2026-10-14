#!/usr/bin/env python3
"""Audit original libxml2 sources, native APIs and complete signed bindings."""
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
from build_xml import VERSION, FEATURES, source_inventory


def sha(data): return hashlib.sha256(data).hexdigest()


def public_api(arch, files, source):
    with tempfile.TemporaryDirectory(prefix="xml-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as archive:
            archive.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/libxml2/"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        recipe = (source / "include/libxml/meson.build").read_text()
        headers = re.findall(r"'([^']+\.h)'", re.search(r"libxml_headers = files\((.*?)\)", recipe, re.S)[1])
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        text = subprocess.check_output(["clang-20", f"--target={arch}-apple-macos26.5", "-fsyntax-only", "-Xclang", "-ast-dump=json",
            "-x", "c", "-", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-Wno-deprecated-declarations",
            "-isystem", str(root / "sdk/usr/include"), "-isystem", resource + "/include", "-I", str(root / "usr/include/libxml2")],
            input="".join(f"#include <libxml/{header}>\n" for header in headers), text=True)
        declared = set()
        def visit(node):
            if node.get("kind") == "FunctionDecl" and node.get("name", "").startswith(("xml", "html")) and node.get("storageClass") != "static":
                declared.add(node["name"])
            for child in node.get("inner", []): visit(child)
        visit(json.loads(text))
        return declared


class XMLArtifacts(unittest.TestCase):
    def test_original_publisher_source_bytes(self):
        source = Path("src/Applications/xml/libxml2") / VERSION
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        self.assertTrue(manifest["publisher_digest_verified"])
        self.assertEqual(len(manifest["upstream_files_sha256"]), 4426)
        for name, expected in manifest["upstream_files_sha256"].items():
            self.assertEqual(sha((source / name).read_bytes()), expected, name)

    def test_original_inventories_native_apis_and_target_features(self):
        source = Path("src/Applications/xml/libxml2") / VERSION
        expected = {str(source / name) for name in source_inventory((source / "meson.build").read_text())}
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/xml.{arch}.tar"); files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/xml.{arch}.json").read_text())
            record = report["libraries"]["libxml2"]
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(record["sources"]), 38)
            self.assertEqual({entry["source"] for entry in record["sources"]}, expected)
            for entry in record["sources"]:
                self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
            archive = files["usr/lib/libxml2.a"]
            self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
            declared = public_api(arch, files, source)
            self.assertGreater(len(declared), 800)
            self.assertEqual({"_" + name for name in declared} - set(record["exports"]), set())
            for name in FEATURES: self.assertEqual(report["feature_configuration"]["WITH_" + name.upper()], 1)
            for name in ("HTTP", "ICU", "LEGACY", "THREAD_ALLOC"): self.assertEqual(report["feature_configuration"]["WITH_" + name], 0)
            for name in ("HAVE_DLOPEN", "HAVE_PTHREAD_H", "HAVE_FUNC_ATTRIBUTE_DESTRUCTOR", "HAVE_DECL_MMAP", "Darwin_LP64"):
                self.assertTrue(report["sdk_features"][name])
            self.assertEqual(sha(files["usr/include/libxml2/libxml/xmlversion.h"]), report["generated_headers"]["usr/include/libxml2/libxml/xmlversion.h"])
            with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)

    def test_complete_signed_stack_actual_ordinals_and_darwin_versions(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/gtk/xml-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/gtk/xml-runtime.{arch}.json").read_text())
            build = json.loads(Path(f"userland/apps/gtk/xml.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 73)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                path = record["install_path"]; data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                if name == "libxml2":
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(build["libraries"][name]["exports"]))
                    self.assertEqual(record["install_path"], "/usr/lib/libxml2.16.dylib")
                    self.assertEqual(record["version_metadata"]["compatibility"], "18.0.0")
                    self.assertEqual(record["version_metadata"]["current"], "18.4.0")
            record = report["launch_desktop"]; path = record["install_path"]; data = files[path.lstrip("/")]
            self.assertGreater(verify_code_signature(data), 0)
            images[path] = image_metadata(data)
            self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
            path = report["libraries"]["libxml2"]["install_path"]
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged = bytearray(files[path.lstrip("/")]); description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
            struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__": unittest.main()
