#!/usr/bin/env python3
"""Audit original text sources, generated data, public APIs and native bindings."""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0,str(Path("userland/apps/x11").resolve()))
from check_clients import archive_objects,tar_files,verify_code_signature
from link_auth import image_metadata,verify_imports
from build_pango import TEXT_RELEASES,meson_list


def sha(data):return hashlib.sha256(data).hexdigest()


class PangoArtifacts(unittest.TestCase):
    def test_exact_upstream_release_bytes(self):
        for name,release in TEXT_RELEASES.items():
            directory=Path("src/Applications/text")/name/release
            manifest=json.loads((directory/"UPSTREAM.json").read_text())
            self.assertTrue(manifest["publisher_digest_verified"])
            for path,expected in manifest["upstream_files_sha256"].items():
                actual=directory/manifest.get("packaging_renames",{}).get(path,path)
                self.assertEqual(sha(actual.read_bytes()),expected,str(actual))

    def test_native_public_apis_and_fresh_unicode_dictionary_data(self):
        base=Path("src/Applications/text");pango=base/"pango/1.58.2"
        recipe=(pango/"pango/meson.build").read_text()
        groups={"libpango-1.0":meson_list(recipe,"pango_headers"),
                "libpangoft2-1.0":sum((meson_list(recipe,name) for name in ("pangofc_headers","pangoot_headers","pangoft2_headers")),[]),
                "libpangocairo-1.0":meson_list(recipe,"pangocairo_headers")}
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            bundle=Path(f"userland/apps/fonts/pango.{arch}.tar");files=tar_files(bundle)
            report=json.loads(Path(f"userland/apps/fonts/pango.{arch}.json").read_text())
            generated=tar_files(Path(f"userland/apps/fonts/fribidi-generated.{arch}.tar"))
            self.assertEqual(sha(bundle.read_bytes()),report["bundle_sha256"])
            self.assertEqual(sum(len(record["sources"]) for record in report["libraries"].values()),92)
            self.assertEqual(report["fribidi_unicode_version"],"18.0.0")
            self.assertIn(b'#define FRIBIDI_UNICODE_VERSION "18.0.0"',files["usr/include/fribidi/fribidi-unicode-version.h"])
            self.assertEqual(files["usr/include/fribidi/fribidi-unicode-version.h"],generated["fribidi-unicode-version.h"])
            for name,expected in report["generated_fribidi_tables"].items():self.assertEqual(sha(generated[name]),expected)
            self.assertNotEqual(report["generated_fribidi_tables"]["bidi-type.tab.i"],report["original_fribidi_tables"]["bidi-type.tab.i"])
            self.assertEqual(report["dictionary_words"],25379)
            self.assertEqual(sha(files["usr/share/libthai/thbrk.tri"]),report["dictionary_sha256"])
            self.assertGreater(len(files["usr/share/libthai/thbrk.tri"]),100000)
            self.assertTrue(report["sdk_features"]["Darwin_LP64"])
            for name,record in report["libraries"].items():
                archive=files["usr/lib/"+name+".a"]
                self.assertEqual(archive_objects(archive,cpu),[entry["member"] for entry in record["sources"]])
                for entry in record["sources"]:
                    if entry["source"].startswith("generated/"):
                        self.assertEqual(entry["source_sha256"],report["generated_sources"][Path(entry["source"]).name])
                    else:self.assertEqual(sha(Path(entry["source"]).read_bytes()),entry["source_sha256"])
                    self.assertEqual(entry["source_sha256"],entry["compiled_source_sha256"])
                if name in groups:
                    headers=list(groups[name])
                    if name=="libpango-1.0":headers.append("pango-enum-types.h")
                    text="\n".join(files["usr/include/pango-1.0/pango/"+header].decode() for header in headers)
                    text=re.sub(r"/\*.*?\*/","",text,flags=re.S)
                    declared=set(re.findall(r"\b(pango_\w+)\s*\(",text))
                    self.assertGreater(len(declared),20)
                    self.assertEqual({"_"+name for name in declared}-set(record["exports"]),set())
                with self.assertRaises(ValueError):archive_objects(archive,0x1000007 if cpu==0x100000c else 0x100000c)
                with self.assertRaises(ValueError):archive_objects(archive.replace(bytes.fromhex("cffaedfe"),b"\x7fELF",1),cpu)

    def test_signed_complete_stack_and_gio_helper_bindings(self):
        for arch,cpu in (("arm64",0x100000c),("x86_64",0x1000007)):
            files=tar_files(Path(f"userland/apps/fonts/pango-runtime.{arch}.tar"))
            report=json.loads(Path(f"userland/apps/fonts/pango-runtime.{arch}.json").read_text())
            compiled=json.loads(Path(f"userland/apps/fonts/pango.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]),60)
            images={"/"+name:image_metadata(data) for name,data in files.items() if name.endswith(".dylib")}
            loader=image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"]|{"dyld_stub_binder"}
            for name,record in report["libraries"].items():
                path=record["install_path"];data=files[path.lstrip("/")]
                self.assertEqual(struct.unpack_from("<4I",data)[1::2],(cpu,6))
                self.assertGreater(verify_code_signature(data),0)
                self.assertEqual(sha(data),record["sha256"])
                self.assertEqual(verify_imports(path,images,loader),record["import_resolutions"])
                if name in compiled["libraries"]:self.assertEqual(set(record["exports"])-{"___dso_handle"},set(compiled["libraries"][name]["exports"]))
            record=report["launch_desktop"];path=record["install_path"];data=files[path.lstrip("/")]
            self.assertEqual(struct.unpack_from("<4I",data)[1::2],(cpu,2))
            self.assertGreater(verify_code_signature(data),0)
            images[path]=image_metadata(data)
            self.assertEqual(verify_imports(path,images,loader),record["import_resolutions"])
            path=report["libraries"]["libpango-1.0"]["install_path"]
            imported=next(entry for entry in images[path]["imports"] if not entry["weak"])
            damaged=bytearray(files[path.lstrip("/")]);description=struct.unpack_from("<H",damaged,imported["nlist_offset"]+6)[0]
            struct.pack_into("<H",damaged,imported["nlist_offset"]+6,(description&0xff)|(0xfd<<8))
            with self.assertRaisesRegex(ValueError,"dependency ordinal"):verify_imports(path,{**images,path:image_metadata(damaged)},loader)


if __name__=="__main__":unittest.main()
