#!/usr/bin/env python3
"""Install the real native Xorg/runtime in an audited copy of the minimal root."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"scripts"))
from native_components import run
from build_diagnostic_userland import verify_installed_files
from link_auth import image_metadata, verify_imports, verify_closure
from sign_diagnostic_macho import sign

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("runtime","runtime-report","base-root","dyld","hfs-tool","output","report","sdk","headers","probe-source"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    parser.add_argument("--probe-name",choices=("native-xorg-test","native-xfce-base-test","native-xfce-ui-test","native-xfce-desktop-test","native-pty-test","native-cxx-system-test","native-cxx-test","native-xfce-start"),default="native-xorg-test")
    parser.add_argument("--product-desktop",action="store_true",help="configure PID 1 to start the original Xfce session automatically")
    parser.add_argument("--root-size-mib",type=int,choices=(64,128),default=64)
    parser.add_argument("--probe-object",type=Path,help="use an already compiled native probe while retaining its source hash")
    parser.add_argument("--desktop-check-object",type=Path,help="install the native desktop inspector in the product image")
    parser.add_argument("--desktop-check-source",type=Path)
    args=parser.parse_args();args.output.parent.mkdir(parents=True,exist_ok=True)
    runtime_report=json.loads(args.runtime_report.read_text())
    if runtime_report["architecture"]!=args.arch or digest(args.runtime)!=runtime_report["bundle_sha256"]:raise ValueError("Xorg runtime/report mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-xorg-root-",dir=args.output.parent) as directory:
        work=Path(directory).resolve();tree=work/"runtime"
        with tarfile.open(args.runtime) as archive:archive.extractall(tree,filter="data")
        sdk,headers=work/"sdk",work/"headers"
        for path,target in ((args.sdk,sdk),(args.headers,headers)):
            with tarfile.open(path) as archive:archive.extractall(target,filter="data")
        resource=run(["clang-20","-print-resource-dir"]).strip()
        flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC","-ffreestanding","-fno-builtin",
               "-fno-stack-protector","-nostdinc","-D_DARWIN_C_SOURCE=1","-D_FORTIFY_SOURCE=0","-Wall","-Wextra","-Werror","-Wno-availability",
               "-isystem",sdk/"usr/include","-isystem",resource+"/include","-I",headers/"usr/include",
               "-I",headers/"usr/include/glib-2.0","-I",headers/"usr/include/xfce4","-I",headers/"usr/include/xfce4/xfconf-0"]
        for include in ("gtk-3.0","pango-1.0","gdk-pixbuf-2.0","atk-1.0","cairo","harfbuzz","freetype2","xfce4/libxfce4ui-2","xfce4/libxfce4kbd-private-3"):
            flags += ["-I",headers/"usr/include"/include]
        flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
        obj=work/"probe.o"
        if args.probe_object:shutil.copyfile(args.probe_object,obj)
        else:run([*flags,"-c",args.probe_source,"-o",obj])
        probe=tree/"bin"/args.probe_name;probe.parent.mkdir(parents=True,exist_ok=True)
        run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-no_fixup_chains","-dead_strip","-dead_strip_dylibs",
             "-platform_version","macos","26.5","26.5","-adhoc_codesign","-pie","-e","_main","-undefined","dynamic_lookup",
             "-o",probe,obj,*sorted((tree/"usr/lib").glob("*.dylib")),"-L",tree/"usr/lib/system"])
        sign(probe)
        if args.desktop_check_object:
            if not args.product_desktop or not args.desktop_check_source:
                raise ValueError("desktop inspector requires its source and a product root")
            inspector=tree/"bin/native-xfce-desktop-test"
            run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-no_fixup_chains","-dead_strip","-dead_strip_dylibs",
                 "-platform_version","macos","26.5","26.5","-adhoc_codesign","-pie","-e","_main","-undefined","dynamic_lookup",
                 "-o",inspector,args.desktop_check_object,*sorted((tree/"usr/lib").glob("*.dylib")),"-L",tree/"usr/lib/system"])
            sign(inspector)
        if args.product_desktop:
            if args.probe_name != "native-xfce-start" or args.root_size_mib != 128:
                raise ValueError("invalid product desktop launcher/profile")
            config=tree/"etc/desktop-session";config.parent.mkdir(parents=True,exist_ok=True)
            config.write_text("/bin/native-xfce-start\n")
            perchannel=tree/"etc/xdg/xfce4/xfconf/xfce-perchannel-xml"
            session_config=perchannel/"xfce4-session.xml";document=ET.parse(session_config)
            failsafe=document.find(".//property[@name='Failsafe']")
            if failsafe is None:raise ValueError("original Xfce failsafe session missing")
            count=failsafe.find("property[@name='Count']")
            if count is None or count.get("value")!="5":raise ValueError("original Xfce session inventory changed")
            thunar=failsafe.find("property[@name='Client3_Command']/value")
            if thunar is None or thunar.get("value")!="Thunar":raise ValueError("original file-manager client changed")
            for property in list(failsafe):
                name=property.get("name","")
                if name.startswith("Client3_"):failsafe.remove(property)
                elif name.startswith("Client4_"):property.set("name",name.replace("Client4_","Client3_",1))
            count.set("value","4");document.write(session_config,encoding="utf-8",xml_declaration=True)
            # Preserve the original terminal desktop entry, with its actual
            # embedded launch command, as an Xfce autostart application.
            entry=(tree/"usr/share/applications/xfce4-terminal.desktop").read_text()
            if "Exec=xfce4-terminal\n" not in entry:raise ValueError("original terminal desktop command changed")
            entry=entry.replace("Exec=xfce4-terminal\n","Exec=xfce4-terminal --disable-server --geometry=80x24\n",1)
            autostart=tree/"etc/xdg/autostart/xfce4-terminal.desktop";autostart.parent.mkdir(parents=True,exist_ok=True);autostart.write_text(entry)
            panel_config=perchannel/"xfce4-panel.xml";panels=ET.parse(panel_config)
            for property in panels.findall(".//property[@name='autohide-behavior']"):property.set("value","0")
            plugin_ids=panels.find(".//property[@name='panel-2']/property[@name='plugin-ids']")
            if plugin_ids is None:raise ValueError("original bottom panel inventory missing")
            for value in list(plugin_ids):
                if value.get("value") in ("14","15","16","18"):plugin_ids.remove(value)
            terminal_entry=panels.find(".//property[@name='plugin-13']/property[@name='items']/value")
            if terminal_entry is None:raise ValueError("original terminal launcher missing")
            terminal_entry.set("value","xfce4-terminal.desktop")
            panels.write(panel_config,encoding="utf-8",xml_declaration=True)
            # First-run panel migration reads this same embedded profile.
            (tree/"etc/xdg/xfce4/panel/default.xml").write_bytes(panel_config.read_bytes())
        image=work/"root.img";shutil.copyfile(args.base_root,image)
        def edit(*command):return run([args.hfs_tool.resolve(),image,*command])
        if args.root_size_mib * 1024 * 1024 > image.stat().st_size:
            edit("grow",str(args.root_size_mib * 1024 * 1024))
        if image.stat().st_size != args.root_size_mib * 1024 * 1024:
            raise ValueError("native graphical root size mismatch")
        original=work/"original";original.mkdir();edit("extractall","/",original)
        expected={"/"+str(path.relative_to(original)):path.read_bytes() for path in original.rglob("*") if path.is_file()}
        installed={"/"+str(path.relative_to(tree)):path for path in tree.rglob("*") if path.is_file()}
        installed["/usr/lib/dyld"]=args.dyld
        directories={"/tmp","/var/tmp","/var/root","/var/root/Desktop","/var/log","/var/lib","/var/lib/xkb","/var/run","/var/empty","/var/cache/fontconfig","/etc/fonts/conf.d"}
        for name in installed:
            parent=Path(name).parent
            while str(parent)!="/":directories.add(str(parent));parent=parent.parent
        for name in sorted(directories,key=lambda name:(len(Path(name).parts),name)):
            if not (original/name.lstrip("/")).is_dir():edit("mkdir",name)
        for name,path in installed.items():
            data=path.read_bytes();native=len(data)>=32 and struct.unpack_from("<I",data)[0]==0xfeedfacf
            edit("add",path,name);edit("chmod","755" if native else "644",name);expected[name]=data
        edit("chmod","1777","/tmp")
        edit("chmod","1777","/var/tmp")
        extracted=work/"verified";extracted.mkdir();edit("extractall","/",extracted)
        types={}
        for name,data in expected.items():
            if data.startswith(b"\x7fELF"):raise ValueError("ELF in native graphical root: "+name)
            if len(data)>=32 and struct.unpack_from("<I",data)[0]==0xfeedfacf:
                cpu,kind=struct.unpack_from("<4I",data)[1::2]
                if cpu!=(0x100000c if args.arch=="arm64" else 0x1000007) or kind not in (2,6,7,8):raise ValueError("foreign native root image: "+name)
                types[name]=kind
        verify_installed_files(extracted,args.arch,expected,types)
        images={name:image_metadata(expected[name]) for name in types}
        loader=images["/usr/lib/dyld"]["exports"]|{"dyld_stub_binder"}
        # System components use the main program's facade/reexport graph for
        # flat imports. Programs are audited in their own actual graph.
        bindings={name:verify_imports(name,images,loader,flat_namespace_root=name if kind==2 else "/bin/"+args.probe_name)
                  for name,kind in types.items() if kind!=7}
        startup_closures={name:verify_closure(name,images,loader) for name,kind in types.items() if kind==2}
        run(["fsck.hfsplus","-n",image])
        args.report.write_text(json.dumps({"architecture":args.arch,"root_size_bytes":image.stat().st_size,"root_sha256":digest(image),"base_root_sha256":digest(args.base_root),
            "runtime_sha256":digest(args.runtime),"dyld_sha256":digest(args.dyld),"probe_source_sha256":digest(args.probe_source),
            "probe_sha256":digest(probe),"installed_sha256":{name:hashlib.sha256(data).hexdigest() for name,data in sorted(expected.items())},
            "import_resolutions":bindings,"startup_closures":startup_closures,"runtime_checked":False,"desktop_checked":False,
            "automatic_desktop_startup":args.product_desktop,
            "desktop_check_source_sha256":digest(args.desktop_check_source) if args.desktop_check_source else None,
            "validation":"Complete HFS byte equality, native Mach-O CPU/type/signatures/ordinals including real GTK MH_BUNDLE, no ELF and fsck."},indent=2)+"\n")
        image.replace(args.output)
        print(f"Native Xorg root {args.arch}: {len(expected)} files, {len(types)} actual Mach-O programs/libraries/modules; runtime pending")

if __name__=="__main__":main()
