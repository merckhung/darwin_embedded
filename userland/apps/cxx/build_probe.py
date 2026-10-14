#!/usr/bin/env python3
"""Compile actual native C++ consumers and collect their native System APIs."""
import argparse,json,tarfile,tempfile
from pathlib import Path
from build_libraries import run,symbols,definitions,digest
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('sdk','headers','build-report','source','module-source','output','module-output','requests'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='.native-cxx-probe-',dir=a.output.parent) as directory:
        work=Path(directory);sdk,headers=work/'sdk',work/'headers'
        for archive_path,destination in ((a.sdk,sdk),(a.headers,headers)):
            with tarfile.open(archive_path) as archive:archive.extractall(destination,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('native C++ consumer SDK mismatch')
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang++-20',f'--target={a.arch}-apple-macos26.5','-std=gnu++23','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-nostdinc++','-D_DARWIN_C_SOURCE=1','-Werror','-Wno-availability','-isystem',headers/'usr/include/c++/v1','-isystem',resource+'/include','-isystem',sdk/'usr/include']
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        imported=set()
        for source,target in ((a.source,a.output),(a.module_source,a.module_output)):
            run([*flags,'-c',source,'-o',target]);imported.update(symbols(target)[1]-definitions(target))
        report=json.loads(a.build_report.read_text());cpp=set().union(*(set(r['exports']) for r in report['libraries'].values()))
        a.requests.write_text('\n'.join(sorted(imported-cpp-{'___dso_handle','_native_cpp_throw','_native_cpp_exception_type'}))+'\n')
if __name__=='__main__':main()
