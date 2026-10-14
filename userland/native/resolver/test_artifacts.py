#!/usr/bin/env python3
"""Audit real resolver objects, provenance, and private component placement."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0, str(Path('scripts').resolve()))
from build_native_system import append_component_archives, verify_import_ordinals
from native_components import run, symbols
sys.path.insert(0, str(Path("userland/apps/x11").resolve()))
from link_auth import image_metadata, verify_imports
from check_clients import verify_code_signature, tar_files


class ResolverArtifacts(unittest.TestCase):
    def test_upstream_bytes(self):
        for manifest in Path('src/Applications/resolver').rglob('UPSTREAM.json'):
            data = json.loads(manifest.read_text())
            for relative, expected in data['files_sha256'].items():
                with self.subTest(source=str(manifest.parent / relative)):
                    self.assertEqual(hashlib.sha256((manifest.parent / relative).read_bytes()).hexdigest(), expected)

    def test_native_component_members(self):
        for arch in ('arm64', 'x86_64'):
            archive = Path(f'userland/native/resolver/libresolver.{arch}.a').resolve()
            report = json.loads(Path(f'userland/native/resolver/resolver.{arch}.json').read_text())
            components = {'libc': []}
            record = append_component_archives(components, ['libc=' + str(archive)], arch)
            self.assertEqual(record[0]['objects'], len(report['objects']))
            self.assertEqual(record[0]['sha256'], report['archive_sha256'])
            with self.assertRaisesRegex(ValueError, 'wrong native CPU'):
                append_component_archives({'libc': []}, ['libc=' + str(archive)],
                                          'arm64' if arch == 'x86_64' else 'x86_64')
            with self.assertRaisesRegex(ValueError, 'unknown built-in'):
                append_component_archives({'libc': []}, ['pretend_service=' + str(archive)], arch)
            with tempfile.TemporaryDirectory() as directory:
                damaged = Path(directory) / 'bad.a'
                data = bytearray(archive.read_bytes())
                offset = data.index(bytes.fromhex('cffaedfe'))
                data[offset:offset + 4] = b'\x7fELF'
                damaged.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'not a native Mach-O'):
                    append_component_archives({'libc': []}, ['libc=' + str(damaged)], arch)

    def test_complete_signed_client_bindings(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/native/resolver/client-runtime.{arch}.tar"))
            report = json.loads(Path(f"userland/native/resolver/client-runtime.{arch}.json").read_text())
            self.assertEqual(len(report["libraries"]), 29)
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"] | {"dyld_stub_binder"}
            for name, record in report["libraries"].items():
                with self.subTest(arch=arch, library=name):
                    path = record["install_path"]
                    data = files[path[1:]]
                    magic, actual_cpu, _, kind = struct.unpack_from("<4I", data)
                    self.assertEqual((magic, actual_cpu, kind), (0xfeedfacf, cpu, 6))
                    self.assertGreater(verify_code_signature(data), 0)
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record["sha256"])
                    self.assertEqual(verify_imports(path, images, loader), record["import_resolutions"])
                    # A deliberately invalid ordinal must fail even when every
                    # symbol still exists elsewhere in the same installation.
                    imported = next((item for item in images[path]["imports"] if not item["weak"]), None)
                    if imported is None:
                        self.assertEqual(name, "libX11-xcb")
                        continue
                    changed = bytearray(data)
                    position = imported["nlist_offset"] + 6
                    description = struct.unpack_from("<H", changed, position)[0]
                    struct.pack_into("<H", changed, position, (description & 0xff) | (0xfd << 8))
                    with self.assertRaisesRegex(ValueError, "dependency ordinal"):
                        verify_imports(path, {**images, path: image_metadata(changed)}, loader)

    def test_x11_closure_keeps_private_libc_state(self):
        for arch in ('arm64', 'x86_64'):
            report = json.loads(Path(f'userland/native/resolver/x11_system_{arch}.json').read_text())
            self.assertEqual(len(report['requested_exports']), 138)
            self.assertNotIn('_libc_hooks', report['requested_exports'])
            self.assertEqual(report['component_archive_inputs'][0]['component'], 'libc')
            with tempfile.TemporaryDirectory() as directory:
                tree = Path(directory)
                with tarfile.open(f'userland/native/resolver/x11_system_{arch}.tar') as archive:
                    archive.extractall(tree, filter='data')
                components = {record['install_path']: set(record['exports'])
                              for name, record in report['libraries'].items() if name != 'LibSystem'}
                for record in report['libraries'].values():
                    image = tree / record['install_path'].lstrip('/')
                    self.assertEqual(hashlib.sha256(image.read_bytes()).hexdigest(), record['sha256'])
                    verify_import_ordinals(image, components)
                    self.assertNotIn('_libc_hooks', symbols(image)[0] | symbols(image)[1])
                self.assertIn('_fdopen$DARWIN_EXTSN', report['libraries']['libc']['exports'])
                self.assertIn('_getaddrinfo', report['libraries']['resolver']['exports'])

    def test_guest_system_overlay_preserves_client_bindings(self):
        for arch in ('arm64', 'x86_64'):
            files = tar_files(Path(f'userland/native/resolver/client-runtime.{arch}.tar'))
            files.update(tar_files(Path(f'userland/native/resolver/test_system_{arch}.tar')))
            images = {'/' + path: image_metadata(data) for path, data in files.items()
                      if path.endswith('.dylib')}
            loader = image_metadata(Path(f'userland/native/dyld.{arch}').read_bytes())['exports'] | {'dyld_stub_binder'}
            clients = json.loads(Path(f'userland/native/resolver/client-runtime.{arch}.json').read_text())
            for record in clients['libraries'].values():
                with self.subTest(architecture=arch, client=record['install_path']):
                    # Probe-only API additions replace the runtime components;
                    # the actual clients must still bind through those images.
                    self.assertEqual(verify_imports(record['install_path'], images, loader),
                                     record['import_resolutions'])


if __name__ == '__main__':
    unittest.main()
