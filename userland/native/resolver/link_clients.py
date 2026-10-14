#!/usr/bin/env python3
"""Link every upstream X11 client object into signed native Darwin dylibs."""
import argparse
import json
from pathlib import Path
import re
import struct
import sys
import tarfile
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'apps/x11'))
from build_clients import digest, pack, run, symbols
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
from sign_diagnostic_macho import sign


def version(source, variable):
    """Read libtool's actual declared interface version, without running it."""
    match = re.search(r'^' + re.escape(variable) + r'_la_LDFLAGS\s*=\s*-version-(info|number)\s+(\d+):(\d+):(\d+)',
                      source.read_text(), re.MULTILINE)
    if not match:
        raise ValueError('missing upstream library version: ' + variable)
    mode, first, second, third = match.groups()
    first, second, third = int(first), int(second), int(third)
    current, revision, age = ((first, second, third) if mode == 'info' else (first + second, third, second))
    if age > current:
        raise ValueError('invalid upstream interface age')
    # Exact Darwin conversion from the imported ltmain.sh version_type=darwin.
    return str(current - age), f'{current + 1}.0.0', f'{current + 1}.{revision}.0'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'clients', 'system', 'dyld', 'output', 'report'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--arch', choices=('arm64', 'x86_64'), required=True)
    a = p.parse_args()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.native-xclients-', dir=a.output.parent) as directory:
        work = Path(directory).resolve()
        clients, tree = work / 'clients', work / 'install'
        for source, destination in ((a.clients, clients), (a.system, tree)):
            with tarfile.open(source) as archive:
                archive.extractall(destination, filter='data')
        upstream = a.root.resolve() / 'src/Applications/x11'
        paths = sorted((clients / 'usr/lib').glob('*.a'))
        archives = {path.stem: path for path in paths if not path.stem.startswith('libnative_')}
        names = ['libXau', 'libXdmcp', 'libxcb']
        names += sorted(name for name in archives if name.startswith('libxcb-'))
        names += ['libX11', 'libX11-xcb']
        if set(names) != set(archives) or len(names) != len(archives):
            raise ValueError('public X11 archive inventory mismatch')
        loader_exports = symbols(a.dyld)[0] | {'dyld_stub_binder'}
        images = {'/' + str(path.relative_to(tree)): image_metadata(path.read_bytes())
                  for path in (tree / 'usr/lib').rglob('*.dylib')}
        built, records = {}, {}
        for name in names:
            if name in ('libXau', 'libXdmcp'):
                project, release = ('libXau', '1.0.12') if name == 'libXau' else ('libXdmcp', '1.1.5')
                version_source = upstream / project / release / 'Makefile.am'
                dependencies = []
            elif name.startswith('libxcb'):
                version_source = upstream / 'libxcb/1.17.0/src/Makefile.am'
                dependencies = ['libXau', 'libXdmcp'] if name == 'libxcb' else ['libxcb']
            else:
                version_source = upstream / 'libX11/1.8.13/src/Makefile.am'
                dependencies = ['libxcb'] if name == 'libX11' else ['libX11', 'libxcb']
            major, compatibility, current = version(version_source, name.replace('-', '_'))
            image = tree / 'usr/lib' / (name + '.' + major + '.dylib')
            archive_exports, archive_imports, _ = symbols(archives[name])
            # Preserve additional genuine cross-extension dependencies observed
            # in the selected source, rather than allowing global-symbol luck.
            for provider, (path, exports) in built.items():
                if archive_imports & exports and provider not in dependencies:
                    dependencies.append(provider)
            run(['/usr/lib/llvm-20/bin/ld64.lld', '-arch', a.arch, '-dylib', '-no_fixup_chains',
                 '-platform_version', 'macos', '26.5', '26.5', '-adhoc_codesign',
                 '-undefined', 'dynamic_lookup', '-install_name', '/usr/lib/' + image.name,
                 '-compatibility_version', compatibility, '-current_version', current,
                 '-force_load', archives[name], *[built[item][0] for item in dependencies],
                 '-L', tree / 'usr/lib/system', tree / 'usr/lib/libSystem.B.dylib', '-o', image])
            sign(image)
            magic, cpu, _, kind = struct.unpack_from('<4I', image.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if a.arch == 'arm64' else 0x1000007, 6):
                raise ValueError('non-native client dylib: ' + name)
            exports, imports, weak = symbols(image)
            # The linker owns the per-image C++ destructor registration handle.
            if exports - {'___dso_handle'} != archive_exports:
                raise ValueError('upstream public client API inventory changed: ' + name)
            install = '/usr/lib/' + image.name
            images[install] = image_metadata(image.read_bytes())
            bindings = verify_imports(install, images, loader_exports)
            if {entry['symbol'] for entry in bindings if not entry['weak']} != imports:
                raise ValueError('native client import audits disagree: ' + name)
            records[name] = {'install_path': install, 'sha256': digest(image),
                'source_archive_sha256': digest(archives[name]), 'exports': sorted(exports),
                'imports': sorted(imports), 'weak_imports': sorted(weak), 'import_resolutions': bindings,
                'version_source': str(version_source.relative_to(a.root.resolve())),
                'version_source_sha256': digest(version_source),
                'compatibility_version': compatibility, 'current_version': current}
            built[name] = image, exports
        pack(tree, a.output)
        a.report.write_text(json.dumps({'architecture': a.arch, 'libraries': records,
            'clients_sha256': digest(a.clients), 'system_sha256': digest(a.system), 'dyld_sha256': digest(a.dyld),
            'bundle_sha256': digest(a.output), 'runtime_checked': False,
            'validation': 'Every public archive force-loaded; signed native dylibs retain all upstream APIs; complete actual dependency-ordinal and reexport closure.'}, indent=2) + '\n')
        print(f'Native X11 clients {a.arch}: {len(records)} complete signed dylibs, all actual strong bindings verified')


if __name__ == '__main__':
    main()
