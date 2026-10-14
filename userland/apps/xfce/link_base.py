#!/usr/bin/env python3
"""Add genuine signed Xfce libraries/services to the verified native Xorg stack."""
import argparse
import json
from pathlib import Path
import re
import sys
import tarfile
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"glib"))
from build_dependencies import digest, definitions
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"x11"))
from build_clients import pack
from link_auth import image_metadata, verify_imports
from build_base import RELEASES

def metadata(root,name):
    project="xfconf" if name=="libxfconf-0" else "libxfce4util"
    source=root/"src/Applications/xfce"/project/RELEASES[project]/"configure.ac"
    variable="libxfconf_verinfo" if project=="xfconf" else "libxfce4util_verinfo"
    match=re.search(r"m4_define\(\["+variable+r"\],\s*\[(\d+):(\d+):(\d+)\]\)",source.read_text())
    if not match:raise ValueError("original Xfce interface version missing")
    current,revision,age=map(int,match.groups())
    return {"major":str(current-age),"compatibility":f"{current+1}.0.0","current":f"{current+1}.{revision}.0",
            "source":str(source.relative_to(root)),"source_sha256":digest(source)}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","libraries","build-report","runtime","runtime-report","system","dyld","output","report"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    args=parser.parse_args();root=args.root.resolve();args.output.parent.mkdir(parents=True,exist_ok=True)
    previous=json.loads(args.runtime_report.read_text());compiled=json.loads(args.build_report.read_text())
    if previous["architecture"]!=args.arch or digest(args.runtime)!=previous["bundle_sha256"]:raise ValueError("native Xorg dependency mismatch")
    if compiled["architecture"]!=args.arch or digest(args.libraries)!=compiled["bundle_sha256"]:raise ValueError("native Xfce source bundle mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-xfce-runtime-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();tree,libraries=work/"runtime",work/"libraries"
        for source,target in ((args.runtime,tree),(args.libraries,libraries),(args.system,tree)):
            with tarfile.open(source) as archive:archive.extractall(target,filter="data")
        images={"/"+str(path.relative_to(tree)):image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        names=list(compiled["libraries"]);public={name:set(record["exports"]) for name,record in compiled["libraries"].items()}
        paths={name:"/usr/lib/"+name+"."+metadata(root,name)["major"]+".dylib" for name in names}
        ordered=[]
        def visit(name):
            if name in ordered:return
            for other in names:
                if other!=name and set(compiled["libraries"][name]["imports"])&public[other]:visit(other)
            ordered.append(name)
        for name in names:visit(name)
        def providers_for(archive):
            needed=set()
            for symbol in symbols(archive)[1]-definitions(archive)-{"___dso_handle","dyld_stub_binder"}:
                matches={path for path,image in images.items() if path.endswith(".dylib") and symbol in image["exports"]}
                if len(matches)>1:raise ValueError("ambiguous native Xfce provider: "+symbol+": "+repr(matches))
                if not matches:raise ValueError("Xfce API needs genuine provider: "+symbol)
                needed|=matches
            return needed
        library_records=dict(previous["libraries"])
        for name in ordered:
            archive=libraries/"usr/lib"/(name+".a");destination=tree/paths[name].lstrip("/");version=metadata(root,name)
            needed=providers_for(archive)
            run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-dylib","-no_fixup_chains","-dead_strip_dylibs","-platform_version","macos","26.5","26.5",
                 "-adhoc_codesign","-undefined","dynamic_lookup","-install_name",paths[name],"-compatibility_version",version["compatibility"],"-current_version",version["current"],
                 "-force_load",archive,*[tree/path.lstrip("/") for path in sorted(needed)],"-L",tree/"usr/lib/system",tree/"usr/lib/libSystem.B.dylib","-o",destination])
            sign(destination);images[paths[name]]=image_metadata(destination.read_bytes())
            library_records[name]={"install_path":paths[name],"sha256":digest(destination),"archive_sha256":digest(archive),"exports":sorted(images[paths[name]]["exports"]),"version_metadata":version}
        programs=dict(previous["programs"])
        for name,record in compiled["programs"].items():
            path="/usr/libexec/xfce4/xfconf/xfconfd" if name=="xfconfd" else "/usr/bin/"+name
            archive=libraries/"usr/libexec/xfce"/(name+".a");destination=tree/path.lstrip("/");destination.parent.mkdir(parents=True,exist_ok=True)
            needed=providers_for(archive)
            run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-no_fixup_chains","-dead_strip","-dead_strip_dylibs","-platform_version","macos","26.5","26.5",
                 "-adhoc_codesign","-pie","-e","_main","-undefined","dynamic_lookup","-o",destination,archive,*[tree/p.lstrip("/") for p in sorted(needed)],
                 "-L",tree/"usr/lib/system",tree/"usr/lib/libSystem.B.dylib"])
            sign(destination);images[path]=image_metadata(destination.read_bytes())
            programs[name]={"install_path":path,"sha256":digest(destination),"archive_sha256":digest(archive),"linked_dylibs":sorted(needed)}
        for source in (libraries/"usr/share").rglob("*"):
            if source.is_file():
                target=tree/source.relative_to(libraries);target.parent.mkdir(parents=True,exist_ok=True)
                if not target.exists():target.write_bytes(source.read_bytes())
        loader=image_metadata(args.dyld.read_bytes())["exports"]|{"dyld_stub_binder"}
        modules=dict(previous["modules"])
        for record in [*library_records.values(),*programs.values(),*modules.values(),previous["launch_desktop"]]:
            path=record["install_path"];file=tree/path.lstrip("/")
            if digest(file)!=record["sha256"]:raise ValueError("unexpected native retained image change: "+path)
            images[path]=image_metadata(file.read_bytes())
            record["import_resolutions"]=verify_imports(path,images,loader,flat_namespace_root=path)
        pack(tree,args.output)
        report={**previous,"libraries":library_records,"programs":programs,"modules":modules,"bundle_sha256":digest(args.output),"system_sha256":digest(args.system),
            "runtime_checked":False,"desktop_checked":False,"xfce_base":{"build_report_sha256":digest(args.build_report),"previous_runtime_sha256":digest(args.runtime),
                "service":"/usr/share/dbus-1/services/org.xfce.Xfconf.service","backend":"Original GDBus service and per-channel XML storage; no fake daemon or ABI adapter"}}
        args.report.write_text(json.dumps(report,indent=2)+"\n")
        print(f"Native Xfce base {args.arch}: {len(library_records)} signed app dylibs, genuine xfconfd/query and retained Xorg/services; guest configuration/UI pending")

if __name__=="__main__":main()
