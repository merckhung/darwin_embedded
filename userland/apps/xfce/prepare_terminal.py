#!/usr/bin/env python3
"""Join verified original C++/VTE/settings/terminal archives into one native stage."""
import argparse,json,tarfile,tempfile,shutil
from pathlib import Path
from build_base import digest,pack,definitions
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'cxx'))
from build_libraries import symbols
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('terminal','terminal-report','settings','settings-report','vte','vte-report','dependencies','dependencies-report','cxx','cxx-report','output','imports','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args()
    libraries,programs,modules={},{},{}
    with tempfile.TemporaryDirectory(prefix='.native-terminal-stage-',dir=a.output.parent) as directory:
        tree=Path(directory)
        for name in ('terminal','settings','vte','dependencies','cxx'):
            report=json.loads(getattr(a,name+'_report').read_text());bundle=getattr(a,name)
            if report['architecture']!=a.arch or digest(bundle)!=report['bundle_sha256']:raise ValueError('terminal stage source/report mismatch: '+name)
            libraries.update(report.get('libraries',{}));programs.update(report.get('programs',{}));modules.update(report.get('modules',{}))
            with tarfile.open(bundle) as archive:archive.extractall(tree,filter='data')
        defined=set();imported=set()
        for record in [*libraries.values(),*programs.values(),*modules.values()]:
            imported.update(record['imports'])
        for archive in (tree/'usr/lib').glob('*.a'):
            # Only public library definitions can provide another image's
            # imports. Retained program archives are separate executables.
            defined.update(symbols(archive)[0])
        a.imports.write_text('\n'.join(sorted(imported-defined-{'___dso_handle'}))+'\n');pack(tree,a.output)
        a.report.write_text(json.dumps({'architecture':a.arch,'libraries':libraries,'programs':programs,'modules':modules,'bundle_sha256':digest(a.output),'runtime_checked':False,'desktop_checked':False},indent=2)+'\n')
if __name__=='__main__':main()
