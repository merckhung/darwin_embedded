#!/usr/bin/env python3
"""Compile original GTK3 VTE and its bundled C++ helpers as native Mach-O."""
import argparse,json,os,re,shutil,struct,subprocess,tarfile,tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from build_base import run,digest,definitions,pack
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'cxx'))
from build_libraries import symbols

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','sdk','dependencies','cxx','source-archive','output','imports','report'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--arch',choices=('arm64','x86_64'),required=True);a=p.parse_args();root=a.root.resolve()
    original_source=root/'src/Applications/xfce-prerequisites/vte/0.84.1';manifest=json.loads((original_source/'UPSTREAM.json').read_text())
    if digest(a.source_archive)!=manifest['archive_sha256']:raise ValueError('VTE publisher archive digest mismatch')
    with tempfile.TemporaryDirectory(prefix='.native-vte-',dir=a.output.parent) as directory:
        work=Path(directory).resolve();sdk,install,cxx,generated=work/'sdk',work/'install',work/'cxx',work/'generated';generated.mkdir();os.environ['TMPDIR']=str(work)
        with tarfile.open(a.source_archive) as archive:archive.extractall(work/'upstream',filter='data')
        source=work/'upstream/vte-0.84.1'
        for name,expected in manifest['upstream_files_sha256'].items():
            if digest(source/name)!=expected:raise ValueError('modified original VTE source: '+name)
        for path,target in ((a.sdk,sdk),(a.dependencies,install),(a.cxx,cxx)):
            with tarfile.open(path) as archive:archive.extractall(target,filter='data')
        if json.loads((sdk/'DarwinSDK.json').read_text())['architecture']!=a.arch:raise ValueError('VTE SDK mismatch')
        resource=run(['clang-20','-print-resource-dir']).strip()
        flags=['clang++-20',f'--target={a.arch}-apple-macos26.5','-std=gnu++23','-O2','-fPIC','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdinc','-nostdinc++','-D_DARWIN_C_SOURCE=1','-D__BSD_VISIBLE=1','-D_FORTIFY_SOURCE=0','-DVTE_GTK=3','-DVTE_COMPILATION','-DG_LOG_DOMAIN="VTE"','-DLOCALEDIR="/usr/share/locale"','-DTERMINFODIR="/usr/share/terminfo"','-DFMT_HEADER_ONLY','-DFMT_USE_CONSTEVAL','-UPARSER_INCLUDE_NOP','-Wno-availability','-isystem',cxx/'usr/include/c++/v1','-isystem',resource+'/include','-isystem',sdk/'usr/include','-I',generated,'-I',source/'src']
        flags+=['-mcpu=cortex-a72','-mno-outline-atomics'] if a.arch=='arm64' else ['-march=penryn']
        flags+=['-I',generated/'vte','-I',source/'src/vte']
        for include in ('','glib-2.0','gio-unix-2.0','gtk-3.0','pango-1.0','gdk-pixbuf-2.0','atk-1.0','harfbuzz','freetype2','cairo','fribidi'):
            flags+=['-I',install/'usr/include'/include]
        for include in ('fmt/include','fast_float/include','simdutf/include','simdutf/src'):
            flags+=['-I',source/'subprojects'/include]
        values={'GETTEXT_PACKAGE':'"vte-2.91"','VERSION':'"0.84.1"','VERSION_MAJOR':0,'VERSION_MINOR':84,'VERSION_MICRO':1,'VTE_DEBUG':0,'WITH_A11Y':1,'WITH_FRIBIDI':1,'WITH_GNUTLS':0,'WITH_ICU':0,'WITH_TERMINFO':0,'WITH_SYSTEMD':0,'GLIB_VERSION_MIN_REQUIRED':'(G_ENCODE_VERSION(2,72))','GLIB_VERSION_MAX_ALLOWED':'(G_ENCODE_VERSION(2,90))'}
        probes={}
        for function,header in [('explicit_bzero','string.h'),('fdwalk','stdlib.h'),('pread','unistd.h'),('pwrite','unistd.h'),('strchrnul','string.h'),('close_range','unistd.h'),('round','math.h')]:
            file=generated/'probe.cc'
            declaration=f'double (*func)(double)=&{function};' if function=='round' else f'void *p=(void*)&{function};'
            file.write_text(f'#include <{header}>\n{declaration}\n')
            result=subprocess.run([*map(str,flags),'-Werror','-c',str(file),'-o',str(generated/'probe.o')],capture_output=True)
            key='HAVE_'+function.upper();values[key]=int(result.returncode==0);probes[key]=bool(values[key])
        (generated/'config.h').write_text(''.join(f'#define {name} {value}\n' for name,value in sorted(values.items())))
        (generated/'vte').mkdir()
        version=(source/'src/vte/vteversion.h.in').read_text()
        (generated/'vte/vteversion.h').write_text(re.sub(r'@(\w+)@',lambda m:{'VTE_MAJOR_VERSION':'0','VTE_MINOR_VERSION':'84','VTE_MICRO_VERSION':'1'}[m[1]],version))
        for name in ('modes.py','parser-seq.py'):run(['python3',source/'src'/name,'--destdir',generated])
        enums=[source/'src/vte'/name for name in ('vtedeprecated.h','vteenums.h')]
        for suffix in ('h','cc'):
            run(['glib-mkenums','--output',generated/'vte'/('vtetypebuiltins-gtk3.'+suffix),'--template',source/'src'/('vtetypebuiltins.'+suffix+'.template'),*enums])
        for suffix,kind in (('h','header'),('c','body')):
            (generated/('marshal.'+suffix)).write_text(run(['glib-genmarshal','--'+kind,'--internal','--valist-marshallers','--prefix=_vte_marshal',source/'src/marshal.list']))
        run(['glib-compile-resources','--generate-source','--c-name=_vte','--target='+str(generated/'vteresources.cc'),source/'src/vte.gresource.xml'])
        preprocessed=generated/'minifont.preprocessed'
        preprocessed.write_text(run([*flags,'-E','-fdirectives-only','-DMINIFONT_COVERAGE',source/'src/minifont.cc']))
        run(['python3',source/'src/minifont-coverage.py','--output',generated/'minifont-coverage-gtk3.inc',preprocessed])
        recipe=(source/'src/meson.build').read_text();groups={}
        for match in re.finditer(r'^(\w+_sources)\s*=\s*files\((.*?)\n\)',recipe,re.M|re.S):groups[match[1]]=re.findall(r"'([^']+)'",match[2])
        match=re.search(r'^libvte_common_sources\s*=\s*(.*?)\+ files\((.*?)\n\)',recipe,re.M|re.S)
        if not match:raise ValueError('original VTE common inventory missing')
        names=re.findall(r"'([^']+)'",match[2])
        for group in re.findall(r'\b\w+_sources\b',match[1]):names+=groups.get(group,[])
        names+=groups['libvte_gtk3_drawing_sources']+groups['a11y_gtk3_sources']
        files=[source/'src'/name for name in dict.fromkeys(names) if name.endswith(('.cc','.c'))]
        files += [generated/'marshal.c',generated/'vteresources.cc',generated/'vte/vtetypebuiltins-gtk3.cc',source/'subprojects/simdutf/src/simdutf.cpp']
        jobs=[(file,generated/f'vte-{i:03d}.o') for i,file in enumerate(files)]
        def compile_one(job):
            file,obj=job
            local_flags=flags
            if file.suffix=='.c':local_flags=['clang-20',*['-std=gnu11' if f=='-std=gnu++23' else f for f in flags[1:] if f!='-nostdinc++']]
            try:run([*local_flags,'-c',file,'-o',obj])
            except RuntimeError as error:return {'error':str(error)}
            if struct.unpack_from('<4I',obj.read_bytes())[1::2]!=(0x100000c if a.arch=='arm64' else 0x1000007,1):raise ValueError('foreign VTE object')
            is_generated=file.is_relative_to(generated)
            relative=str(file.relative_to(generated)) if is_generated else str(original_source.relative_to(root)/file.relative_to(source))
            return {'source':relative,'generated':is_generated,'source_sha256':digest(file),'member':obj.name,'object_sha256':digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
        errors=[r['error'] for r in records if 'error' in r]
        if errors:raise ValueError('VTE native compilation failed:\n'+'\n'.join(errors))
        archive=install/'usr/lib/libvte-2.91.a';run(['llvm-ar-20','--format=darwin','rcsD',archive,*[obj for _,obj in jobs]])
        public,imported,weak=symbols(archive)
        include=install/'usr/include/vte-2.91/vte';include.mkdir(parents=True,exist_ok=True)
        for file in (source/'src/vte').glob('*.h'):shutil.copyfile(file,include/file.name)
        for file in (generated/'vte').glob('*.h'):shutil.copyfile(file,include/file.name)
        saved=install/'build-metadata/vte';shutil.copytree(generated,saved,ignore=shutil.ignore_patterns('*.o','probe.cc','minifont.preprocessed'))
        licenses=install/'usr/share/licenses/vte';licenses.mkdir(parents=True,exist_ok=True)
        for file in source.glob('COPYING*'):shutil.copyfile(file,licenses/file.name)
        defined=set();undefined=set()
        for file in [*install.rglob('*.a'),*cxx.rglob('*.a')]:defined.update(definitions(file));undefined.update(symbols(file)[1])
        a.imports.write_text('\n'.join(sorted(undefined-defined-{'___dso_handle'}))+'\n');pack(install,a.output)
        a.report.write_text(json.dumps({'architecture':a.arch,'libraries':{'libvte-2.91':{'sources':records,'exports':sorted(public),'imports':sorted(imported-definitions(archive)),'weak_imports':sorted(weak),'archive_sha256':digest(archive)}},'configuration':values,'sdk_probes':probes,'bundle_sha256':digest(a.output),'runtime_checked':False},indent=2)+'\n')
        print(f'Native GTK3 VTE {a.arch}: {len(records)} Mach-O objects')
if __name__=='__main__':main()
