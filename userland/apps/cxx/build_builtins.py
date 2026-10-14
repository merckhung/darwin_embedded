#!/usr/bin/env python3
"""Build original LLVM 128-bit integer division for the native Darwin runtime."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import tarfile
import tempfile
from build_libraries import run, symbols, definitions, digest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','sdk','output','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args()
    source=a.root.resolve()/'src/Applications/cxx/compiler-rt-builtins/20.1.8'
    manifest=json.loads((source/'UPSTREAM.json').read_text())
    for name,expected in manifest['upstream_files_sha256'].items():
        if digest(source/name)!=expected:raise ValueError('modified original compiler builtin: '+name)
    with tempfile.TemporaryDirectory(prefix='.native-int128-',dir=a.output.parent) as directory:
        work=Path(directory);sdk=work/'sdk'
        with tarfile.open(a.sdk) as archive:archive.extractall(sdk,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('numeric SDK architecture mismatch')
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang-20',f'--target={a.arch}-apple-macos26.5','-std=gnu11','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-D_DARWIN_C_SOURCE=1','-Wall','-Wextra','-Werror','-Wno-availability','-isystem',sdk/'usr/include','-isystem',resource+'/include','-I',source]
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        objects=[];records=[]
        for name in ('divti3.c','udivti3.c','udivmodti4.c','int_util.c'):
            obj=work/(name+'.o');run([*flags,'-c',source/name,'-o',obj])
            if struct.unpack_from('<4I',obj.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign numeric builtin object')
            objects.append(obj);records.append({'source':str((source/name).relative_to(a.root.resolve())),'source_sha256':digest(source/name),'object_sha256':digest(obj),'member':obj.name})
        run(['llvm-ar-20','--format=darwin','rcsD',a.output,*objects])
        public,imported,weak=symbols(a.output)
        if not {'___divti3','___udivti3','___udivmodti4'}<=public:raise ValueError('original 128-bit division routines missing')
        a.report.write_text(json.dumps({'architecture':a.arch,'sources':records,'archive_sha256':digest(a.output),'exports':sorted(public),'imports':sorted(imported-definitions(a.output)),'weak_imports':sorted(weak),'runtime_checked':False},indent=2)+'\n')
        print(f'Native LLVM int128 {a.arch}: four original Mach-O objects')
if __name__=='__main__':main()
