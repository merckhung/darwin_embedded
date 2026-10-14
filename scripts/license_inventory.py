#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Index retained third-party provenance and license notices without relabeling code."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    paths = [Path(f) for f in files if f and (ROOT / f).is_file()]
    manifests = [p for p in paths if p.name == 'UPSTREAM.json']
    roots = {p.parent for p in manifests}
    # Some older host imports have source-header notices rather than a manifest.
    roots.update(Path('tools') / name for name in ('cctools', 'dtrace_ctf', 'unifdef', 'xar'))
    records = []
    notice_name = re.compile(r'^(?:APPLE_LICENSE|LICENSE(?:[._-].*)?|LICENCE(?:[._-].*)?|COPYING(?:[._-].*)?|COPYRIGHT(?:[._-].*)?|copyright)$', re.I)
    notice_paths = [p for p in paths if notice_name.match(p.name)]
    puredarwin_repository = 'https://github.com/PureDarwin/PureDarwin'
    puredarwin_notice = {
        'path': 'PUREDARWIN_LICENSE.txt',
        'sha256': hashlib.sha256((ROOT / 'PUREDARWIN_LICENSE.txt').read_bytes()).hexdigest(),
        'repository': puredarwin_repository,
        'commit': 'bd832c0035744ff3edc2f6259853a30bf1c624e7',
        'license': 'BSD-2-Clause',
        'scope': 'PureDarwin-original code; individual source notices and other imported licenses remain applicable.',
    }
    for root in sorted(roots):
        manifest = ROOT / root / 'UPSTREAM.json'
        metadata = json.loads(manifest.read_text()) if manifest.is_file() else {}
        notices = []
        for p in notice_paths:
            if not p.is_relative_to(root):
                continue
            if any(other != root and other.is_relative_to(root) and p.is_relative_to(other) for other in roots):
                continue
            notices.append({'path': str(p), 'sha256': hashlib.sha256((ROOT / p).read_bytes()).hexdigest()})
        # Newly introduced publisher notices are included before their first commit.
        if root == Path('bootloader/ovmf') and not any(n['path'].endswith('/COPYRIGHT') for n in notices):
            p = root / 'COPYRIGHT'
            notices.append({'path': str(p), 'sha256': hashlib.sha256((ROOT / p).read_bytes()).hexdigest()})
        record = {'component': metadata.get('project', root.name), 'root': str(root),
                  'license_scope': 'Retained license texts and individual source headers; no root-license override.',
                  'notices': sorted(notices, key=lambda n: n['path'])}
        if manifest.is_file():
            record['provenance'] = str(root / 'UPSTREAM.json')
        for key in ('version', 'tag', 'commit', 'license', 'licenses'):
            if key in metadata:
                record[key] = metadata[key]
        uses_puredarwin = (
            metadata.get('repository') == puredarwin_repository or
            metadata.get('linux_port_repository') == puredarwin_repository or
            any(source.get('repository') == puredarwin_repository
                for source in metadata.get('compatibility_sources', []))
        )
        if uses_puredarwin:
            record['upstream_project_notices'] = [puredarwin_notice]
        if not notices:
            record['notice_location'] = (
                'Source file headers and the retained PureDarwin project notice; per-file terms govern.'
                if uses_puredarwin else
                'Source file headers; consult the provenance record where present.'
            )
        records.append(record)
    output = {'format_version': 1, 'scope': 'Third-party notice index, not a whole-component license determination.',
              'components': records}
    (ROOT / 'docs/licenses.json').write_text(json.dumps(output, indent=2) + '\n')
    print(f'Indexed {len(records)} components and {sum(len(r["notices"]) for r in records)} notice files.')


if __name__ == '__main__':
    main()
