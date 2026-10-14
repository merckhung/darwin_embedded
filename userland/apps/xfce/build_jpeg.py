#!/usr/bin/env python3
"""Compile the complete original portable libjpeg-turbo library through Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tarfile
import tempfile
from build_base import run, symbols, digest, definitions, pack

VERSION = "3.2.0"
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","sdk","output","imports","report"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    args=parser.parse_args();root=args.root.resolve();args.output.parent.mkdir(parents=True,exist_ok=True)
    source=root/"src/Applications/xfce-prerequisites/libjpeg-turbo"/VERSION
    manifest=json.loads((source/"UPSTREAM.json").read_text())
    for name,expected in manifest["upstream_files_sha256"].items():
        if digest(source/name)!=expected:raise ValueError("modified original JPEG source: "+name)
    with tempfile.TemporaryDirectory(prefix=".native-jpeg-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();sdk,install,generated=work/"sdk",work/"install",work/"generated";generated.mkdir()
        os.environ["TMPDIR"]=str(work)
        with tarfile.open(args.sdk) as archive:archive.extractall(sdk,filter="data")
        if json.loads((sdk/"DarwinSDK.json").read_text())["architecture"]!=args.arch:raise ValueError("JPEG SDK architecture mismatch")
        values={"JPEG_LIB_VERSION":"62","VERSION":VERSION,"LIBJPEG_TURBO_VERSION_NUMBER":"3002000","BUILD":"20260630",
                "HIDDEN":"__attribute__((visibility(\"hidden\")))","INLINE":"__inline__ __attribute__((always_inline))","THREAD_LOCAL":"_Thread_local",
                "CMAKE_PROJECT_NAME":"libjpeg-turbo","SIZE_T":"8","COPYRIGHT_YEAR":"1991-2026","SIMD_ARCHITECTURE":"none"}
        enabled={"C_ARITH_CODING_SUPPORTED","D_ARITH_CODING_SUPPORTED","HAVE_BUILTIN_CTZL"}
        for name in ("jconfig.h","jconfigint.h","jversion.h"):
            text=(source/"src"/(name+".in")).read_text()
            text=re.sub(r"@(\w+)@",lambda m:values[m[1]],text)
            text=re.sub(r"^#cmakedefine\s+(\w+)([^\n]*)",lambda m:"#define "+m[1]+m[2] if m[1] in enabled else "/* #undef "+m[1]+" */",text,flags=re.M)
            (generated/name).write_text(text)
        recipe=(source/"CMakeLists.txt").read_text();match=re.search(r"set\(JPEG_SOURCES\s+(.*?)\)",recipe,re.S)
        if not match:raise ValueError("original JPEG inventory missing")
        files=[source/name for name in match[1].split()]
        # Original configure defaults enable both arithmetic implementations.
        files += [source/"src"/name for name in ("jaricom.c","jcarith.c","jdarith.c")]
        resource=run(["clang-20","-print-resource-dir"]).strip()
        flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC","-ffreestanding","-fno-builtin","-fno-stack-protector","-nostdinc",
               "-D_DARWIN_C_SOURCE=1","-D__BSD_VISIBLE=1","-D_FORTIFY_SOURCE=0","-Werror=implicit-function-declaration","-Wno-availability",
               "-isystem",resource+"/include","-isystem",sdk/"usr/include","-I",generated,"-I",source/"src"]
        flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
        jobs=[(file,generated/f"jpeg-{index:03d}.o") for index,file in enumerate(files)]
        def compile_one(job):
            file,obj=job
            try:run([*flags,"-c",file,"-o",obj])
            except RuntimeError as error:return {"error":str(error)}
            if struct.unpack_from("<4I",obj.read_bytes())[1::2]!=(0x100000c if args.arch=="arm64" else 0x1000007,1):raise ValueError("foreign JPEG object")
            return {"source":str(file.relative_to(root)),"source_sha256":digest(file),"member":obj.name,"object_sha256":digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(compile_one,jobs))
        errors=[record["error"] for record in records if "error" in record]
        if errors:raise ValueError("native JPEG compilation failed:\n"+"\n".join(errors))
        archive=install/"usr/lib/libjpeg.a";archive.parent.mkdir(parents=True,exist_ok=True)
        run(["llvm-ar-20","--format=darwin","rcsD",archive,*[obj for _,obj in jobs]])
        include=install/"usr/include";include.mkdir(parents=True,exist_ok=True)
        for name in ("jpeglib.h","jmorecfg.h","jerror.h"):shutil.copyfile(source/"src"/name,include/name)
        shutil.copyfile(generated/"jconfig.h",include/"jconfig.h")
        license=install/"usr/share/licenses/libjpeg-turbo";license.mkdir(parents=True,exist_ok=True)
        for name in ("LICENSE.md","README.ijg"):shutil.copyfile(source/name,license/name)
        public,imported,weak=symbols(archive);requests=sorted(imported-definitions(archive)-{"___dso_handle"});args.imports.write_text("\n".join(requests)+"\n");pack(install,args.output)
        args.report.write_text(json.dumps({"architecture":args.arch,"libraries":{"libjpeg":{"sources":records,"exports":sorted(public),"imports":requests,"weak_imports":sorted(weak),"archive_sha256":digest(archive)}},
            "configuration":values,"bundle_sha256":digest(args.output),"runtime_checked":False,"scope":"Complete original portable 8/12/16-bit JPEG library; SIMD backend not selected."},indent=2)+"\n")
        print(f"Native JPEG {args.arch}: {len(records)} original Mach-O objects")
if __name__=="__main__":main()
