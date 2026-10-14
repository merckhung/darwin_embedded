#!/usr/bin/env python3
"""Audit native XKB compiler objects, signed programs and complete layout data."""
import hashlib
import json
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

from check_clients import archive_objects, tar_files, verify_code_signature
from link_auth import image_metadata, verify_imports

def sha(data): return hashlib.sha256(data).hexdigest()

class NativeXKBArtifacts(unittest.TestCase):
    def test_original_sources_and_symbol_aliases(self):
        for project, version in (("xkbcomp","1.5.0"),("xkeyboard-config","2.48")):
            source=Path("src/Applications/xserver") / project / version
            manifest=json.loads((source / "UPSTREAM.json").read_text())
            for name,expected in manifest["upstream_files_sha256"].items():
                self.assertEqual(sha((source / name).read_bytes()),expected,name)
            for name,target in manifest.get("upstream_symlinks",{}).items():
                # Bazel replaces input symlink targets; their pinned source
                # destinations must still expose exactly the original bytes.
                path=source / name
                self.assertEqual(path.read_bytes(),(path.parent / target).read_bytes(),name)

    def test_native_objects_signatures_bindings_and_retained_stack(self):
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            package=Path("userland/apps/x11")
            record=json.loads((package / f"xkb-runtime.{arch}.json").read_text())
            previous=json.loads((package / f"server-dependencies-runtime.{arch}.json").read_text())
            archive=(package / f"xkbcomp.{arch}.a").read_bytes()
            objects=archive_objects(archive,cpu)
            self.assertEqual(len(objects),20)
            self.assertEqual(len(record["xkb"]["objects"]),20)
            self.assertEqual(sha(archive),record["programs"]["xkbcomp"]["archive_sha256"])
            with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as temporary:
                subprocess.run(["llvm-ar-20","x",str((package / f"xkbcomp.{arch}.a").resolve())],cwd=temporary,check=True,capture_output=True)
                for item in record["xkb"]["objects"]:
                    self.assertIn(item["member"],objects)
                    self.assertEqual(sha((Path(temporary)/item["member"]).read_bytes()),item["object_sha256"])
                    self.assertEqual(sha(Path(item["source"]).read_bytes()),item["source_sha256"])
            with self.assertRaises(ValueError): archive_objects(archive,0x1000007 if cpu==0x100000c else 0x100000c)
            runtime=tar_files(package / f"xkb-runtime.{arch}.tar")
            baseline=tar_files(package / f"server-dependencies-runtime.{arch}.tar")
            self.assertEqual(len(record["libraries"]),82)
            self.assertEqual(len(record["programs"]),13)
            for item in previous["libraries"].values():
                path=item["install_path"].lstrip("/")
                self.assertEqual(runtime[path],baseline[path])
            for family in ("programs","modules"):
                for name,item in previous[family].items():
                    path=item["install_path"].lstrip("/")
                    self.assertEqual(runtime[path],baseline[path],name)
            images={"/"+name:image_metadata(data) for name,data in runtime.items() if name.endswith(".dylib")}
            loader=image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            path="/usr/bin/xkbcomp"; data=runtime[path.lstrip("/")]
            self.assertEqual(struct.unpack_from("<4I",data)[::3],(0xfeedfacf,2))
            self.assertEqual(struct.unpack_from("<I",data,4)[0],cpu)
            verify_code_signature(data)
            images[path]=image_metadata(data)
            self.assertEqual(sha(data),record["programs"]["xkbcomp"]["sha256"])
            self.assertEqual(verify_imports(path,images,loader,flat_namespace_root=path),record["programs"]["xkbcomp"]["import_resolutions"])
            for path,expected in record["xkb"]["installed_data_sha256"].items():
                self.assertEqual(sha(runtime[path.lstrip("/")]),expected,path)
            self.assertEqual(runtime["usr/share/X11/xkb/keycodes/nativehid"],Path("userland/apps/x11/data/keycodes/nativehid").read_bytes())
            self.assertEqual(runtime["usr/share/X11/xkb/rules/nativehid"],Path("userland/apps/x11/data/rules/nativehid").read_bytes())
            allocator=[item for item in record["xkb"]["objects"] if item["source"].endswith("/src/util/reallocarray.c")]
            self.assertEqual(len(allocator),1)
            self.assertEqual(record["xkb"]["configuration"]["HAVE_REALLOCARRAY"],1)
            self.assertFalse(record["runtime_checked"])
            self.assertFalse(record["desktop_checked"])

if __name__ == "__main__": unittest.main()
