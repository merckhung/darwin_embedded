#!/usr/bin/env python3
"""Build a native font consumer and install it in a real init/dash root."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glib"))
from build_dependencies import definitions, digest
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from build_diagnostic_userland import verify_installed_files
from sign_diagnostic_macho import sign


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("root","sdk","libraries","output","report","exports","probe","runtime","system","base-root","dyld","hfs-tool"):
        parser.add_argument("--"+name,type=Path,required=name in ("root","sdk","output","report"))
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    modes=parser.add_mutually_exclusive_group()
    modes.add_argument("--pango",action="store_true")
    modes.add_argument("--pixbuf",action="store_true")
    modes.add_argument("--dbus",action="store_true")
    args=parser.parse_args()
    root=args.root.resolve()
    program="native-dbus-test" if args.dbus else "native-pixbuf-test" if args.pixbuf else "native-pango-test" if args.pango else "native-fonts-test"
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-fonts-test-",dir=args.output.parent) as directory:
        work=Path(directory).resolve()
        if args.exports:
            sdk,libraries=work/"sdk",work/"libraries"
            for path,destination in ((args.sdk,sdk),(args.libraries,libraries)):
                with tarfile.open(path) as archive: archive.extractall(destination,filter="data")
            if json.loads((sdk/"DarwinSDK.json").read_text())["architecture"]!=args.arch:
                raise ValueError("font probe SDK architecture mismatch")
            compiler_rt=root/"src/Toolchains/compiler-rt/20.1.8"
            resource=run(["clang-20","-print-resource-dir"]).strip()
            flags=["clang-20",f"--target={args.arch}-apple-macos26.5","-std=gnu11","-O2","-fPIC",
                   "-ffreestanding","-fno-builtin","-fno-stack-protector","-nostdinc","-D_DARWIN_C_SOURCE=1",
                   "-D_FORTIFY_SOURCE=0","-Wall","-Wextra","-Werror","-Wno-availability",
                   "-isystem",sdk/"usr/include","-isystem",resource+"/include","-I",libraries/"usr/include",
                   "-I",libraries/"usr/include/glib-2.0","-I",libraries/"usr/include/harfbuzz",
                   "-I",libraries/"usr/include/cairo","-I",libraries/"usr/include/freetype2",
                   "-I",libraries/"usr/include/pango-1.0",
                   "-I",libraries/"usr/include/gdk-pixbuf-2.0",
                   "-I",libraries/"usr/include/dbus-1.0",
                   "-I",compiler_rt/"lib/builtins"]
            flags += ["-mcpu=cortex-a72","-mno-outline-atomics"] if args.arch=="arm64" else ["-march=penryn"]
            inputs=[(root/"userland/apps/dbus/test_dbus.c",[])] if args.dbus else [(root/"userland/apps/gtk/test_pixbuf.c",["-Wno-deprecated-declarations"])] if args.pixbuf else [
                (root/"userland/apps/fonts"/("test_pango.c" if args.pango else "test_fonts.c"),[])]
            if args.pango:
                inputs.append((root/"src/Applications/text/libthai/0.1.30/tests/test_thbrk.c",
                               ["-Dmain=native_thai_vectors","-Wno-pointer-sign"]))
            elif not (args.pixbuf or args.dbus):
                for name in ("udivti3","udivmodti4"):
                    inputs.append((compiler_rt/"lib/builtins"/(name+".c"),["-fvisibility=hidden"]))
                    inputs.append((compiler_rt/"test/builtins/Unit"/(name+"_test.c"),["-Dmain=native_"+name+"_vectors"]))
            records,objects=[],[]
            for index,(source,options) in enumerate(inputs):
                obj=work/f"fonts_{index:03d}.o"
                run([*flags,*options,"-c",source,"-o",obj])
                magic,cpu,_,kind=struct.unpack_from("<4I",obj.read_bytes())
                if (magic,cpu,kind)!=(0xfeedfacf,0x100000c if args.arch=="arm64" else 0x1000007,1):
                    raise ValueError("font probe contains a foreign object")
                objects.append(obj)
                records.append({"source":str(source.relative_to(root)),"source_sha256":digest(source),
                                "compiler_options":options,"object_sha256":digest(obj),"member":obj.name})
            run(["llvm-ar-20","--format=darwin","rcsD",args.output,*objects])
            imports=set().union(*(symbols(path)[1] for path in objects))-definitions(args.output)
            provided=set().union(*(symbols(path)[0] for path in (libraries/"usr/lib").glob("*.a")))
            requests=sorted(imports-provided)
            args.exports.write_text("\n".join(requests)+"\n")
            args.report.write_text(json.dumps({"architecture":args.arch,"sources":records,"imports":sorted(imports),
                "system_requests":requests,"probe_sha256":digest(args.output),"libraries_sha256":digest(args.libraries)},indent=2)+"\n")
            return
        tree=work/"runtime"
        for bundle in (args.runtime,args.system):
            with tarfile.open(bundle) as archive: archive.extractall(tree,filter="data")
        executable=work/program
        libraries=sorted(path for path in (tree/"usr/lib").glob("*.dylib") if path.name!="libSystem.B.dylib")
        run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-no_fixup_chains","-dead_strip","-dead_strip_dylibs",
             "-platform_version","macos","26.5","26.5","-adhoc_codesign","-pie","-e","_main",
             "-undefined","dynamic_lookup","-o",executable,args.probe.resolve(),*libraries,
             "-L",tree/"usr/lib/system",tree/"usr/lib/libSystem.B.dylib"])
        sign(executable)
        image=work/"root.img"
        shutil.copyfile(args.base_root,image)
        def edit(*arguments): return run([args.hfs_tool.resolve(),image,*arguments])
        original=work/"original"
        original.mkdir(); edit("extractall","/",original)
        expected={"/"+str(path.relative_to(original)):path.read_bytes() for path in original.rglob("*") if path.is_file()}
        harfbuzz=root/"src/Applications/fonts/harfbuzz/14.6.0"
        fixtures={"/usr/share/fonts/native-latin.ttf":harfbuzz/"perf/fonts/Roboto-Regular.ttf",
                  "/usr/share/fonts/native-arabic.ttf":harfbuzz/"test/shape/data/in-house/fonts/df768b9c257e0c9c35786c47cae15c46571d56be.ttf"}
        image_fixtures={}
        if args.pixbuf:
            source=root/"src/Applications/gtk/gdk-pixbuf/2.44.8/tests"
            names=["test-image.png","test-image.pixdata","test-image-rle.pixdata"]
            names += ["test-images/gif-test-suite/dispose-keep.gif"]
            names += [f"test-images/gif-test-suite/animation-fill.{index}.rgba" for index in range(4)]
            image_fixtures={"/usr/share/native-pixbuf/"+Path(name).name:source/name for name in names}
        installed={"/bin/"+program:executable,"/usr/lib/dyld":args.dyld,**fixtures,**image_fixtures}
        installed.update({"/"+str(path.relative_to(tree)):path for path in tree.rglob("*") if path.is_file()})
        if args.dbus:
            passwd=work/"passwd"
            passwd.write_bytes(expected["/etc/passwd"]+b"messagebus:*:81:81:D-Bus daemon:/var/empty:/bin/sh\n")
            group=work/"group"
            group.write_bytes(b"wheel:*:0:root\nstaff:*:20:desktop\nmessagebus:*:81:messagebus\n"
                b"native-extra:*:12345:desktop,root\nnative-duplicate:*:12345:desktop\nmalformed:*:bad:desktop\n"
                b"malformed-members:*:12346:desktop,,root\n")
            installed.update({"/etc/passwd":passwd,"/etc/group":group})
            if "/etc/master.passwd" in expected:
                master=work/"master.passwd"
                master.write_bytes(expected["/etc/master.passwd"]+b"messagebus:*:81:81::0:0:D-Bus daemon:/var/empty:/bin/sh\n")
                installed["/etc/master.passwd"]=master
        directories={str(parent) for name in installed for parent in Path(name).parents if parent!=Path("/")}
        directories.update(("/var/cache","/var/cache/fontconfig","/etc/fonts/conf.d"))
        if args.dbus:
            directories.update(("/tmp","/var/empty","/var/run","/var/run/dbus","/var/lib","/var/lib/dbus",
                "/etc/dbus-1","/etc/dbus-1/session.d","/etc/dbus-1/system.d","/usr/share/dbus-1/session.d",
                "/usr/share/dbus-1/system.d","/usr/share/dbus-1/services","/usr/share/dbus-1/system-services"))
        for name in sorted(directories,key=lambda name:(len(Path(name).parts),name)):
            if not (original/name.lstrip("/")).is_dir(): edit("mkdir",name)
        for name,path in installed.items():
            binary=name=="/usr/lib/dyld" or name.startswith(("/bin/","/usr/bin/","/usr/libexec/")) or name.endswith(".dylib")
            edit("add",path,name); edit("chmod","600" if args.dbus and name=="/etc/master.passwd" else "755" if binary else "644",name)
            expected[name]=path.read_bytes()
        if args.dbus: edit("chmod","1777","/tmp")
        extracted=work/"verified"
        extracted.mkdir(); edit("extractall","/",extracted)
        types={name:7 if name=="/usr/lib/dyld" else 6 if name.endswith(".dylib") else 2 for name in expected
               if name.startswith(("/bin/","/sbin/","/usr/bin/","/usr/libexec/")) or name=="/usr/lib/dyld" or name.endswith(".dylib")}
        verify_installed_files(extracted,args.arch,expected,types)
        images={name:image_metadata(expected[name]) for name in types}
        loader=images["/usr/lib/dyld"]["exports"]|{"dyld_stub_binder"}
        bindings={name:verify_imports(name,images,loader,flat_namespace_root="/bin/"+program)
                  for name,kind in types.items() if kind!=7}
        run(["fsck.hfsplus","-n",image])
        args.report.write_text(json.dumps({"architecture":args.arch,"root_sha256":digest(image),"executable_sha256":digest(executable),
            "probe_sha256":digest(args.probe),"runtime_sha256":digest(args.runtime),"system_sha256":digest(args.system),
            "base_root_sha256":digest(args.base_root),"dyld_sha256":digest(args.dyld),
            "font_fixtures":{name:{"source":str(path.relative_to(root)),"source_sha256":digest(path)} for name,path in fixtures.items()},
            "image_fixtures":{name:{"source":str(path.relative_to(root)),"source_sha256":digest(path)} for name,path in image_fixtures.items()},
            "installed_sha256":{name:hashlib.sha256(data).hexdigest() for name,data in sorted(expected.items())},
            "import_resolutions":bindings,"runtime_checked":False,
            "validation":"Real native init/dash; complete HFS byte/CPU/type inventory and actual loaded ordinal bindings; no ELF; fsck read-only PASS."},indent=2)+"\n")
        image.replace(args.output)
        print(f"Native {'D-Bus' if args.dbus else 'GdkPixbuf' if args.pixbuf else 'Pango' if args.pango else 'fonts'} {args.arch}: signed consumer, real fixtures/config/cache directories and native init/dash root")


if __name__=="__main__": main()
