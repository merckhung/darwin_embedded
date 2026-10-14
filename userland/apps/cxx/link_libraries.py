#!/usr/bin/env python3
"""Link the original LLVM C++ runtime as signed native Mach-O libraries."""
import argparse
import json
from pathlib import Path
import sys
import tarfile
import tempfile
from build_libraries import run, symbols, definitions, digest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"x11"))
from link_auth import image_metadata,verify_closure
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"scripts"))
from sign_diagnostic_macho import sign

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('libraries','build-report','system','dyld','output','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--module',type=Path)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args()
    compiled=json.loads(a.build_report.read_text())
    if compiled['architecture']!=a.arch or digest(a.libraries)!=compiled['bundle_sha256']:raise ValueError('C++ archive/report mismatch')
    with tempfile.TemporaryDirectory(prefix='.native-cxx-link-',dir=a.output.parent) as directory:
        work=Path(directory);objects,tree=work/'objects',work/'runtime'
        for source,destination in ((a.libraries,objects),(a.system,tree)):
            with tarfile.open(source) as archive:archive.extractall(destination,filter='data')
        images={'/'+str(f.relative_to(tree)):image_metadata(f.read_bytes()) for f in tree.rglob('*.dylib')}
        common=['/usr/lib/llvm-20/bin/ld64.lld','-arch',a.arch,'-no_fixup_chains','-dead_strip_dylibs','-platform_version','macos','26.5','26.5','-adhoc_codesign','-undefined','dynamic_lookup','-needed_library',tree/'usr/lib/libSystem.B.dylib','-L',tree/'usr/lib/system']
        loader=image_metadata(a.dyld.read_bytes())['exports']|{'dyld_stub_binder'}
        records={}
        for name in ('libunwind','libc++abi','libc++'):
            archive=objects/'usr/lib'/(name+'.a');record=compiled['libraries'][name]
            if digest(archive)!=record['archive_sha256']:raise ValueError('modified C++ archive: '+name)
            needed=set()
            for symbol in symbols(archive)[1]-definitions(archive)-loader-{'___dso_handle'}:
                providers={path for path,image in images.items() if symbol in image['exports']}
                if len(providers)!=1:raise ValueError('C++ import needs a genuine unambiguous provider: '+symbol+': '+repr(providers))
                needed.update(providers)
            path='/usr/lib/'+name+'.1.dylib';target=tree/path.lstrip('/')
            run([*common,'-dylib','-install_name',path,'-compatibility_version','1.0','-current_version','1.0','-force_load',archive,*[tree/f.lstrip('/') for f in sorted(needed)],'-o',target])
            sign(target);images[path]=image_metadata(target.read_bytes())
            records[name]={'install_path':path,'sha256':digest(target),'archive_sha256':digest(archive)}
        for record in records.values():record['startup_closure']=verify_closure(record['install_path'],images,loader)
        if a.module:
            path='/usr/lib/libnative_cxx_test.dylib';target=tree/path.lstrip('/')
            run([*common,'-dylib','-install_name',path,a.module,*[tree/r['install_path'].lstrip('/') for r in records.values()],'-o',target])
            sign(target);images[path]=image_metadata(target.read_bytes())
            records['test-module']={'install_path':path,'sha256':digest(target),'startup_closure':verify_closure(path,images,loader)}
        with tarfile.open(a.output,'w') as archive:
            for file in sorted(tree.rglob('*')):
                if file.is_file():
                    info=archive.gettarinfo(str(file),str(file.relative_to(tree)));info.uid=info.gid=info.mtime=0;info.uname=info.gname='root'
                    with file.open('rb') as stream:archive.addfile(info,stream)
        a.report.write_text(json.dumps({'architecture':a.arch,'libraries':records,'bundle_sha256':digest(a.output),'runtime_checked':False,'scope':'Signed original LLVM native C++ libraries; guest execution remains a separate validation stage.'},indent=2)+'\n')
        print(f'Native LLVM C++ linkage {a.arch}: three signed Mach-O libraries')
if __name__=='__main__':main()
