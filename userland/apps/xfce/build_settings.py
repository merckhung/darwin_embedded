#!/usr/bin/env python3
"""Build the original X11 Xfce settings daemon and GTK settings module."""
import argparse,json,os,re,shutil,struct,tarfile,tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from build_base import run,symbols,digest,definitions,pack
from build_desktop import inventory

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','sdk','dependencies','output','imports','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args();root=a.root.resolve()
    source=root/'src/Applications/xfce/xfce4-settings/4.20.5';manifest=json.loads((source/'UPSTREAM.json').read_text())
    for name,expected in manifest['upstream_files_sha256'].items():
        if digest(source/name)!=expected:raise ValueError('modified original settings source: '+name)
    with tempfile.TemporaryDirectory(prefix='.native-xfsettings-',dir=a.output.parent) as directory:
        work=Path(directory).resolve();sdk,install,generated=work/'sdk',work/'install',work/'generated';generated.mkdir();os.environ['TMPDIR']=str(work)
        for path,target in ((a.sdk,sdk),(a.dependencies,install)):
            with tarfile.open(path) as archive:archive.extractall(target,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('settings SDK mismatch')
        values={'PACKAGE':'"xfce4-settings"','PACKAGE_NAME':'"xfce4-settings"','PACKAGE_VERSION':'"4.20.5"','PACKAGE_STRING':'"xfce4-settings 4.20.5"','VERSION':'"4.20.5"','GETTEXT_PACKAGE':'"xfce4-settings"','ENABLE_NLS':1,'ENABLE_X11':1,'ENABLE_DISPLAY_SETTINGS':1,'HAVE_XRANDR':1,'HAVE_XCURSOR':1,'HAVE_MATH_H':1,'HAVE_STRING_H':1,'HAVE_UNISTD_H':1,'DATADIR':'"/usr/share"','BINDIR':'"/usr/bin"','SYSCONFIGDIR':'"/etc"','LOCALEDIR':'"/usr/share/locale"','PNP_IDS':'"/usr/share/hwdata/pnp.ids"'}
        values.update(PACKAGE_BUGREPORT='"https://gitlab.xfce.org/xfce/xfce4-settings/-/issues"',COPYRIGHT_YEAR='"2026"',VERSION_FULL='"4.20.5"')
        (generated/'config.h').write_text(''.join(f'#define {key} {value}\n' for key,value in sorted(values.items())))
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang-20',f'--target={a.arch}-apple-macos26.5','-std=gnu11','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-D_DARWIN_C_SOURCE=1','-D__BSD_VISIBLE=1','-D_FORTIFY_SOURCE=0','-DHAVE_CONFIG_H','-DG_LOG_DOMAIN="xfsettingsd"','-DPNP_IDS="/usr/share/hwdata/pnp.ids"','-Werror=implicit-function-declaration','-Werror=incompatible-pointer-types','-Wno-availability','-isystem',resource+'/include','-isystem',sdk/'usr/include','-I',generated,'-I',source,'-I',source/'xfsettingsd','-I',source/'common']
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        for include in ('','glib-2.0','gio-unix-2.0','gtk-3.0','pango-1.0','gdk-pixbuf-2.0','atk-1.0','harfbuzz','freetype2','cairo','xfce4','xfce4/xfconf-0','xfce4/libxfce4ui-2','xfce4/libxfce4kbd-private-3'):
            flags+=['-I',install/'usr/include'/include]
        enabled={'ENABLE_X11':True,'ENABLE_DISPLAY_SETTINGS':True,'HAVE_XRANDR':True}
        common=[source/'common'/name for name in inventory(source/'common/Makefile.am','libxfce4_settings_la_SOURCES',enabled) if name.endswith('.c')]
        programs,modules={},{}
        for name,variable,kind,path in [('xfsettingsd','xfsettingsd_SOURCES','programs','/usr/bin/xfsettingsd'),('libxfsettingsd-gtk-settings-sync','libxfsettingsd_gtk_settings_sync_la_SOURCES','modules','/usr/lib/gtk-3.0/modules/libxfsettingsd-gtk-settings-sync.so')]:
            files=[source/'xfsettingsd'/f for f in inventory(source/'xfsettingsd/Makefile.am',variable,enabled) if f.endswith('.c')]+common
            jobs=[(f,generated/(name+f'-{i:02d}.o')) for i,f in enumerate(files)]
            def compile_one(job):
                file,obj=job
                try:run([*flags,'-c',file,'-o',obj])
                except RuntimeError as error:return {'error':str(error)}
                if struct.unpack_from('<4I',obj.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign settings object')
                return {'source':str(file.relative_to(root)),'source_sha256':digest(file),'member':obj.name,'object_sha256':digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
            errors=[r['error'] for r in records if 'error' in r]
            if errors:raise ValueError('\n'.join(errors))
            archive=install/'usr/libexec/xfce'/(name+'.a');archive.parent.mkdir(parents=True,exist_ok=True)
            run(['llvm-ar-20','--format=darwin','rcsD',archive,*[obj for _,obj in jobs]])
            public,imported,weak=symbols(archive)
            record={'sources':records,'archive_path':str(archive.relative_to(install)),'archive_sha256':digest(archive),'exports':sorted(public),'imports':sorted(imported-definitions(archive)),'weak_imports':sorted(weak),'install_path':path}
            (programs if kind=='programs' else modules)[name]=record
        settings=install/'etc/xdg/xfce4/xfconf/xfce-perchannel-xml';settings.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source/'xfsettingsd/xsettings.xml',settings/'xsettings.xml')
        entry=(source/'xfsettingsd/xfsettingsd.desktop.in').read_text();entry=re.sub(r'^_(\w+)=',r'\1=',entry,flags=re.M)
        autostart=install/'etc/xdg/autostart';autostart.mkdir(parents=True,exist_ok=True);(autostart/'xfsettingsd.desktop').write_text(entry)
        licenses=install/'usr/share/licenses'/source.parent.name;licenses.mkdir(parents=True,exist_ok=True)
        for file in [*source.glob('COPYING*'),*source.glob('LICENSE*')]:
            if file.is_file():shutil.copyfile(file,licenses/file.name)
        defined=set();imported=set()
        for file in install.rglob('*.a'):defined.update(definitions(file));imported.update(symbols(file)[1])
        a.imports.write_text('\n'.join(sorted(imported-defined-{'___dso_handle'}))+'\n');pack(install,a.output)
        a.report.write_text(json.dumps({'architecture':a.arch,'programs':programs,'modules':modules,'libraries':{},'configuration':values,'bundle_sha256':digest(a.output),'runtime_checked':False},indent=2)+'\n')
        print(f'Native Xfce settings {a.arch}: '+str(sum(len(r['sources']) for r in [*programs.values(),*modules.values()]))+' Mach-O objects')
if __name__=='__main__':main()
