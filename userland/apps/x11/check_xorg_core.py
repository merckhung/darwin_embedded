#!/usr/bin/env python3
"""Audit the real native Xorg core while explicitly requiring an unfinished DDX."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

from check_clients import archive_objects, tar_files


def sha(data): return hashlib.sha256(data).hexdigest()


class XorgCoreArtifacts(unittest.TestCase):
    def test_original_sources_native_objects_and_private_darwin_port(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/x11/xorg-core.{arch}.tar"); files = tar_files(bundle)
            report = json.loads(Path(f"userland/apps/x11/xorg-core.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            self.assertEqual(len(report["sources"]), 281)
            for entry in report["sources"]:
                self.assertEqual(sha(Path(entry["source"]).read_bytes()), entry["source_sha256"])
                if entry["source"].endswith("/os/client.c"):
                    self.assertNotEqual(entry["source_sha256"], entry["compiled_source_sha256"])
                else: self.assertEqual(entry["source_sha256"], entry["compiled_source_sha256"])
            archive = files["usr/libexec/xorg/core.a"]
            self.assertEqual(archive_objects(archive, cpu), [entry["member"] for entry in report["sources"]])
            with self.assertRaises(ValueError): archive_objects(archive, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError): archive_objects(archive.replace(bytes.fromhex("cffaedfe"), b"\x7fELF", 1), cpu)

    def test_extension_inventories_real_runtime_imports_and_pending_ddx(self):
        required_extensions = {"_CompositeExtensionInit", "_DamageExtensionInit", "_DbeExtensionInit", "_RRExtensionInit", "_RenderExtensionInit",
                               "_XFixesExtensionInit", "_XTestExtensionInit", "_ShmExtensionInit", "_ResExtensionInit", "_XInputExtensionInit",
                               "_SecurityExtensionInit", "_ScreenSaverExtensionInit", "_RecordExtensionInit", "_PanoramiXExtensionInit"}
        required_ddx = {"_InitOutput", "_InitInput", "_CloseInput", "_ProcessInputEvents", "_ddxGiveUp", "_ddxProcessArgument"}
        for arch in ("arm64", "x86_64"):
            report = json.loads(Path(f"userland/apps/x11/xorg-core.{arch}.json").read_text())
            self.assertEqual(required_extensions - set(report["definitions"]), set())
            self.assertTrue(required_ddx <= set(report["unresolved"]))
            self.assertTrue({"_g_checksum_new", "_g_checksum_update", "_g_checksum_get_digest", "_g_checksum_free", "_sysctl", "_pthread_once"} <= set(report["unresolved"]))
            self.assertNotIn("_dispatch_once", report["unresolved"])
            self.assertNotIn("__NSConcreteGlobalBlock", report["unresolved"])
            self.assertFalse(report["ddx_linked"]); self.assertFalse(report["runtime_checked"])
            for option in ("CONFIG_UDEV", "CONFIG_HAL", "SYSTEMD_LOGIND", "WITH_LIBDRM", "GLAMOR", "GLXEXT", "DRI2", "DRI3"):
                self.assertNotIn(option, report["configuration"])
            for option in ("HAVE_LINUX_AGPGART_H", "HAVE_EPOLL_CREATE1", "HAVE_MEMFD_CREATE", "HAVE_SYS_SYSMACROS_H"):
                self.assertFalse(report["sdk_features"][option])
            self.assertIn("shm.c", report["inventories"]["Xext"])
            self.assertIn("xres.c", report["inventories"]["Xext"])
            self.assertIn("security.c", report["inventories"]["Xext"])
            self.assertIn("rrxinerama.c", report["inventories"]["randr"])


if __name__ == "__main__": unittest.main()
