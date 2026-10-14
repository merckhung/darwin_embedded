#!/usr/bin/env python3
"""Audit native GTK3 X11 sources, public APIs, resource data and actual bindings."""
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
from build_gtk import VERSION, source_inventory


def sha(data): return hashlib.sha256(data).hexdigest()


def public_api(arch, files):
    with tempfile.TemporaryDirectory(prefix="gtk-header-audit-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/sdk.{arch}.tar") as sdk: sdk.extractall(root / "sdk", filter="data")
        for name, data in files.items():
            if name.startswith("usr/include/"):
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        flags = ["clang-20", f"--target={arch}-apple-macos26.5", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-x", "c", "-", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-Wno-deprecated-declarations", "-isystem", str(root / "sdk/usr/include"), "-isystem", resource + "/include"]
        for include in ("", "glib-2.0", "gtk-3.0", "gtk-3.0/unix-print", "atk-1.0", "pango-1.0", "cairo", "gdk-pixbuf-2.0", "freetype2", "harfbuzz"):
            flags += ["-I", str(root / "usr/include" / include)]
        ast = subprocess.check_output(flags, input="#include <gtk/gtk.h>\n#include <gtk/gtk-a11y.h>\n#include <gdk/gdkx.h>\n#include <gtk/gtkunixprint.h>\n", text=True)
        declared = {"libgtk-3": set(), "libgdk-3": set()}
        def visit(node):
            if node.get("kind") == "FunctionDecl" and node.get("storageClass") != "static":
                name = node.get("name", "")
                if name.startswith("gtk_"): declared["libgtk-3"].add("_" + name)
                # GdkPixbuf's two historical enum APIs also use gdk_, and
                # belong to the image library included by these headers.
                if name.startswith("gdk_") and not name.startswith(("gdk_pixbuf_", "gdk_pixdata_")) and name not in ("gdk_colorspace_get_type", "gdk_interp_type_get_type"):
                    declared["libgdk-3"].add("_" + name)
            for child in node.get("inner", []): visit(child)
        visit(json.loads(ast)); return declared


