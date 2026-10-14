#!/usr/bin/env python3
"""Audit native accessibility source bytes, public APIs, programs and bindings."""
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
from build_accessibility import VERSION, HEADER_ONLY_DECLARATIONS, source_inventory


def sha(data): return hashlib.sha256(data).hexdigest()


def public_api(arch, files):
    with tempfile.TemporaryDirectory(prefix="accessibility-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as archive: archive.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        flags = ["clang-20", f"--target={arch}-apple-macos26.5", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-x", "c", "-", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-Wno-deprecated-declarations", "-isystem", str(root / "sdk/usr/include"), "-isystem", resource + "/include"]
        for include in ("", "glib-2.0", "dbus-1.0", "atk-1.0", "at-spi-2.0", "at-spi2-atk/2.0"):
            flags += ["-I", str(root / "usr/include" / include)]
        ast = subprocess.check_output(flags, input="#include <atk/atk.h>\n#include <atspi/atspi.h>\n#include <atk-bridge.h>\n", text=True)
        declared = {"libatk-1.0": set(), "libatspi": set(), "libatk-bridge-2.0": set()}
        def visit(node):
            if node.get("kind") == "FunctionDecl" and node.get("storageClass") != "static":
                name = node.get("name", "")
                library = "libatk-bridge-2.0" if name.startswith("atk_bridge_") else "libatk-1.0" if name.startswith("atk_") else "libatspi" if name.startswith("atspi_") else None
                if library: declared[library].add("_" + name)
            for child in node.get("inner", []): visit(child)
        visit(json.loads(ast))
        return declared


class AccessibilityArtifacts(unittest.TestCase):
    def test_original_publisher_source_bytes(self):
        source = Path("src/Applications/accessibility/at-spi2-core") / VERSION
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        self.assertTrue(manifest["publisher_digest_verified"])
        self.assertEqual(len(manifest["upstream_files_sha256"]), 500)
        for name, expected in manifest["upstream_files_sha256"].items():
            self.assertEqual(sha((source / name).read_bytes()), expected, name)

    def test_original_inventories_target_apis_and_real_features(self):
        source = Path("src/Applications/accessibility/at-spi2-core") / VERSION
        inventories = source_inventory(source)
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/accessibility.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/accessibility.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            declared = public_api(arch, files)
            self.assertGreater(len(declared["libatk-1.0"]), 250)
            self.assertGreater(len(declared["libatspi"]), 250)
            self.assertEqual(len(declared["libatk-bridge-2.0"]), 3)
            for name, record in {**report["libraries"], **report["programs"]}.items():
                expected = [path if path.startswith("generated/") else str(source / path) for path in inventories[name]]
                self.assertEqual([entry["source"] for entry in record["sources"]], expected)
                for entry in record["sources"]:
                    if not entry["source"].startswith("generated/"):
                        self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                path = "usr/lib/" + name + ".a" if name.startswith("lib") else "usr/libexec/accessibility/" + name + ".a"
                archive = files[path]
                self.assertEqual(sha(archive), record["archive_sha256"])
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                if name in declared:
                    self.assertEqual(declared[name] - set(record["exports"]),
                                     {"_" + symbol for symbol in HEADER_ONLY_DECLARATIONS.get(name, [])})
                with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            self.assertNotIn("HAVE_DBUS_GLIB", report["configuration"])
            self.assertEqual(report["upstream_header_only_declarations"], HEADER_ONLY_DECLARATIONS)
            for name in ("HAVE_X11", "HAVE_XKB", "HAVE_XRES", "HAVE_DLOPEN", "HAVE_BIND_TEXTDOMAIN_CODESET"):
                self.assertTrue(report["sdk_features"][name])
            for name, expected in report["generated_headers"].items(): self.assertEqual(sha(files[name]), expected)
            self.assertEqual(len(report["introspection_inputs"]), 21)
            self.assertIn(b"unix:dir=/tmp", files["usr/share/defaults/at-spi2/accessibility.conf"])
            self.assertIn(b"/usr/libexec/at-spi2-registryd", files["usr/share/dbus-1/accessibility-services/org.a11y.atspi.Registry.service"])

    def test_signed_stack_retains_dbus_and_resolves_actual_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/accessibility-runtime.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/accessibility-runtime.{arch}.json").read_text())
            build = json.loads(Path(f"userland/apps/gtk/accessibility.{arch}.json").read_text())
            dbus = tar_files(Path(f"userland/apps/dbus/runtime.{arch}.tar"))
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(report["libraries"]), 76)
            self.assertEqual(len(report["programs"]), 12)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in {**report["libraries"], **report["programs"], "gio-launch-desktop": report["launch_desktop"]}.items():
                path = record["install_path"]
                data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6 if name in report["libraries"] else 2))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                images[path] = image_metadata(data)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])
                if name in build["libraries"]:
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(build["libraries"][name]["exports"]))
                    expected = ("26211.0.0", "26211.1.0") if name == "libatk-1.0" else ("0.0.0", "0.0.0")
                    self.assertEqual((record["version_metadata"]["compatibility"], record["version_metadata"]["current"]), expected)
                if "retained_from_runtime_sha256" in record: self.assertEqual(data, dbus[path.lstrip("/")])
                if name in build["libraries"] or name in build["programs"]:
                    imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                    damaged = bytearray(data)
                    description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
                    struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
                    with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                        verify_imports(path, {**images, path: image_metadata(damaged)}, loader, flat_namespace_root=path)


if __name__ == "__main__": unittest.main()
