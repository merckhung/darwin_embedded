#!/usr/bin/env python3
"""Verify original LLVM source bytes and native C++ archive inventories."""
import hashlib
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0, "userland/apps/xfce")
from check_wm import tar_files, archive_objects
from check_clients import verify_code_signature
from link_auth import image_metadata,verify_closure
import struct

def sha(data):
    return hashlib.sha256(data).hexdigest()

class NativeCxx(unittest.TestCase):
    def test_signed_runtime_and_actual_dependency_scopes(self):
        package=Path("userland/apps/cxx")
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            bundle=package/f"cxx-runtime.{arch}.tar";report=json.loads((package/f"cxx-runtime.{arch}.json").read_text());files=tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()),report["bundle_sha256"]);images={}
            self.assertEqual(set(report["libraries"]),{"libunwind","libc++abi","libc++","test-module"})
            for path,data in files.items():
                self.assertFalse(data.startswith(b"\x7fELF"),path)
                if data.startswith(b"\xcf\xfa\xed\xfe"):
                    self.assertEqual(struct.unpack_from("<I",data,4)[0],cpu);verify_code_signature(data);images["/"+path]=image_metadata(data)
            loader=image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"]|{"dyld_stub_binder"}
            for record in report["libraries"].values():
                path=record["install_path"];self.assertEqual(sha(files[path.lstrip("/")]),record["sha256"])
                self.assertEqual(verify_closure(path,images,loader),record["startup_closure"])
            # The exception definition must have one provider across libraries.
            owners=[name for name,image in images.items() if "__ZTISt13runtime_error" in image["exports"]]
            self.assertEqual(owners,["/usr/lib/libc++abi.1.dylib"])
            damaged=bytearray(files["usr/lib/libunwind.1.dylib"]);damaged[-1]^=1
            with self.assertRaises(ValueError):verify_code_signature(damaged)

    def test_original_int128_builtins(self):
        source=Path("src/Applications/cxx/compiler-rt-builtins/20.1.8")
        manifest=json.loads((source/"UPSTREAM.json").read_text())
        for name,expected in manifest["upstream_files_sha256"].items():
            data=(source/name).read_bytes();self.assertEqual(sha(data),expected)
            self.assertEqual(hashlib.sha1(b"blob "+str(len(data)).encode()+b"\0"+data).hexdigest(),manifest["upstream_git_blob_sha1"][name])
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            package=Path("userland/apps/cxx");report=json.loads((package/f"compiler-rt.{arch}.json").read_text());data=(package/f"libcompiler_rt.{arch}.a").read_bytes()
            self.assertEqual(sha(data),report["archive_sha256"])
            self.assertEqual(len(report["sources"]),4)
            self.assertEqual(archive_objects(data,cpu),[r["member"] for r in report["sources"]])
            self.assertTrue({"___divti3","___udivti3","___udivmodti4"}<=set(report["exports"]))
            with self.assertRaises(ValueError):archive_objects(data,0x1000007 if arch=="arm64" else 0x100000c)

    def test_original_sources_and_native_archive_closures(self):
        for project in ("libcxx", "libcxxabi", "libunwind", "llvm-libc-shared"):
            source = Path("src/Applications/cxx") / project / "20.1.8"
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            actual = {str(p.relative_to(source)):sha(p.read_bytes()) for p in source.rglob("*") if p.is_file() and p.name != "UPSTREAM.json"}
            self.assertEqual(actual, manifest["upstream_files_sha256"])
        package = Path("userland/apps/cxx")
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            report=json.loads((package/f"cxx.{arch}.json").read_text());bundle=package/f"cxx.{arch}.tar";files=tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()),report["bundle_sha256"])
            for path,data in files.items():self.assertFalse(data.startswith(b"\x7fELF"),path)
            self.assertEqual({name:len(r["sources"]) for name,r in report["libraries"].items()}, {"libunwind":9,"libc++abi":17,"libc++":54})
            exports,imports=set(),set()
            for name,record in report["libraries"].items():
                data=files[f"usr/lib/{name}.a"]
                self.assertEqual(sha(data),record["archive_sha256"])
                self.assertEqual(archive_objects(data,cpu),[r["member"] for r in record["sources"]])
                self.assertFalse(set(record["imports"]) & set(record["definitions"]))
                missing=set(record["exports"])-set(record["definitions"])
                self.assertFalse(missing,"export has no definition: "+repr(sorted(missing)[:10]))
                for item in record["sources"]:self.assertEqual(sha(Path(item["source"]).read_bytes()),item["source_sha256"])
                with self.assertRaises(ValueError):archive_objects(data,0x1000007 if arch=="arm64" else 0x100000c)
                exports.update(record["exports"]);imports.update(record["imports"])
            self.assertEqual(set((package/f"cxx.{arch}.imports").read_text().splitlines()),imports-exports-{"___dso_handle"})
            self.assertFalse(report["runtime_checked"])

if __name__ == "__main__":unittest.main()
