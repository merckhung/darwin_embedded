#!/usr/bin/env python3
"""Audit the fully linked native Xorg program and its actual dependency bindings."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports

def sha(data):return hashlib.sha256(data).hexdigest()

class NativeXorgServer(unittest.TestCase):
    def test_program_cpu_signature_and_actual_complete_bindings(self):
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            package=Path("userland/apps/x11")
            report=json.loads((package/f"xorg-runtime.{arch}.json").read_text())
            files=tar_files(package/f"xorg-runtime.{arch}.tar")
            self.assertEqual(sha((package/f"xorg-runtime.{arch}.tar").read_bytes()),report["bundle_sha256"])
            archive=(package/f"xorg-server.{arch}.a").read_bytes()
            self.assertEqual(len(archive_objects(archive,cpu)),286)
            with self.assertRaises(ValueError):archive_objects(archive,0x1000007 if cpu==0x100000c else 0x100000c)
            program=report["programs"]["Xorg"];data=files[program["install_path"].lstrip("/")]
            self.assertEqual(sha(archive),program["archive_sha256"])
            self.assertEqual(sha(data),program["sha256"])
            self.assertEqual(struct.unpack_from("<4I",data)[1::2],(cpu,2))
            self.assertGreater(verify_code_signature(data),0)
            bad=bytearray(data);bad[-1]^=1
            with self.assertRaises(ValueError):verify_code_signature(bad)
            images={"/"+name:image_metadata(contents) for name,contents in files.items() if name.endswith(".dylib")}
            loader=image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"]|{"dyld_stub_binder"}
            all_records={**report["libraries"],**report["programs"],**report["modules"],"gio-helper":report["launch_desktop"]}
            for name,item in all_records.items():
                path=item["install_path"];images[path]=image_metadata(files[path.lstrip("/")])
            for name,item in all_records.items():
                path=item["install_path"]
                self.assertEqual(verify_imports(path,images,loader,flat_namespace_root=path),item["import_resolutions"],name)
            self.assertTrue(report["xorg"]["server_program_linked"])
            self.assertFalse(report["runtime_checked"])
            self.assertFalse(report["desktop_checked"])

    def test_actual_core_backend_and_original_ddx_bytes(self):
        for arch in ("arm64","x86_64"):
            package=Path("userland/apps/x11")
            report=json.loads((package/f"xorg-runtime.{arch}.json").read_text())["xorg"]
            for field,file in (("core_sha256",f"xorg-core.{arch}.tar"),("display_sha256",f"xorg-display.{arch}.o"),("input_sha256",f"xorg-input.{arch}.o")):
                self.assertEqual(sha((package/file).read_bytes()),report[field])
            for item in report["original_ddx_sources"]:
                self.assertEqual(sha(Path(item["source"]).read_bytes()),item["source_sha256"])

if __name__=="__main__":unittest.main()
