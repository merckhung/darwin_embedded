#!/usr/bin/env python3
"""Audit full original desktop source inventories, modules and startup closures."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import unittest
from check_wm import tar_files, archive_objects, verify_code_signature, image_metadata, verify_imports
from link_auth import verify_closure

PACKAGE=Path("userland/apps/xfce")
def sha(data):return hashlib.sha256(data).hexdigest()
class NativeDesktop(unittest.TestCase):
    def test_original_sources_and_native_inventories(self):
        for base,project,version in [('xfce','xfce4-panel','4.20.8'),('xfce','xfce4-session','4.20.4'),('xfce','xfdesktop','4.20.2'),('xfce-prerequisites','iceauth','1.0.11'),('xfce-prerequisites','libyaml','0.2.5')]:
            source=Path('src/Applications')/base/project/version;manifest=json.loads((source/'UPSTREAM.json').read_text())
            actual={str(f.relative_to(source)):sha(f.read_bytes()) for f in source.rglob('*') if f.is_file() and f.name!='UPSTREAM.json'}
            self.assertEqual(actual,manifest['upstream_files_sha256'])
        for arch,cpu in [('arm64',0x100000c),('x86_64',0x1000007)]:
            report=json.loads((PACKAGE/f'xfce-desktop.{arch}.json').read_text());bundle=PACKAGE/f'xfce-desktop.{arch}.tar';files=tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()),report['bundle_sha256']);count=0
            generated_hashes={sha(data) for path,data in files.items() if path.startswith('build-metadata/generated/')}
            self.assertEqual(len(report['programs']),10);self.assertEqual(len(report['modules']),11);self.assertEqual(len(report['libraries']),2)
            for category in ['libraries','programs','modules']:
                for name,record in report[category].items():
                    data=files[record['archive_path']];self.assertEqual(sha(data),record['archive_sha256']);count+=len(record['sources'])
                    self.assertEqual(archive_objects(data,cpu),[item['member'] for item in record['sources']])
                    for item in record['sources']:
                        if item['generated']:
                            self.assertIn(item['source_sha256'],generated_hashes)
                        else:self.assertEqual(sha(Path(item['source']).read_bytes()),item['source_sha256'])
                    with self.assertRaises(ValueError):archive_objects(data,0x1000007 if arch=='arm64' else 0x100000c)
            self.assertEqual(count,225)
            for path,record in report['installed_data'].items():
                self.assertEqual(sha(files[path.lstrip('/')]),record['sha256']);self.assertEqual(sha(Path(record['source']).read_bytes()),record['source_sha256'])

    def test_actual_graph_and_required_libsystem_context(self):
        for arch,cpu in [('arm64',0x100000c),('x86_64',0x1000007)]:
            report=json.loads((PACKAGE/f'xfce-desktop-runtime.{arch}.json').read_text());bundle=PACKAGE/f'xfce-desktop-runtime.{arch}.tar';files=tar_files(bundle)
            self.assertEqual(sha(bundle.read_bytes()),report['bundle_sha256']);images={}
            for path,data in files.items():
                self.assertFalse(data.startswith(b'\x7fELF'),path)
                if data.startswith(b'\xcf\xfa\xed\xfe'):
                    self.assertEqual(struct.unpack_from('<I',data,4)[0],cpu);verify_code_signature(data);images['/'+path]=image_metadata(data)
            loader=image_metadata(Path(f'userland/native/dyld.{arch}').read_bytes())['exports']|{'dyld_stub_binder'}
            self.assertEqual(len(report['libraries']),99);self.assertEqual(len(report['programs']),31);self.assertEqual(len(report['modules']),12)
            records={**report['libraries'],**report['programs'],**report['modules'],'gio-helper':report['launch_desktop']}
            for name,record in records.items():
                path=record['install_path'];self.assertEqual(sha(files[path.lstrip('/')]),record['sha256'],name)
                self.assertEqual(verify_imports(path,images,loader,flat_namespace_root=path),record['import_resolutions'],name)
                if 'startup_closure' in record:self.assertEqual(verify_closure(path,images,loader),record['startup_closure'])
            # This utility has no direct pthread calls. Removing its mandatory
            # facade must expose the real libc-to-pthread private data binding.
            broken=copy.deepcopy(images);entry=broken['/usr/bin/iceauth'];entry['dependencies']=[(cmd,path) for cmd,path in entry['dependencies'] if path!='/usr/lib/libSystem.B.dylib']
            with self.assertRaises(ValueError):verify_closure('/usr/bin/iceauth',broken,loader)
            damaged=bytearray(files['usr/lib/xfce4/panel/plugins/libclock.so']);damaged[-1]^=1
            with self.assertRaises(ValueError):verify_code_signature(damaged)
            self.assertFalse(report['desktop_checked'])
if __name__=='__main__':unittest.main()
