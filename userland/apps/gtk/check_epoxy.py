#!/usr/bin/env python3
"""Audit the original native GL/GLX dispatch and its complete runtime stack."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path("userland/apps/x11").resolve()))
from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports

VERSION = "1.5.10"


def sha(data): return hashlib.sha256(data).hexdigest()


class EpoxyArtifacts(unittest.TestCase):
    def test_original_publisher_source_bytes(self):
        source = Path("src/Applications/widgets/libepoxy") / VERSION
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        self.assertTrue(manifest["publisher_digest_verified"])
        self.assertEqual(len(manifest["upstream_files_sha256"]), 66)
        for name, expected in manifest["upstream_files_sha256"].items():
            self.assertEqual(sha((source / name).read_bytes()), expected, name)
        recipe = (source / "src/meson.build").read_text()
        for name in ("gl_generated_dispatch.c", "dispatch_common.c", "glx_generated_dispatch.c", "dispatch_glx.c"):
            self.assertIn("'" + name + "'", recipe)

    def test_native_dispatch_inventory_public_variables_and_functions(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/epoxy.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/epoxy.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            record = report["libraries"]["libepoxy"]
            self.assertEqual(len(record["sources"]), 4)
            for entry in record["sources"]:
                if not entry["source"].startswith("generated/"):
                    self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
            archive = files["usr/lib/libepoxy.a"]
            self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
            with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            with tempfile.TemporaryDirectory(prefix="epoxy-header-audit-") as directory:
                root = Path(directory)
                with tarfile.open(f"userland/native/sdk.{arch}.tar") as sdk: sdk.extractall(root / "sdk", filter="data")
                for name, data in files.items():
                    if name.startswith("usr/include/"):
                        path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
                resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
                ast = subprocess.check_output(["clang-20", f"--target={arch}-apple-macos26.5", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-x", "c", "-",
                    "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-isystem", str(root / "sdk/usr/include"), "-isystem", resource + "/include", "-I", str(root / "usr/include")],
                    input="#include <epoxy/gl.h>\n#include <epoxy/glx.h>\n", text=True)
                declared = {"_" + node["name"] for node in json.loads(ast)["inner"]
                            if node.get("kind") in ("FunctionDecl", "VarDecl") and node.get("name", "").startswith("epoxy_") and node.get("storageClass") != "static"}
                self.assertGreater(len(declared), 2000)
                self.assertEqual(declared - set(record["exports"]), set())
            self.assertFalse(report["opengl_provider_supplied"])
            self.assertEqual(report["configuration"]["ENABLE_GLX"], 1)
            self.assertEqual(report["configuration"]["ENABLE_EGL"], 0)
            for name, expected in report["generated_headers"].items(): self.assertEqual(sha(files[name]), expected)

    def test_signed_complete_stack_and_retained_native_programs(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/epoxy-runtime.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/epoxy-runtime.{arch}.json").read_text())
            previous = tar_files(Path(f"userland/apps/gtk/accessibility-runtime.{arch}.tar"))
            build = json.loads(Path(f"userland/apps/gtk/epoxy.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(report["libraries"]), 77)
            self.assertEqual(len(report["programs"]), 12)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in {**report["libraries"], **report["programs"], "gio-launch-desktop": report["launch_desktop"]}.items():
                path = record["install_path"]; data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6 if name in report["libraries"] else 2))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                images[path] = image_metadata(data)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])
                if name in report["programs"]: self.assertEqual(data, previous[path.lstrip("/")])
            record = report["libraries"]["libepoxy"]; path = record["install_path"]
            self.assertEqual(path, "/usr/lib/libepoxy.0.dylib")
            self.assertEqual(record["version_metadata"]["compatibility"], "1.0.0")
            self.assertEqual(record["version_metadata"]["current"], "1.0.0")
            self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(build["libraries"]["libepoxy"]["exports"]))
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged = bytearray(files[path.lstrip("/")])
            description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
            struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__": unittest.main()
