#!/usr/bin/env python3
"""Build original Xfce utility/configuration libraries and services with Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"scripts"))
from native_components import run, symbols
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"glib"))
from build_dependencies import digest, definitions
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"x11"))
from build_clients import pack

RELEASES={"libxfce4util":"4.20.1","xfconf":"4.20.0"}

def inventory(recipe,variable):
    text=recipe.read_text().replace("\\\n"," ")
    match=re.search(r"^"+re.escape(variable)+r"\s*=\s*(.*)$",text,re.M)
    if not match:raise ValueError("missing original inventory: "+variable)
    return match[1].split()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","sdk","dependencies","output","imports","report"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    args=parser.parse_args();root=args.root.resolve();base=root/"src/Applications/xfce"
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xfce-base-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();os.environ["TMPDIR"]=str(work)
        sdk,install,generated=work/"sdk",work/"install",work/"generated";generated.mkdir()
        for path,target in ((args.sdk,sdk),(args.dependencies,install)):
            with tarfile.open(path) as archive:archive.extractall(target,filter="data")
        resource=run(["clang-20","-print-resource-dir"]).strip()
        flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC","-ffreestanding","-fno-builtin",
               "-fno-stack-protector","-fno-strict-aliasing","-nostdinc","-D_DARWIN_C_SOURCE=1","-D_FORTIFY_SOURCE=0","-DHAVE_CONFIG_H",
               "-Werror=implicit-function-declaration","-Werror=incompatible-pointer-types","-Wno-availability",
               "-isystem",resource+"/include","-isystem",sdk/"usr/include","-isystem",sdk/"usr/local/include","-I",install/"usr/include",
               "-I",install/"usr/include/glib-2.0","-I",install/"usr/include/gio-unix-2.0","-I",install/"usr/include/xfce4",
               "-I",install/"usr/include/xfce4/xfconf-0"]
        flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
        libraries,programs,configurations,features={}, {}, {}, {}
        for project,version in RELEASES.items():
            source=base/project/version;configdir=generated/project;configdir.mkdir()
            manifest=json.loads((source/"UPSTREAM.json").read_text())
            for name,expected in manifest["upstream_files_sha256"].items():
                if digest(source/name)!=expected:raise ValueError("modified upstream Xfce file: "+name)
            config={"PACKAGE":json.dumps(project),"PACKAGE_NAME":json.dumps(project),"PACKAGE_VERSION":json.dumps(version),
                    "PACKAGE_STRING":json.dumps(project+" "+version),"VERSION":json.dumps(version),"VERSION_FULL":json.dumps(version),"VERSION_SHORT":'"4.20"',
                    "GETTEXT_PACKAGE":json.dumps(project),"ENABLE_NLS":1,"STDC_HEADERS":1,"LOCALEDIR":'"/usr/share/locale"',
                    "PACKAGE_BUGREPORT":json.dumps("https://gitlab.xfce.org/xfce/"+project),
                    "PACKAGE_LOCALE_DIR":'"/usr/share/locale"',"BINDIR":'"/usr/bin"',"DATADIR":'"/usr/share"',"SYSCONFDIR":'"/etc"',"PREFIX":'"/usr"'}
            tests={}
            def probe(key,text,required=False):
                path=configdir/"feature.c";path.write_text(text)
                result=subprocess.run([*map(str,flags),"-Werror","-c",str(path),"-o",str(configdir/"feature.o")],capture_output=True)
                tests[key]=result.returncode==0
                if required and result.returncode:raise ValueError(key+":\n"+result.stderr.decode())
                if not result.returncode:config[key]=1
            for header in ("errno.h","err.h","dlfcn.h","fcntl.h","grp.h","pwd.h","limits.h","locale.h","signal.h","stdarg.h","stdint.h","stdio.h","stdlib.h","string.h","strings.h","sys/stat.h","sys/types.h","sys/utsname.h","time.h","unistd.h","wchar.h","libintl.h"):
                probe("HAVE_"+re.sub(r"[^A-Z0-9]","_",header.upper()),'#include <'+header+'>\n')
            headers='#include <unistd.h>\n#include <pwd.h>\n#include <locale.h>\n#include <libintl.h>\n'
            for function in ("gethostname","getpwnam","setlocale","gettext","dcgettext","bind_textdomain_codeset","fsync","fdatasync"):
                probe("HAVE_"+function.upper(),headers+"void *p=(void*)&"+function+";\n")
            if project=="xfconf":
                config.update(XFCONF_SERVICE_NAME_PREFIX='"org.xfce"',XFCONF_SERVICE_PATH_PREFIX='"/org/xfce"',BUILD_XFCONF_BACKEND_PERCHANNEL_XML=1)
                for name,ctype,alignment in (("GBOOLEAN","gboolean",4),("GCHAR","gchar",1),("GUCHAR","guchar",1),("GDOUBLE","gdouble",8),("GFLOAT","gfloat",4),
                    ("GINT16","gint16",2),("GUINT16","guint16",2),("GINT32","gint32",4),("GUINT32","guint32",4),("GINT64","gint64",8),("GUINT64","guint64",8),("GPOINTER","gpointer",8)):
                    probe("ALIGNOF_"+name,'#include <glib.h>\n_Static_assert(_Alignof('+ctype+f')=={alignment},"Darwin alignment");\n',True);config["ALIGNOF_"+name]=alignment
            (configdir/"config.h").write_text("/* Actual Darwin declarations and original feature choices. */\n"+"".join(f"#define {key} {value}\n" for key,value in sorted(config.items())))
            local=["-I",configdir,"-I",source,"-I",source/"common","-I",source/project]
            adaptations={}
            if project=="xfconf":
                original=source/"xfconf/xfconf.c";text=original.read_text()
                arguments="NULL,\n                                   NULL);"
                ending="\n    ++xfconf_refcnt;\n    return TRUE;\n}"
                if text.count(arguments)!=1 or text.count(ending)!=1:raise ValueError("original Xfconf proxy initialization changed")
                text=text.replace(arguments,"NULL,\n                                   error);")
                text=text.replace(ending,"\n    if (!gproxy) {\n        g_clear_object(&gdbus);\n        return FALSE;\n    }\n"+ending)
                compiled=configdir/"xfconf.c";compiled.write_text(text)
                adaptations[original]=compiled
            def build(name,paths,options=(),program=False):
                destination=install/"usr/libexec/xfce"/(name+".a") if program else install/"usr/lib"/(name+".a")
                destination.parent.mkdir(parents=True,exist_ok=True)
                jobs=[(path,configdir/f"{name}_{i:02d}.o") for i,path in enumerate(paths)]
                def compile_one(job):
                    path,obj=job
                    common_options=["-DLIBXFCONF_COMPILATION"] if project=="xfconf" and path.is_relative_to(source/"common") else []
                    compiled=adaptations.get(path,path)
                    run([*flags,*local,*options,*common_options,"-c",compiled,"-o",obj])
                    if struct.unpack_from("<4I",obj.read_bytes())[::3]!=(0xfeedfacf,1) or struct.unpack_from("<I",obj.read_bytes(),4)[0]!=(0x100000c if args.arch=="arm64" else 0x1000007):raise ValueError("foreign Xfce object")
                    return {"source":str(path.relative_to(root)) if path.is_relative_to(root) else "generated/"+str(path.relative_to(generated)),"source_sha256":digest(path),"member":obj.name,"object_sha256":digest(obj),
                            "compiled_sha256":digest(compiled),"adaptation":"Pass the real proxy construction error and reject an uninitialized NULL proxy; preserve upstream bytes" if path in adaptations else None}
                with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
                run(["llvm-ar-20","--format=darwin","rcsD",destination,*[obj for _,obj in jobs]])
                exported,imported,weak=symbols(destination)
                record={"sources":records,"exports":sorted(exported),"imports":sorted(imported-definitions(destination)),"weak_imports":sorted(weak),"archive_sha256":digest(destination)}
                (programs if program else libraries)[name]=record
            if project=="libxfce4util":
                include=install/"usr/include/xfce4/libxfce4util";include.mkdir(parents=True,exist_ok=True)
                for path in (source/"libxfce4util").glob("*.h"):shutil.copyfile(path,include/path.name)
                substitutions={"LIBXFCE4UTIL_VERSION_"+key:value for key,value in zip(("MAJOR","MINOR","MICRO"),version.split('.'),strict=True)}
                header=re.sub(r"@(\w+)@",lambda m:substitutions[m[1]],(source/"libxfce4util/libxfce4util-config.h.in").read_text())
                (include/"libxfce4util-config.h").write_text(header)
                local += ["-I",include]
                paths=[source/"libxfce4util"/name for name in inventory(source/"libxfce4util/Makefile.am","libxfce4util_la_SOURCES") if name.endswith('.c')]
                build("libxfce4util",paths,["-DLIBXFCE4UTIL_COMPILATION",'-DDEFAULT_XDG_CACHE_HOME="~/.cache"','-DDEFAULT_XDG_DATA_HOME="~/.local/share"',
                    '-DDEFAULT_XDG_CONFIG_HOME="~/.config"','-DDEFAULT_XDG_DATA_DIRS="/usr/share:/usr/local/share"','-DDEFAULT_XDG_CONFIG_DIRS="/etc/xdg:/etc/xfce4"',
                    '-DKIOSKDIR="/etc/xdg/xfce4/kiosk"','-DKIOSKDEF="ALL"'])
            else:
                include=install/"usr/include/xfce4/xfconf-0/xfconf";include.mkdir(parents=True,exist_ok=True)
                for name in inventory(source/"xfconf/Makefile.am","libxfconfinclude_HEADERS"):shutil.copyfile(source/"xfconf"/name,include/name)
                common=[source/"common"/name for name in ("xfconf-errors.c","xfconf-marshal.c","xfconf-gvaluefuncs.c","xfconf-gdbus-bindings.c")]
                paths=[source/"xfconf"/name for name in inventory(source/"xfconf/Makefile.am","libxfconf_0_la_SOURCES") if name.endswith('.c') and not name.startswith('$')]
                paths += [source/"common/xfconf-types.c",*common]
                build("libxfconf-0",paths,["-DLIBXFCONF_COMPILATION"])
                daemon=[source/"xfconfd"/name for name in inventory(source/"xfconfd/Makefile.am","xfconfd_SOURCES") if name.endswith('.c') and not name.startswith('$')]
                daemon += [source/"xfconfd/xfconf-backend-perchannel-xml.c",source/"common/xfconf-types.c",*common]
                build("xfconfd",daemon,["-DLIBXFCONF_COMPILATION"],True)
                query=[source/"xfconf-query"/name for name in inventory(source/"xfconf-query/Makefile.am","xfconf_query_SOURCES") if name.endswith('.c')]
                build("xfconf-query",query+[source/"common/xfconf-gvaluefuncs.c"],(),True)
                service=install/"usr/share/dbus-1/services/org.xfce.Xfconf.service";service.parent.mkdir(parents=True,exist_ok=True)
                service.write_text((source/"xfconfd/org.xfce.Xfconf.service.in").read_text().replace("@XFCONF_SERVICE_NAME_PREFIX@","org.xfce").replace("@HELPERDIR@","/usr/libexec"))
            configurations[project]=config;features[project]=tests
            license=install/"usr/share/licenses"/project;license.mkdir(parents=True,exist_ok=True);shutil.copyfile(source/"COPYING",license/"COPYING")
        defined,undefined=set(),set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install/"usr/libexec/xorg"):continue # server DDX belongs to the already linked Xorg program
            defined|=definitions(path);undefined|=symbols(path)[1]
        for path in (install/"usr/libexec").glob("*.o"):undefined|=symbols(path)[1]
        requests=sorted(undefined-defined-{"___dso_handle"});args.imports.write_text("\n".join(requests)+"\n")
        pack(install,args.output)
        report={"architecture":args.arch,"libraries":libraries,"programs":programs,"configuration":configurations,"sdk_features":features,
                "bundle_sha256":digest(args.output),"runtime_checked":False,"desktop_checked":False,"generated_sources":"Original release-provided visibility, marshal and GDBus sources; version header from original template"}
        args.report.write_text(json.dumps(report,indent=2)+"\n")
        print(f"Native Xfce base {args.arch}: {sum(len(r['sources']) for r in [*libraries.values(),*programs.values()])} Mach-O objects; real util, xfconf, XML daemon and query tool")

if __name__=="__main__":main()
