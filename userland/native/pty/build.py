#!/usr/bin/env python3
"""Compile a native PTY consumer and describe its real LibSystem runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch',required=True,choices=['arm64','x86_64'])
    p.add_argument('--runtime',type=Path)
    p.add_argument('--sdk',type=Path)
    p.add_argument('--source',type=Path)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--requests',type=Path)
    a=p.parse_args()
    if a.runtime:
        a.output.write_text(json.dumps({'architecture':a.arch,'bundle_sha256':hashlib.sha256(a.runtime.read_bytes()).hexdigest()})+'\n')
        return
    if not all((a.sdk,a.source,a.requests)):p.error('compilation requires SDK, source and requests')
    with tempfile.TemporaryDirectory(dir=a.output.parent,prefix='.native-pty-') as directory:
        sdk=Path(directory)
        with tarfile.open(a.sdk) as archive:archive.extractall(sdk,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('PTY SDK CPU mismatch')
        resource=subprocess.check_output(['clang-20','-print-resource-dir'],text=True).strip()
        flags=['clang-20',f'--target={a.arch}-apple-macos26.5','-std=gnu11','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-D_DARWIN_C_SOURCE=1','-Wall','-Wextra','-Werror','-Wno-availability','-isystem',str(sdk/'usr/include'),'-isystem',resource+'/include']
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        subprocess.run([*flags,'-c',str(a.source),'-o',str(a.output)],check=True)
        if struct.unpack_from('<4I',a.output.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign PTY object')
        imports=subprocess.check_output(['llvm-nm-20','--undefined-only','-j',str(a.output)],text=True)
        a.requests.write_text('\n'.join(sorted(set(imports.splitlines())))+'\n')
if __name__=='__main__':main()
