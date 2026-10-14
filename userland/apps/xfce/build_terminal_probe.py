#!/usr/bin/env python3
"""Compile the desktop/terminal probe and request its actual native APIs."""
import argparse,json,os,struct,tarfile,tempfile
from pathlib import Path
from build_base import run,symbols,digest

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('sdk','headers','source','output','imports','report'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--arch',choices=('arm64','x86_64'),required=True);args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='.native-terminal-probe-',dir=args.output.parent) as directory:
        work=Path(directory).resolve();sdk,headers=work/'sdk',work/'headers';os.environ['TMPDIR']=str(work)
        for file,target in ((args.sdk,sdk),(args.headers,headers)):
            with tarfile.open(file) as archive:archive.extractall(target,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=args.arch:raise ValueError('probe SDK mismatch')
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang-20',f'--target={args.arch}-apple-macos26.5','-std=gnu11','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-D_DARWIN_C_SOURCE=1','-D_FORTIFY_SOURCE=0','-Wall','-Wextra','-Werror','-Wno-availability','-isystem',sdk/'usr/include','-isystem',resource+'/include']
        for include in ('','glib-2.0','xfce4','xfce4/xfconf-0','gtk-3.0','pango-1.0','gdk-pixbuf-2.0','atk-1.0','cairo','harfbuzz','freetype2','xfce4/libxfce4ui-2','xfce4/libxfce4kbd-private-3'):flags+=['-I',headers/'usr/include'/include]
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if args.arch=='arm64' else ['-march=penryn']
        run([*flags,'-c',args.source,'-o',args.output]);cpu,kind=struct.unpack_from('<4I',args.output.read_bytes())[1::2]
        if (cpu,kind)!=(0x100000c if args.arch=='arm64' else 0x1000007,1):raise ValueError('foreign terminal probe object')
        public,imports,weak=symbols(args.output);apps=set()
        for archive in (headers/'usr/lib').glob('*.a'):apps.update(symbols(archive)[0])
        args.imports.write_text('\n'.join(sorted(imports-apps))+'\n')
        args.report.write_text(json.dumps({'architecture':args.arch,'source_sha256':digest(args.source),'object_sha256':digest(args.output),'imports':sorted(imports),'runtime_checked':False},indent=2)+'\n')
if __name__=='__main__':main()
