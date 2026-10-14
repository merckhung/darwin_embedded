#!/usr/bin/env python3
"""Compile the original Xfce terminal on native GTK3/VTE through Bazel."""
import argparse,ast,json,os,re,shutil,struct,subprocess,tarfile,tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from build_base import run,symbols,digest,definitions,pack

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','sdk','dependencies','output','imports','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args();root=a.root.resolve()
    source=root/'src/Applications/xfce/xfce4-terminal/1.2.0';manifest=json.loads((source/'UPSTREAM.json').read_text())
    for name,expected in manifest['upstream_files_sha256'].items():
        if digest(source/name)!=expected:raise ValueError('modified original terminal source: '+name)
    with tempfile.TemporaryDirectory(prefix='.native-xfce-terminal-',dir=a.output.parent) as directory:
        work=Path(directory).resolve();sdk,install,generated=work/'sdk',work/'install',work/'generated';generated.mkdir();os.environ['TMPDIR']=str(work)
        for path,target in ((a.sdk,sdk),(a.dependencies,install)):
            with tarfile.open(path) as archive:archive.extractall(target,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('terminal SDK mismatch')
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang-20',f'--target={a.arch}-apple-macos26.5','-std=gnu11','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-D_DARWIN_C_SOURCE=1','-D_FORTIFY_SOURCE=0','-DENABLE_X11=1','-DHAVE_XFCE_REVISION_H=1','-DG_LOG_DOMAIN="xfce4-terminal"','-Werror=implicit-function-declaration','-Werror=incompatible-pointer-types','-Wno-availability','-isystem',resource+'/include','-isystem',sdk/'usr/include','-I',generated,'-I',source,'-I',source/'terminal']
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        for include in ('','glib-2.0','gio-unix-2.0','gtk-3.0','pango-1.0','gdk-pixbuf-2.0','atk-1.0','harfbuzz','freetype2','cairo','xfce4','xfce4/xfconf-0','xfce4/libxfce4ui-2','xfce4/libxfce4kbd-private-3','vte-2.91'):
            flags+=['-I',install/'usr/include'/include]
        values={'PACKAGE':'xfce4-terminal','PACKAGE_NAME':'xfce4-terminal','PACKAGE_VERSION':'1.2.0','VERSION':'1.2.0','VERSION_SHORT':'1.2','PACKAGE_STRING':'xfce4-terminal 1.2.0','GETTEXT_PACKAGE':'xfce4-terminal','PACKAGE_BUGREPORT':'https://gitlab.xfce.org/apps/xfce4-terminal/-/issues','PACKAGE_DATADIR':'/usr/share/xfce4/terminal','PACKAGE_LOCALE_DIR':'/usr/share/locale','PREFIX':'/usr','BINDIR':'/usr/bin','DATADIR':'/usr/share','LIBDIR':'/usr/lib','LIBEXECDIR':'/usr/libexec','LOCALEDIR':'/usr/share/locale','SYSCONFDIR':'/etc','COPYRIGHT_YEAR':'2026'}
        flags += ['-D'+key+'='+json.dumps(value) for key,value in values.items()]
        for header in ('sys/types.h','errno.h','limits.h','locale.h','memory.h','pwd.h','signal.h','stdarg.h','stdlib.h','string.h','time.h','unistd.h'):
            file=generated/'probe.c';file.write_text('#include <'+header+'>\n')
            result=subprocess.run([*map(str,flags),'-Werror','-c',str(file),'-o',str(generated/'probe.o')],capture_output=True)
            if not result.returncode:flags+=['-DHAVE_'+re.sub(r'[^A-Z0-9]','_',header.upper())+'=1']
        (generated/'terminal-config.h').write_text((source/'terminal/terminal-config.h.in').read_text().replace('@TERMINAL_VERSION_DBUS@','5'))
        (generated/'xfce-revision.h').write_text((source/'xfce-revision.h.in').read_text().replace('@REVISION@','UNKNOWN'))
        for suffix,kind in (('h','header'),('c','body')):
            (generated/('terminal-marshal.'+suffix)).write_text(run(['glib-genmarshal','--'+kind,'--internal','--prefix=_terminal_marshal',source/'terminal/terminal-marshal.list']))
        recipe=(source/'terminal/meson.build').read_text()
        for match in re.finditer(r"gnome.mkenums\(\s*'([^']+)',(.*?)\n\)",recipe,re.S):
            command=['glib-mkenums']
            for option,value in re.findall(r"\b(fhead|fprod|vhead|vprod|vtail|ftail):\s*('(?:\\.|[^'])*')",match[2]):command+=['--'+option,ast.literal_eval(value)]
            command += [source/'terminal/terminal-preferences.h']
            (generated/match[1]).write_text(run(command))
        match=re.search(r'terminal_sources\s*=\s*\[(.*?)\n\]',recipe,re.S)
        files=[source/'terminal'/name for name in re.findall(r"'([^']+)'",match[1]) if name.endswith('.c')]
        files += [generated/'terminal-marshal.c',generated/'terminal-enum-types.c']
        original_dropdown=source/'terminal/terminal-window-dropdown.c'
        before='''  if (dropdown->set_monitor_idle_id != 0)
    g_source_remove (dropdown->set_monitor_idle_id);

  if (dropdown->monitor_removed_idle_id != 0)
    g_source_remove (dropdown->monitor_removed_idle_id);'''
        text=original_dropdown.read_text()
        if text.count(before)!=1:raise ValueError('original Wayland-only dropdown cleanup changed')
        compiled_dropdown=generated/'terminal-window-dropdown.c'
        compiled_dropdown.write_text(text.replace(before,'#ifdef HAVE_GTK_LAYER_SHELL\n'+before+'\n#endif'))
        jobs=[(file,generated/f'terminal-{i:02d}.o') for i,file in enumerate(files)]
        def compile_one(job):
            file,obj=job
            compiled=compiled_dropdown if file==original_dropdown else file
            try:run([*flags,'-c',compiled,'-o',obj])
            except RuntimeError as error:return {'error':str(error)}
            if struct.unpack_from('<4I',obj.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign terminal object')
            is_generated=file.is_relative_to(generated)
            return {'source':str(file.relative_to(generated if is_generated else root)),'generated':is_generated,'source_sha256':digest(file),'compiled_source_sha256':digest(compiled),'adaptation':'Guard cleanup of Wayland-only fields with their original feature condition' if file==original_dropdown else None,'member':obj.name,'object_sha256':digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
        errors=[r['error'] for r in records if 'error' in r]
        if errors:raise ValueError('Native Xfce terminal compilation failed:\n'+'\n'.join(errors))
        archive=install/'usr/libexec/xfce/xfce4-terminal.a';archive.parent.mkdir(parents=True,exist_ok=True)
        run(['llvm-ar-20','--format=darwin','rcsD',archive,*[obj for _,obj in jobs]])
        public,imported,weak=symbols(archive)
        preferences=install/'etc/xdg/xfce4/terminal/terminalrc';preferences.parent.mkdir(parents=True,exist_ok=True)
        preferences.write_text('[Configuration]\nEncoding=UTF-8\n')
        for file in source.glob('*.desktop.in'):
            destination=install/'usr/share/applications'/file.name.removesuffix('.in');destination.parent.mkdir(parents=True,exist_ok=True);destination.write_text(re.sub(r'^_(\w+)=',r'\1=',file.read_text(),flags=re.M))
        for file in (source/'colorschemes').glob('*.desktop.in'):
            destination=install/'usr/share/xfce4/terminal/colorschemes'/file.name.removesuffix('.in');destination.parent.mkdir(parents=True,exist_ok=True);destination.write_text(file.read_text())
        for file in (source/'icons').rglob('*.png'):
            destination=install/'usr/share/icons/hicolor'/file.parent.name/'apps'/file.name;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,destination)
        for file in (source/'icons/scalable').glob('*.svg'):
            destination=install/'usr/share/icons/hicolor/scalable/apps'/file.name;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,destination)
        saved=install/'build-metadata/terminal';shutil.copytree(generated,saved,ignore=shutil.ignore_patterns('*.o','probe.c'))
        licenses=install/'usr/share/licenses'/source.parent.name;licenses.mkdir(parents=True,exist_ok=True)
        for file in [*source.glob('COPYING*'),*source.glob('LICENSE*')]:
            if file.is_file():shutil.copyfile(file,licenses/file.name)
        defined=set();undefined=set()
        for file in install.rglob('*.a'):defined.update(definitions(file));undefined.update(symbols(file)[1])
        a.imports.write_text('\n'.join(sorted(undefined-defined-{'___dso_handle'}))+'\n');pack(install,a.output)
        a.report.write_text(json.dumps({'architecture':a.arch,'programs':{'xfce4-terminal':{'sources':records,'archive_path':str(archive.relative_to(install)),'archive_sha256':digest(archive),'exports':sorted(public),'imports':sorted(imported-definitions(archive)),'weak_imports':sorted(weak),'install_path':'/usr/bin/xfce4-terminal'}},'libraries':{},'modules':{},'bundle_sha256':digest(a.output),'runtime_checked':False},indent=2)+'\n')
        print(f'Native Xfce terminal {a.arch}: {len(records)} Mach-O objects')
if __name__=='__main__':main()