class GTKArtifacts(unittest.TestCase):
    def test_original_publisher_source_bytes(self):
        source = Path("src/Applications/widgets/gtk") / VERSION
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        self.assertTrue(manifest["publisher_digest_verified"])
        self.assertEqual(len(manifest["upstream_files_sha256"]), 4832)
        for name, expected in manifest["upstream_files_sha256"].items():
            self.assertEqual(sha((source / name).read_bytes()), expected, name)

    def test_original_x11_unix_inventories_target_apis_and_resources(self):
        source = Path("src/Applications/widgets/gtk") / VERSION
        inventories = source_inventory(source)
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/gtk.{arch}.tar"); files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/gtk.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            declared = public_api(arch, files)
            self.assertGreater(len(declared["libgtk-3"]), 3000)
            self.assertGreater(len(declared["libgdk-3"]), 500)
            for name, record in report["libraries"].items():
                expected = [path if path.startswith("generated/") else str(source / path) for path in inventories[name]]
                self.assertEqual([entry["source"] for entry in record["sources"]], expected)
                for entry in record["sources"]:
                    if not entry["source"].startswith("generated/"):
                        self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                    if entry["compiler_options"]:
                        self.assertIn(Path(entry["source"]).name, ("gdkscreen-x11.c", "gdkcairo.c"))
                        self.assertEqual(entry["compiler_options"], ["-Wno-error=incompatible-pointer-types-discards-qualifiers"])
                archive = files["usr/lib/" + name + ".a"]
                self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in record["sources"]])
                self.assertEqual(declared[name] - set(record["exports"]), set())
                with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
                with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)
            self.assertTrue(report["x11_backend"]); self.assertFalse(report["quartz_backend"]); self.assertFalse(report["wayland_backend"])
            for name in ("HAVE_LINUX_MEMFD_H", "HAVE_LINUX_INPUT_H", "HAVE_SYS_SYSINFO_H", "HAVE_GETRESUID", "HAVE_MALLINFO"):
                self.assertFalse(report["sdk_features"][name])
            for name in ("HAVE_XKB", "XINPUT_2", "XINPUT_2_2", "XINPUT_2_4", "HAVE_RANDR15", "HAVE_XCURSOR", "HAVE_BIND_TEXTDOMAIN_CODESET", "HAVE_MMAP"):
                self.assertTrue(report["sdk_features"][name])
            for name, expected in report["generated_headers"].items(): self.assertEqual(sha(files[name]), expected)
            for name, record in report["installed_source_headers"].items():
                self.assertEqual(sha(files[name]), record["sha256"])
                self.assertEqual(sha(Path(record["source"]).read_bytes()), record["sha256"])
            self.assertEqual(report["configuration"]["GTK_PRINT_BACKENDS"], '"file"')
            entry = report["modules"]["printbackend-file"]; archive = files["usr/libexec/gtk/printbackend-file.a"]
            self.assertEqual(archive_objects(archive, cpu), ["gtkprintbackendfile.o"])
            self.assertEqual(sha(archive), entry["archive_sha256"])
            self.assertEqual(sha(Path(entry["sources"][0]["source"]).read_bytes()), entry["sources"][0]["source_sha256"])
            self.assertIn(b"GVariant", files["usr/share/glib-2.0/schemas/gschemas.compiled"])
            for name in ("FileChooser", "ColorChooser", "EmojiChooser", "Debug"):
                self.assertIn("usr/share/glib-2.0/schemas/org.gtk.Settings." + name + ".gschema.xml", files)

    def test_complete_signed_stack_services_and_actual_native_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/gtk/gtk-runtime.{arch}.tar"); files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/gtk/gtk-runtime.{arch}.json").read_text())
            build = json.loads(Path(f"userland/apps/gtk/gtk.{arch}.json").read_text())
            previous = tar_files(Path(f"userland/apps/gtk/epoxy-runtime.{arch}.tar"))
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(report["libraries"]), 79); self.assertEqual(len(report["programs"]), 12)
            self.assertTrue({"_gdk_colorspace_get_type", "_gdk_interp_type_get_type"} <= set(report["libraries"]["libgdk_pixbuf-2.0"]["exports"]))
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in {**report["libraries"], **report["programs"], "gio-launch-desktop": report["launch_desktop"]}.items():
                path = record["install_path"]; data = files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 6 if name in report["libraries"] else 2))
                self.assertGreater(verify_code_signature(data), 0); self.assertEqual(sha(data), record["sha256"])
                images[path] = image_metadata(data)
                self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])
                if name in report["programs"]: self.assertEqual(data, previous[path.lstrip("/")])
                if name in build["libraries"]:
                    self.assertEqual(set(record["exports"]) - {"___dso_handle"}, set(build["libraries"][name]["exports"]))
                    self.assertEqual(record["version_metadata"]["compatibility"], "2421.0.0")
                    self.assertEqual(record["version_metadata"]["current"], "2421.32.0")
                    imported = next(entry for entry in images[path]["imports"] if not entry["weak"])
                    damaged = bytearray(data); description = struct.unpack_from("<H", damaged, imported["nlist_offset"] + 6)[0]
                    struct.pack_into("<H", damaged, imported["nlist_offset"] + 6, (description & 0xff) | (0xfd << 8))
                    with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                        verify_imports(path, {**images, path: image_metadata(damaged)}, loader)
            self.assertEqual(set(report["modules"]), {"printbackend-file"})
            record = report["modules"]["printbackend-file"]; path = record["install_path"]; data = files[path.lstrip("/")]
            self.assertEqual(struct.unpack_from("<4I", data)[1::2], (cpu, 8))
            self.assertGreater(verify_code_signature(data), 0); self.assertEqual(sha(data), record["sha256"])
            images[path] = image_metadata(data)
            self.assertTrue({"_pb_module_init", "_pb_module_exit", "_pb_module_create"} <= images[path]["exports"])
            self.assertEqual(verify_imports(path, images, loader, flat_namespace_root=path), record["import_resolutions"])


if __name__ == "__main__": unittest.main()
