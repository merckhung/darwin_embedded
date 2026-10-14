#!/usr/bin/env python3
"""Build original ICE/SM session protocols with the native Darwin socket ABI."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

from build_base import inventory, run, symbols, digest, definitions, pack
RELEASES={"libICE":"1.1.2","libSM":"1.2.6"}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","sdk","dependencies","output","imports","report"):parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    args=parser.parse_args();root=args.root.resolve();args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-ice-sm-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();sdk,install=work/"sdk",work/"install"
        for path,target in ((args.sdk,sdk),(args.dependencies,install)):
            with tarfile.open(path) as archive:archive.extractall(target,filter="data")
        resource=run(["clang-20","-print-resource-dir"]).strip()
        flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC","-ffreestanding","-fno-builtin","-fno-stack-protector",
               "-nostdinc","-D_DARWIN_C_SOURCE=1","-D_FORTIFY_SOURCE=0","-DHAVE_CONFIG_H","-Werror=implicit-function-declaration","-Werror=incompatible-pointer-types","-Wno-availability",
               "-isystem",resource+"/include","-isystem",sdk/"usr/include","-isystem",sdk/"usr/local/include","-I",install/"usr/include"]
        flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
        libraries,configurations={},{}
        for name,version in RELEASES.items():
            source=root/"src/Applications/xfce-prerequisites"/name/version;generated=work/name;generated.mkdir()
            manifest=json.loads((source/"UPSTREAM.json").read_text())
            for file,expected in manifest["upstream_files_sha256"].items():
                if digest(source/file)!=expected:raise ValueError("modified session source: "+file)
            for file in (source/"include").rglob("*.h"):
                target=install/"usr/include"/file.relative_to(source/"include");target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,target)
            values={"PACKAGE_VERSION":json.dumps(version),"PACKAGE_STRING":json.dumps(name+" "+version),"PACKAGE_BUGREPORT":'"https://gitlab.freedesktop.org/xorg/lib"',
                    "STDC_HEADERS":1,"BSD44SOCKETS":1,"HAVE_SOCKLEN_T":1,"IPv6":1,"UNIXCONN":1,"TCPCONN":1,"ICE_t":1,"TRANS_CLIENT":1,"TRANS_SERVER":1}
            tested={}
            for header in ("dlfcn.h","inttypes.h","stdint.h","stdio.h","stdlib.h","strings.h","string.h","sys/stat.h","sys/types.h","unistd.h","wchar.h"):
                file=generated/"probe.c";file.write_text('#include <'+header+'>\n')
                result=subprocess.run([*map(str,flags),"-Werror","-c",str(file),"-o",str(generated/"probe.o")],capture_output=True)
                key="HAVE_"+re.sub(r"[^A-Z0-9]","_",header.upper());tested[key]=result.returncode==0
                if not result.returncode:values[key]=1
            for function in ("arc4random_buf","getentropy","asprintf","strcasecmp","strlcpy"):
                file=generated/"probe.c";file.write_text('#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\n#include <strings.h>\n#include <unistd.h>\nvoid *p=(void*)&'+function+';\n')
                result=subprocess.run([*map(str,flags),"-Werror","-c",str(file),"-o",str(generated/"probe.o")],capture_output=True)
                tested["HAVE_"+function.upper()]=result.returncode==0
                if not result.returncode:values["HAVE_"+function.upper()]=1
            (generated/"config.h").write_text("".join(f"#define {key} {value}\n" for key,value in sorted(values.items())))
            local=["-I",generated,"-I",source/"src","-I",source/"include"]
            jobs=[(source/"src"/file,generated/f"session_{i:02d}.o") for i,file in enumerate(inventory(source/"src/Makefile.am",name+"_la_SOURCES")) if file.endswith('.c')]
            def compile_one(job):
                file,obj=job;run([*flags,*local,"-c",file,"-o",obj])
                if struct.unpack_from("<4I",obj.read_bytes())[1::2]!=(0x100000c if args.arch=="arm64" else 0x1000007,1):raise ValueError("foreign session object")
                return {"source":str(file.relative_to(root)),"source_sha256":digest(file),"member":obj.name,"object_sha256":digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
            archive=install/"usr/lib"/(name+".a");run(["llvm-ar-20","--format=darwin","rcsD",archive,*[obj for _,obj in jobs]])
            public,imported,weak=symbols(archive)
            libraries[name]={"sources":records,"exports":sorted(public),"imports":sorted(imported-definitions(archive)),"weak_imports":sorted(weak),"archive_sha256":digest(archive)}
            configurations[name]={"values":values,"header_probes":tested,"client_id":"Original hostname/time/PID sequence fallback; no UUID emulation"}
            license=install/"usr/share/licenses"/name;license.mkdir(parents=True,exist_ok=True);shutil.copyfile(source/"COPYING",license/"COPYING")
        defined,undefined=set(),set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install/"usr/libexec/xorg"):continue
            defined|=definitions(path);undefined|=symbols(path)[1]
        args.imports.write_text("\n".join(sorted(undefined-defined-{"___dso_handle"}))+"\n");pack(install,args.output)
        args.report.write_text(json.dumps({"architecture":args.arch,"libraries":libraries,"configuration":configurations,"bundle_sha256":digest(args.output),"runtime_checked":False},indent=2)+"\n")
        print(f"Native ICE/SM {args.arch}: {sum(len(v['sources']) for v in libraries.values())} original Mach-O session objects")

if __name__=="__main__":main()
