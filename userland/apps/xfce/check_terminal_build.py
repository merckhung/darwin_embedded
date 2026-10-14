#!/usr/bin/env python3
"""Audit original terminal/settings/VTE sources and native build inventories."""
import hashlib,json,tarfile,unittest
from pathlib import Path
from check_wm import tar_files,archive_objects
def sha(data):return hashlib.sha256(data).hexdigest()
class NativeTerminalBuild(unittest.TestCase):
    def test_native_inventories_and_original_bytes(self):
        package=Path('userland/apps/xfce')
        expected={'terminal-dependencies':{'liblz4':5},'vte':{'libvte-2.91':46},'xfce-settings':{'xfsettingsd':18,'libxfsettingsd-gtk-settings-sync':6},'xfce-terminal':{'xfce4-terminal':16}}
        for arch,cpu in [('arm64',0x100000c),('x86_64',0x1000007)]:
            for prefix,counts in expected.items():
                report=json.loads((package/f'{prefix}.{arch}.json').read_text());bundle=package/f'{prefix}.{arch}.tar';files=tar_files(bundle)
                self.assertEqual(sha(bundle.read_bytes()),report['bundle_sha256']);records={}
                for category in ('libraries','programs','modules'):records.update(report.get(category,{}))
                self.assertEqual({n:len(r['sources']) for n,r in records.items()},counts)
                generated_hashes={sha(data) for name,data in files.items() if name.startswith('build-metadata/')}
                for name,record in records.items():
                    archive_path=record.get('archive_path',f'usr/lib/{name}.a');data=files[archive_path]
                    self.assertEqual(sha(data),record['archive_sha256']);self.assertEqual(archive_objects(data,cpu),[r['member'] for r in record['sources']])
                    for item in record['sources']:
                        if item.get('generated'):self.assertIn(item['source_sha256'],generated_hashes)
                        elif not item['source'].startswith('src/Applications/xfce-prerequisites/vte/'):
                            self.assertEqual(sha(Path(item['source']).read_bytes()),item['source_sha256'])
                    with self.assertRaises(ValueError):archive_objects(data,0x1000007 if arch=='arm64' else 0x100000c)
                self.assertFalse(report['runtime_checked'])
                if prefix=='vte':self.assertTrue(report['sdk_probes']['HAVE_ROUND'])
        manifest=json.loads(Path('src/Applications/xfce-prerequisites/vte/0.84.1/UPSTREAM.json').read_text())
        archive=Path('src/Applications/xfce-prerequisites/vte-0.84.1.tar.xz');self.assertEqual(sha(archive.read_bytes()),manifest['archive_sha256'])
        with tarfile.open(archive) as bundle:
            actual={m.name.split('/',1)[1]:sha(bundle.extractfile(m).read()) for m in bundle if m.isfile()}
        self.assertEqual(actual,manifest['upstream_files_sha256'])
if __name__=='__main__':unittest.main()
