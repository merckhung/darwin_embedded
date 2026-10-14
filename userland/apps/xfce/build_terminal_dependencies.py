#!/usr/bin/env python3
"""Build original LZ4, retaining the native FriBidi already supplied by Pango."""
import argparse,json,os,re,shutil,struct,tarfile,tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from build_base import run,digest,definitions,pack
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'cxx'))
from build_libraries import symbols

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','sdk','dependencies','output','imports','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args();root=a.root.resolve()
    with tempfile.TemporaryDirectory(prefix='.native-terminal-deps-',dir=a.output.parent) as directory:
        work=Path(directory).resolve();sdk,install=work/'sdk',work/'install';os.environ['TMPDIR']=str(work)
        for path,target in ((a.sdk,sdk),(a.dependencies,install)):
            with tarfile.open(path) as archive:archive.extractall(target,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('terminal dependency SDK mismatch')
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang-20',f'--target={a.arch}-apple-macos26.5','-std=gnu11','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-D_DARWIN_C_SOURCE=1','-D_FORTIFY_SOURCE=0','-Werror=implicit-function-declaration','-Wno-availability','-isystem',resource+'/include','-isystem',sdk/'usr/include']
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        libraries={}
        for project,version in (('lz4','1.10.0'),):
            source=root/'src/Applications/xfce-prerequisites'/project/version
            manifest=json.loads((source/'UPSTREAM.json').read_text())
            for name,expected in manifest['upstream_files_sha256'].items():
                if digest(source/name)!=expected:raise ValueError('modified original terminal dependency: '+name)
            generated=work/project;generated.mkdir();local=['-I',generated,'-I',source/'lib']
            # Read the original source inventory; Bazel invokes the compiler directly.
            files=sorted((source/'lib').glob('*.c'));name='liblz4';include=install/'usr/include';include.mkdir(parents=True,exist_ok=True)
            for header in ('lz4.h','lz4hc.h','lz4frame.h','lz4file.h'):shutil.copyfile(source/'lib'/header,include/header)
            jobs=[(file,generated/f'{project}-{i:02d}.o') for i,file in enumerate(files)]
            def compile_one(job):
                file,obj=job
                try:run([*flags,*local,'-c',file,'-o',obj])
                except RuntimeError as error:return {'error':str(error)}
                if struct.unpack_from('<4I',obj.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign terminal dependency object')
                return {'source':str(file.relative_to(root)),'source_sha256':digest(file),'member':obj.name,'object_sha256':digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
            errors=[r['error'] for r in records if 'error' in r]
            if errors:raise ValueError('\n'.join(errors))
            archive=install/'usr/lib'/(name+'.a');archive.unlink(missing_ok=True);run(['llvm-ar-20','--format=darwin','rcsD',archive,*[obj for _,obj in jobs]])
            public,imported,weak=symbols(archive)
            libraries[name]={'sources':records,'exports':sorted(public),'imports':sorted(imported-definitions(archive)),'weak_imports':sorted(weak),'archive_sha256':digest(archive)}
            licenses=install/'usr/share/licenses'/project;licenses.mkdir(parents=True,exist_ok=True)
            for file in [*source.glob('COPYING*'),*source.glob('LICENSE*')]:
                if file.is_file():shutil.copyfile(file,licenses/file.name)
        defined=set();imported=set()
        for archive in install.rglob('*.a'):
            if archive.is_relative_to(install/'usr/libexec/xorg'):continue
            defined.update(definitions(archive));imported.update(symbols(archive)[1])
        a.imports.write_text('\n'.join(sorted(imported-defined-{'___dso_handle'}))+'\n');pack(install,a.output)
        a.report.write_text(json.dumps({'architecture':a.arch,'libraries':libraries,'bundle_sha256':digest(a.output),'runtime_checked':False},indent=2)+'\n')
        print(f'Native terminal dependencies {a.arch}: '+str(sum(len(r['sources']) for r in libraries.values()))+' original Mach-O objects')
if __name__=='__main__':main()
