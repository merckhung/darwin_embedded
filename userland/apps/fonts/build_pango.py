#!/usr/bin/env python3
"""Build real Pango/FriBidi/Thai/trie inventories inside Bazel actions."""
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
from native_components import run,symbols
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"glib"))
from build_dependencies import definitions,digest
from build_cairo import merge_archive

TEXT_RELEASES={"pango":"1.58.2","fribidi":"1.0.17","libthai":"0.1.30","libdatrie":"0.2.14"}


def meson_list(recipe,name):
    match=re.search(r"^\s*"+re.escape(name)+r"\s*=\s*(?:files\()?\[(.*?)\]",recipe,re.M|re.S)
    if not match:
        match=re.search(r"^\s*"+re.escape(name)+r"\s*=\s*files\((.*?)\)",recipe,re.M|re.S)
    if not match:raise ValueError("missing original text inventory: "+name)
    return re.findall(r"'([^']+)'",match[1])


def make_list(path,name):
    text=path.read_text().replace("\\\n"," ")
    match=re.search(r"^"+re.escape(name)+r"\s*=\s*(.*)$",text,re.M)
    if not match:raise ValueError("missing original Make inventory: "+name)
    return match[1].split()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","sdk","cairo","gio","output","imports","report","generated"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    args=parser.parse_args();root=args.root.resolve()
    sources={name:root/"src/Applications/text"/name/release for name,release in TEXT_RELEASES.items()}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-pango-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();os.environ["TMPDIR"]=str(work)
        sdk,install,generated=work/"sdk",work/"install",work/"generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive:archive.extractall(sdk,filter="data")
        for bundle in (args.cairo,args.gio):merge_archive(bundle,install)
        if json.loads((sdk/"DarwinSDK.json").read_text())["architecture"]!=args.arch:raise ValueError("Pango SDK CPU mismatch")
        resource=run(["clang-20","-print-resource-dir"]).strip()
        flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC","-ffreestanding","-fno-builtin",
               "-fno-stack-protector","-fno-strict-aliasing","-nostdinc","-D_DARWIN_C_SOURCE=1","-D__BSD_VISIBLE=1","-D_GNU_SOURCE=1",
               "-D_FORTIFY_SOURCE=0","-DHAVE_CONFIG_H","-Werror=implicit-function-declaration","-Werror=incompatible-pointer-types",
               "-Wno-availability","-Wno-deprecated-declarations","-isystem",sdk/"usr/include","-isystem",resource+"/include",
               "-I",install/"usr/include","-I",install/"usr/include/glib-2.0","-I",install/"usr/include/freetype2",
               "-I",install/"usr/include/harfbuzz","-I",install/"usr/include/cairo"]
        flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
        features,records={},{}
        def probe(name,text,required=False):
            path=generated/"feature.c";path.write_text(text)
            result=subprocess.run([*map(str,flags),"-Werror","-c",str(path),"-o",str(generated/"feature.o")],capture_output=True)
            features[name]=result.returncode==0
            if required and result.returncode:raise ValueError(name+":\n"+result.stderr.decode())
            return result.returncode==0
        def configuration(directory,values):
            directory.mkdir(parents=True,exist_ok=True)
            (directory/"config.h").write_text("/* Native Darwin target declarations. */\n"+"".join(f"#define {key} {value}\n" for key,value in sorted(values.items())))
        def compile_library(name,files,options=(),compile_copies=None):
            jobs=[(path,generated/(name+f"_{index:03d}.o")) for index,path in enumerate(files)]
            def compile_one(job):
                path,obj=job
                compiled=(compile_copies or {}).get(path,path)
                try:run([*flags,*options,"-c",compiled,"-o",obj])
                except RuntimeError as error:return {"error":str(error)}
                magic,cpu,_,kind=struct.unpack_from("<4I",obj.read_bytes())
                if (magic,cpu,kind)!=(0xfeedfacf,0x100000c if args.arch=="arm64" else 0x1000007,1):raise ValueError("non-native text object")
                return {"source":"generated/"+str(path.relative_to(generated)) if path.is_relative_to(generated) else str(path.relative_to(root)),
                        "source_sha256":digest(path),"compiled_source_sha256":digest(compiled),"member":obj.name,"object_sha256":digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:entries=list(pool.map(compile_one,jobs))
            errors=[entry["error"] for entry in entries if "error" in entry]
            if errors:raise ValueError(name+" compilation failed:\n"+"\n".join(errors))
            archive=install/"usr/lib"/(name+".a")
            run(["llvm-ar-20","--format=darwin","rcsD",archive,*[obj for _,obj in jobs]])
            public,imports,weak=symbols(archive)
            records[name]={"sources":entries,"exports":sorted(public),"imports":sorted(imports-definitions(archive)),"weak_imports":sorted(weak)}
        probe("Darwin_LP64","#include <stdint.h>\n#include <wchar.h>\n#include <stdatomic.h>\n"
              "_Static_assert(__APPLE__ && __MACH__ && sizeof(void*)==8 && sizeof(long)==8 && sizeof(int)==4 && sizeof(wchar_t)==4,\"Darwin ABI\");\n"
              "_Static_assert(ATOMIC_INT_LOCK_FREE==2 && ATOMIC_POINTER_LOCK_FREE==2,\"native atomics\");\n",True)
        common={}
        for header in ("stdio.h","stdlib.h","stdint.h","inttypes.h","limits.h","string.h","strings.h","memory.h","unistd.h","sys/types.h","sys/stat.h","sys/times.h","wchar.h","dlfcn.h"):
            macro="HAVE_"+re.sub(r"[^A-Z0-9]","_",header.upper())
            if probe(macro,"#include <"+header+">\n"):common[macro]=1
        for name in ("memmove","memset","strdup","malloc"):
            if probe("HAVE_"+name.upper(),"#include <stdlib.h>\n#include <string.h>\nvoid *f=(void*)&"+name+";\n"):common["HAVE_"+name.upper()]=1
        common.update(STDC_HEADERS=1,HAVE_STRINGIZE=1,SIZEOF_INT=4,SIZEOF_SHORT=2,SIZEOF_VOID_P=8,SIZEOF_WCHAR_T=4)
        # Trie files have their own original portable byte-order encoding.
        datrie=sources["libdatrie"];datrie_config=generated/"datrie"
        configuration(datrie_config,common)
        include=install/"usr/include/datrie";include.mkdir()
        for header in ("typedefs.h","triedefs.h","alpha-map.h","trie.h"):shutil.copyfile(datrie/"datrie"/header,include/header)
        files=[datrie/"datrie"/name for name in make_list(datrie/"datrie/Makefile.am","libdatrie_la_SOURCES") if name.endswith(".c")]
        compile_library("libdatrie",files,["-I",datrie_config,"-I",datrie])
        # Compile the original host dictionary tool; only its generated data
        # goes into the target bundle, never this host executable.
        host=generated/"dictionary";configuration(host,{**common,"HAVE_LANGINFO_CODESET":1,"VERSION":'"0.2.14"'})
        run(["clang-20","-std=gnu11","-O2","-DHAVE_CONFIG_H","-I",host,"-I",datrie,
             datrie/"tools/trietool.c",*files,"-o",host/"trietool"])
        thai=sources["libthai"]
        dictionary=host/"tdict.txt"
        words=make_list(thai/"data/Makefile.am","TDICT_SRC")
        dictionary.write_bytes(b"".join(line+b"\n" for line in sorted({line for name in words
            for line in (thai/"data"/Path(name).name).read_bytes().splitlines()})))
        shutil.copyfile(thai/"data/thbrk.abm",host/"thbrk.abm")
        subprocess.run([str(host/"trietool"),"thbrk","add-list","-e","utf-8",str(dictionary)],cwd=host,check=True,capture_output=True)
        target_dict=install/"usr/share/libthai/thbrk.tri";target_dict.parent.mkdir(parents=True);shutil.copyfile(host/"thbrk.tri",target_dict)
        for header in (thai/"include").rglob("*.h"):
            destination=install/"usr/include"/header.relative_to(thai/"include");destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(header,destination)
        subdirs=make_list(thai/"src/Makefile.am","SUBDIRS")
        files=[thai/"src/libthai.c"]
        for subdir in subdirs:
            if subdir=="utils":continue
            files += [thai/"src"/subdir/name for name in make_list(thai/"src"/subdir/"Makefile.am","lib"+subdir+"_la_SOURCES") if name.endswith(".c")]
        compile_library("libthai",files,["-I",thai/"include","-I",thai/"src",'-DDICT_DIR="/usr/share/libthai"'])
        fribidi=sources["fribidi"];fribidi_config=generated/"fribidi"
        configuration(fribidi_config,{**common,"FRIBIDI_NO_DEPRECATED":0})
        include=install/"usr/include/fribidi";include.mkdir()
        recipe=(fribidi/"lib/meson.build").read_text()
        for header in meson_list(recipe,"fribidi_headers"):shutil.copyfile(fribidi/"lib"/header,include/header)
        values={"configure_input":"Configured by Bazel from original template","PACKAGE":"fribidi","PACKAGE_NAME":"GNU FriBidi",
                "PACKAGE_BUGREPORT":"https://github.com/fribidi/fribidi/issues/new","FRIBIDI_VERSION":"1.0.17",
                "FRIBIDI_MAJOR_VERSION":1,"FRIBIDI_MINOR_VERSION":0,"FRIBIDI_MICRO_VERSION":17,"FRIBIDI_INTERFACE_VERSION":4,"SIZEOF_INT":4,
                "FRIBIDI_MSVC_BUILD_PLACEHOLDER":"#undef FRIBIDI_BUILT_WITH_MSVC"}
        text=re.sub(r"@(\w+)@",lambda m:str(values[m[1]]),(fribidi/"lib/fribidi-config.h.in").read_text())
        (include/"fribidi-config.h").write_text(text)
        # The archive's pregenerated tables are Unicode 14, while its UCD
        # inputs are Unicode 18. Generate with the original tools, and compile
        # private copies so quoted includes cannot select those stale tables.
        fresh_lib=fribidi_config/"lib";shutil.copytree(fribidi/"lib",fresh_lib)
        host_flags=["clang-20","-std=gnu11","-O2","-UHAVE_CONFIG_H","-DHAVE_STRINGIZE","-DHAVE_STDLIB_H=1",
                    "-DHAVE_STRING_H=1","-DHAVE_STRINGS_H=1","-DSTDC_HEADERS=1","-I",include,"-I",fresh_lib,"-I",fribidi/"gen.tab"]
        run([*host_flags,fribidi/"gen.tab/gen-unicode-version.c","-o",fribidi_config/"gen-unicode-version"])
        unicode_header=run([fribidi_config/"gen-unicode-version",fribidi/"gen.tab/unidata/ReadMe.txt",
                           fribidi/"gen.tab/unidata/BidiMirroring.txt","gen-unicode-version"])
        if '#define FRIBIDI_UNICODE_VERSION "18.0.0"' not in unicode_header:raise ValueError("FriBidi generated Unicode version mismatch")
        (fresh_lib/"fribidi-unicode-version.h").write_text(unicode_header)
        (include/"fribidi-unicode-version.h").write_text(unicode_header)
        gen_recipe=(fribidi/"gen.tab/meson.build").read_text()
        tabs=re.search(r"^tabs = \[(.*?)^\]",gen_recipe,re.M|re.S)
        if not tabs or "COMPRESSION='2'" not in gen_recipe:raise ValueError("FriBidi generator recipe changed")
        for name,entries in re.findall(r"\['([^']+)', files\((.*?)\)\]",tabs[1]):
            tool=fribidi_config/("gen-"+name+"-tab")
            run([*host_flags,fribidi/"gen.tab"/(tool.name+".c"),fribidi/"gen.tab/packtab.c","-o",tool])
            inputs=[fribidi/"gen.tab"/path for path in re.findall(r"'([^']+)'",entries)]
            (fresh_lib/(name+".tab.i")).write_text(run([tool,"2",*inputs,tool.name]))
        with tarfile.open(args.generated,"w") as archive:
            for path in sorted([*fresh_lib.glob("*.tab.i"),fresh_lib/"fribidi-unicode-version.h"]):
                info=archive.gettarinfo(path,path.name);info.uid=info.gid=info.mtime=0;info.uname=info.gname=""
                with path.open("rb") as stream:archive.addfile(info,stream)
        files=[fribidi/"lib"/name for name in meson_list(recipe,"fribidi_sources")]
        compile_library("libfribidi",files,["-I",fribidi_config,"-I",include,"-I",fresh_lib,"-fvisibility=hidden"],
                        {path:fresh_lib/path.name for path in files})
        pango=sources["pango"];pango_config=generated/"pango"
        values={"PANGO_BINARY_AGE":5802,"PANGO_INTERFACE_AGE":2,"PANGO_VERSION_MAJOR":1,"PANGO_VERSION_MINOR":58,"PANGO_VERSION_MICRO":2,
                "_PANGO_EXTERN":'__attribute__((visibility("default"))) extern',"HAVE_LIBTHAI":1,"HAVE_TH_BRK_FIND_BREAKS":1,
                "HAVE_FREETYPE":1,"HAVE_CAIRO":1,"HAVE_CAIRO_FREETYPE":1,"HAVE_CAIRO_PNG":1,"HAVE_CAIRO_PS":1,"HAVE_CAIRO_PDF":1,"HAVE_CAIRO_XLIB":1}
        configuration(pango_config,values)
        include=install/"usr/include/pango-1.0/pango";include.mkdir(parents=True)
        recipe=(pango/"pango/meson.build").read_text()
        groups={"libpango-1.0":meson_list(recipe,"pango_sources"),
                "libpangoft2-1.0":sum((meson_list(recipe,name) for name in ("pangofc_public_sources","pangoot_public_sources","pangoft2_public_sources")),[]),
                "libpangocairo-1.0":meson_list(recipe,"pangocairo_sources")}
        match=re.search(r"if pango_font_backends.contains\('freetype'\)\s+ pangocairo_sources \+= \[(.*?)\]",recipe,re.S)
        if not match:raise ValueError("missing original Pango Cairo FT source selection")
        groups["libpangocairo-1.0"]+=re.findall(r"'([^']+)'",match[1])
        headers=sum((meson_list(recipe,name) for name in ("pango_headers","pangofc_headers","pangoot_headers","pangoft2_headers","pangocairo_headers")),[])
        for header in headers+["pango-version-macros.h"]:shutil.copyfile(pango/"pango"/header,include/header)
        text=(pango/"pango/pango-features.h.meson").read_text()
        text=re.sub(r"^#mesondefine (\w+)$",lambda m:f"#define {m[1]} {values[m[1]]}",text,flags=re.M)
        text=re.sub(r"@(\w+)@",lambda m:str(values[m[1]]),text);(include/"pango-features.h").write_text(text)
        generator=pango_config/"glib-mkenums"
        generator.write_text((root/"src/Applications/glib/glib/2.90.1/gobject/glib-mkenums.in").read_text().replace("@PYTHON@","python3").replace("@VERSION@","2.90.1"))
        for suffix in ("h","c"):
            target=pango_config/("pango-enum-types."+suffix)
            with target.open("w") as stream:subprocess.run(["python3",str(generator),"--template",str(pango/"pango"/("pango-enum-types."+suffix+".template")),
                *[str(pango/"pango"/header) for header in meson_list(recipe,"pango_headers")]],stdout=stream,check=True)
        shutil.copyfile(pango_config/"pango-enum-types.h",include/"pango-enum-types.h")
        options=["-I",pango_config,"-I",include,"-I",include.parent,"-I",install/"usr/include/fribidi","-I",pango/"pango","-I",pango,
                 "-DPANGO_COMPILATION","-DPANGO_DISABLE_DEPRECATION_WARNINGS","-DG_LOG_USE_STRUCTURED=1",'-DG_LOG_DOMAIN="Pango"',
                 '-DSYSCONFDIR="/etc"','-DLIBDIR="/usr/lib"',"-fvisibility=hidden"]
        for name,files in groups.items():
            paths=[pango/"pango"/name for name in files]
            if name=="libpango-1.0":paths.append(pango_config/"pango-enum-types.c")
            compile_library(name,paths,options)
        for name,directory in sources.items():
            destination=install/"usr/share/licenses"/name;destination.mkdir(parents=True,exist_ok=True)
            for path in directory.glob("COPYING*"):shutil.copyfile(path,destination/path.name)
        defined,undefined=set(),set()
        for archive in (install/"usr/lib").glob("*.a"):defined|=definitions(archive);undefined|=symbols(archive)[1]
        for obj in (install/"usr/libexec").glob("*.o"):undefined|=symbols(obj)[1]
        requests=sorted(undefined-defined-{"___dso_handle"});args.imports.write_text("\n".join(requests)+"\n")
        with tarfile.open(args.output,"w") as archive:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    info=archive.gettarinfo(path,str(path.relative_to(install)));info.uid=info.gid=info.mtime=0;info.uname=info.gname=""
                    with path.open("rb") as stream:archive.addfile(info,stream)
        args.report.write_text(json.dumps({"architecture":args.arch,"versions":TEXT_RELEASES,"libraries":records,"configuration":values,"sdk_features":features,
            "dictionary_sha256":digest(target_dict),"dictionary_words":len(dictionary.read_bytes().splitlines()),"original_fribidi_tables":{path.name:digest(path) for path in (fribidi/"lib").glob("*.tab.i")},
            "generated_fribidi_tables":{path.name:digest(path) for path in fresh_lib.glob("*.tab.i")},"fribidi_unicode_version":"18.0.0",
            "generated_sources":{"pango-enum-types.c":digest(pango_config/"pango-enum-types.c")},"system_requests":requests,"bundle_sha256":digest(args.output),"runtime_checked":False,
            "disabled_optional_backends":["Xft (not built)","CoreText/CoreFoundation frameworks","Windows backends","sysprof tracing","GObject introspection tooling"]},indent=2)+"\n")
        print(f"Native Pango/text {args.arch}: {sum(len(entry['sources']) for entry in records.values())} genuine Mach-O objects, {len(dictionary.read_bytes().splitlines())} real Thai dictionary words")


if __name__=="__main__":main()
