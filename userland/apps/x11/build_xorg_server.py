#!/usr/bin/env python3
"""Link the original Xorg server with native display/input and real System APIs."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"glib"))
from build_dependencies import digest, definitions
from build_clients import pack
from link_auth import image_metadata, verify_imports

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    parser.add_argument("--prepare",action="store_true")
    for name in ("root","sdk","core","display","input","runtime","runtime-report","base-imports","archive","imports","output","report","dyld"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--system",type=Path)
    parser.add_argument("--init-exports",type=Path,required=True)
    args=parser.parse_args();root=args.root.resolve()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    previous=json.loads(args.runtime_report.read_text())
    if previous["architecture"]!=args.arch or digest(args.runtime)!=previous["bundle_sha256"]: raise ValueError("Xorg dependency runtime/report mismatch")
    source=root/"src/Applications/xserver/xorg-server/21.1.25"
    with tempfile.TemporaryDirectory(prefix=".native-xorg-server-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();sdk,core,tree=work/"sdk",work/"core",work/"runtime"
        for path,target in ((args.sdk,sdk),(args.core,core),(args.runtime,tree)):
            with tarfile.open(path) as package: package.extractall(target,filter="data")
        resource=run(["clang-20","-print-resource-dir"]).strip()
        flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC","-ffreestanding","-fno-builtin",
               "-fno-stack-protector","-fno-strict-aliasing","-nostdinc","-D_DARWIN_C_SOURCE=1","-D_FORTIFY_SOURCE=0","-DHAVE_DIX_CONFIG_H",
               "-Werror=implicit-function-declaration","-Werror=incompatible-pointer-types","-Wno-availability",
               "-isystem",resource+"/include","-isystem",sdk/"usr/include","-isystem",sdk/"usr/local/include",
               "-I",core/"usr/include","-I",core/"usr/include/pixman-1","-I",core/"usr/libexec/xorg","-I",source]
        includes=re.search(r"^inc\s*=\s*include_directories\((.*?)\)",(source/"meson.build").read_text(),re.M|re.S)
        for name in re.findall(r"'([^']+)'",includes[1]): flags += ["-I",source/name]
        flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
        objects=work/"objects";objects.mkdir()
        subprocess.run(["llvm-ar-20","x",str(core/"usr/libexec/xorg/core.a")],cwd=objects,check=True,capture_output=True)
        records=[]
        for name in ("ddxPrivate.c","ddxKillSrv.c","ddxVT.c"):
            path=source/"xkb"/name;obj=objects/("native_"+path.stem+".o")
            run([*flags,"-c",path,"-o",obj])
            records.append({"source":str(path.relative_to(root)),"source_sha256":digest(path),"object_sha256":digest(obj),"member":obj.name})
        shutil.copyfile(args.display,objects/"native_display.o");shutil.copyfile(args.input,objects/"native_input.o")
        run(["llvm-ar-20","--format=darwin","rcsD",args.archive,*sorted(objects.glob("*.o"))])
        imported=symbols(args.archive)[1]-definitions(args.archive)
        images={"/"+str(path.relative_to(tree)):image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        existing=set().union(*(set(image["exports"]) for image in images.values()))
        requests=set(args.base_imports.read_text().split())|set(args.init_exports.read_text().split())|(imported-existing-{"dyld_stub_binder"})
        args.imports.write_text("\n".join(sorted(requests))+"\n")
        if args.prepare:
            args.report.write_text(json.dumps({"architecture":args.arch,"archive_sha256":digest(args.archive),"original_ddx_sources":records,
                "new_system_requests":sorted(imported-existing),"core_sha256":digest(args.core),"display_sha256":digest(args.display),"input_sha256":digest(args.input),"runtime_checked":False},indent=2)+"\n")
            print(f"Native Xorg {args.arch}: prepared complete server archive; {len(imported-existing)} additional System requests")
            return
        if not args.system: raise ValueError("native Xorg requires a complete source-built System")
        with tarfile.open(args.system) as package: package.extractall(tree,filter="data")
        images={"/"+str(path.relative_to(tree)):image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        needed=set()
        for symbol in imported:
            candidates={name for name,image in images.items() if symbol in image["exports"]}
            if len(candidates)>1: raise ValueError("ambiguous Xorg provider: "+symbol)
            if not candidates and symbol!="dyld_stub_binder": raise ValueError("Xorg API needs real provider: "+symbol)
            needed|=candidates
        program=tree/"usr/bin/Xorg";program.parent.mkdir(parents=True,exist_ok=True)
        run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-no_fixup_chains","-dead_strip","-dead_strip_dylibs",
             "-platform_version","macos","26.5","26.5","-adhoc_codesign","-pie","-e","_main","-undefined","dynamic_lookup",
             "-o",program,args.archive.resolve(),*[tree/name.lstrip("/") for name in sorted(needed)],"-L",tree/"usr/lib/system",tree/"usr/lib/libSystem.B.dylib"])
        sign(program);images["/usr/bin/Xorg"]=image_metadata(program.read_bytes())
        loader=image_metadata(args.dyld.read_bytes())["exports"]|{"dyld_stub_binder"}
        programs={}
        for name,record in previous["programs"].items():
            path=record["install_path"];original=tree/path.lstrip("/")
            if digest(original)!=record["sha256"]: raise ValueError("modified native program: "+path)
            images[path]=image_metadata(original.read_bytes())
            programs[name]={**record,"import_resolutions":verify_imports(path,images,loader,flat_namespace_root=path)}
        programs["Xorg"]={"install_path":"/usr/bin/Xorg","sha256":digest(program),"archive_sha256":digest(args.archive),
            "linked_dylibs":sorted(needed),"import_resolutions":verify_imports("/usr/bin/Xorg",images,loader,flat_namespace_root="/usr/bin/Xorg")}
        pack(tree,args.output)
        report={**previous,"programs":programs,"bundle_sha256":digest(args.output),"system_sha256":digest(args.system),"runtime_checked":False,
            "xorg":{"version":"21.1.25","original_ddx_sources":records,"core_sha256":digest(args.core),"display_sha256":digest(args.display),"input_sha256":digest(args.input),
                    "native_backends":"NativeBootFramebuffer scanout and console ownership; NativeBootInput i8042/DWC2 HID events", "server_program_linked":True},"desktop_checked":False}
        args.report.write_text(json.dumps(report,indent=2)+"\n")
        print(f"Native Xorg {args.arch}: signed full server, native display/input and exact binding closure; guest execution pending")

if __name__=="__main__":main()
