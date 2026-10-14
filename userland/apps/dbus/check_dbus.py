#!/usr/bin/env python3
"""Audit original D-Bus inventories, native public APIs and signed programs."""
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
from build_dbus import VERSION, inventories, programs


def sha(data):
    return hashlib.sha256(data).hexdigest()


def public_api(arch, files):
    with tempfile.TemporaryDirectory(prefix="dbus-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as archive:
            archive.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/dbus-1.0/"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        text = subprocess.check_output(["clang-20", f"--target={arch}-apple-macos26.5", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-x", "c", "-", "-nostdinc",
            "-D_DARWIN_C_SOURCE=1", "-isystem", str(root / "sdk/usr/include"), "-isystem", resource + "/include",
            "-I", str(root / "usr/include/dbus-1.0")], input="#include <dbus/dbus.h>\n", text=True)
        declarations = set()
        def visit(node):
            if node.get("kind") == "FunctionDecl" and node.get("name", "").startswith("dbus_") and node.get("storageClass") != "static":
                declarations.add(node["name"])
            for child in node.get("inner", []): visit(child)
        visit(json.loads(text))
        return declarations


class DBusArtifacts(unittest.TestCase):
    def test_exact_publisher_release_bytes(self):
        source = Path("src/Applications/ipc/dbus") / VERSION
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        self.assertTrue(manifest["publisher_digest_verified"])
        self.assertEqual(len(manifest["upstream_files_sha256"]), 628)
        for name, expected in manifest["upstream_files_sha256"].items():
            self.assertEqual(sha((source / name).read_bytes()), expected, name)

    def test_complete_native_inventories_target_headers_and_unix_features(self):
        source = Path("src/Applications/ipc/dbus") / VERSION
        expected = inventories(source)
        expected.update({"program-" + name: paths for name, paths in programs(source).items()})
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/dbus/libraries.{arch}.tar")
            files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/dbus/libraries.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            records = {**report["libraries"], **report["internal_archives"]}
            self.assertEqual(set(records), set(expected))
            self.assertEqual(sum(len(value["sources"]) for value in records.values()), 105)
            for name, record in records.items():
                self.assertEqual({entry["source"] for entry in record["sources"]}, {str(path) for path in expected[name]})
                for entry in record["sources"]:
                    self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                    if entry["source"].endswith("/dbus-sysdeps-unix.c"):
                        self.assertIn(entry["source"], report["native_source_adaptations"])
                        self.assertNotEqual(entry["compiled_source_sha256"], entry["source_sha256"])
                    else:
                        self.assertEqual(entry["compiled_source_sha256"], entry["source_sha256"])
                archive = files[record["archive_path"]]
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            declared = public_api(arch, files)
            self.assertGreater(len(declared), 200)
            self.assertEqual({"_" + name for name in declared} - set(records["libdbus-1"]["exports"]), set())
            for name in ("DBUS_UNIX", "ENABLE_TRADITIONAL_ACTIVATION", "DBUS_ENABLE_ASSERT", "DBUS_ENABLE_CHECKS", "HAVE_UNIX_FD_PASSING", "HAVE_POLL", "HAVE_GETPEEREID"):
                self.assertEqual(report["configuration"][name], 1)
            self.assertNotIn("HAVE_MONOTONIC_CLOCK", report["configuration"])
            self.assertFalse(report["sdk_features"]["HAVE_MONOTONIC_CLOCK"])
            self.assertTrue(report["sdk_features"]["NATIVE_KQUEUE"])
            self.assertEqual(report["architecture_configuration"]["DBUS_INT64_TYPE"], "long")
            self.assertEqual(report["architecture_configuration"]["DBUS_SIZEOF_VOID_P"], 8)
            self.assertNotIn("DBUS_HAVE_LINUX_EPOLL", report["configuration"])
            self.assertNotIn("DBUS_WIN", report["configuration"])
            self.assertIn(b"<auth>EXTERNAL</auth>", files["usr/share/dbus-1/session.conf"])

    def test_complete_signed_stack_daemons_helpers_and_real_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/dbus/runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/apps/dbus/runtime.{arch}.json").read_text())
            build = json.loads(Path(f"userland/apps/dbus/libraries.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 72)
            self.assertEqual(set(report["programs"]), set(build["programs"]))
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                path = record["install_path"]; data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                if name == "libdbus-1":
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(build["libraries"][name]["exports"]))
            for name, record in {**report["programs"], "gio-launch-desktop": report["launch_desktop"]}.items():
                path = record["install_path"]; data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 2))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(sha(data), record["sha256"])
                images[path] = image_metadata(data)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])
            path = report["libraries"]["libdbus-1"]["install_path"]
            imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged = bytearray(files[path.lstrip("/")])
            description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
            struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
            with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                verify_imports(path, {**images, path: image_metadata(damaged)}, loader)


if __name__ == "__main__": unittest.main()
