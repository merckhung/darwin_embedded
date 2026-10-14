#!/usr/bin/env python3
"""Build native xkbcomp and original XKB layout data entirely through Bazel."""
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

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glib"))
from build_dependencies import digest, definitions, inventory
from build_clients import pack
from link_auth import image_metadata, verify_imports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--compile-only", action="store_true")
    for name in ("base-imports", "imports", "system"):
        parser.add_argument("--" + name, type=Path)
    for name in ("root", "sdk", "headers", "runtime", "runtime-report", "dyld", "archive", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args(); root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    previous = json.loads(args.runtime_report.read_text())
    if previous["architecture"] != args.arch or digest(args.runtime) != previous["bundle_sha256"]:
        raise ValueError("native dependency runtime/report mismatch")
    source = root / "src/Applications/xserver/xkbcomp/1.5.0"
    layouts = root / "src/Applications/xserver/xkeyboard-config/2.48"
    for package in (source, layouts):
        manifest = json.loads((package / "UPSTREAM.json").read_text())
        for name, expected in manifest["upstream_files_sha256"].items():
            if digest(package / name) != expected: raise ValueError("modified upstream XKB file: " + name)
        for name, target in manifest.get("upstream_symlinks", {}).items():
            # Bazel rewrites input symlinks to its execution tree. Verify the
            # actual alias bytes against the pinned original destination.
            alias = package / name
            if alias.read_bytes() != (alias.parent / target).read_bytes(): raise ValueError("modified upstream XKB alias: " + name)
    with tempfile.TemporaryDirectory(prefix=".native-xkb-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, headers, tree = work / "sdk", work / "headers", work / "runtime"
        for archive, target in ((args.sdk,sdk), (args.headers,headers), (args.runtime,tree)):
            with tarfile.open(archive) as package: package.extractall(target, filter="data")
        generated = work / "generated"; generated.mkdir()
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration",
                 "-Werror=incompatible-pointer-types", "-Wno-availability", "-isystem", resource + "/include",
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include", "-I", headers / "usr/include",
                 "-I", generated, "-I", source]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        features = {}
        for function, header in (("asprintf","stdio.h"), ("reallocarray","stdlib.h"), ("recallocarray","stdlib.h")):
            probe = generated / "feature.c"; probe.write_text('#include <' + header + '>\nvoid *p=(void*)&' + function + ';\n')
            features[function] = subprocess.run([*map(str,flags), "-Werror", "-c", str(probe), "-o", str(generated / "feature.o")], capture_output=True).returncode == 0
        config = {"PACKAGE_VERSION": '"1.5.0"', "DFLT_XKB_CONFIG_ROOT": '"/usr/share/X11/xkb"',
                  "XKBCONFIGROOT": '"/usr/share/X11/xkb"', "HAVE_REALLOCARRAY": 1}
        if features["asprintf"]: config["HAVE_ASPRINTF"] = 1
        if features["recallocarray"]: config["HAVE_RECALLOCARRAY"] = 1
        (generated / "config.h").write_text("".join(f"#define {key} {value}\n" for key,value in sorted(config.items())))
        # Original xkbcomp's fallback multiplies without an overflow check.
        # Select the already pinned X.Org/OpenBSD checked implementation instead.
        (generated / "allocator.h").write_text('#include <stdlib.h>\nvoid *reallocarray(void *, size_t, size_t);\n')
        flags += ["-include", generated / "allocator.h"]
        names = inventory(source / "Makefile.am", "xkbcomp_SOURCES")
        paths = [source / ("xkbparse.c" if name == "xkbparse.y" else name) for name in names if name.endswith((".c",".y"))]
        allocator = root / "src/Applications/xserver/libXfont2/2.0.9/src/util/reallocarray.c"
        paths.append(allocator)
        def compile_one(index):
            original = paths[index]; obj = generated / f"xkb_{index:02d}.o"
            extra = ["-I", allocator.parents[2]] if original == allocator else []
            run([*flags,*extra,"-c",original,"-o",obj])
            if struct.unpack_from("<4I",obj.read_bytes())[::3] != (0xfeedfacf,1) or struct.unpack_from("<I",obj.read_bytes(),4)[0] != (0x100000c if args.arch == "arm64" else 0x1000007):
                raise ValueError("non-native XKB object")
            return {"source":str(original.relative_to(root)),"source_sha256":digest(original),"member":obj.name,"object_sha256":digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool: objects = list(pool.map(compile_one,range(len(paths))))
        run(["llvm-ar-20","--format=darwin","rcsD",args.archive,*[generated / item["member"] for item in objects]])
        imported = symbols(args.archive)[1] - definitions(args.archive)
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        if args.compile_only:
            existing = set().union(*(set(image["exports"]) for image in images.values()))
            requests = set(args.base_imports.read_text().split()) | (imported - existing - {"dyld_stub_binder"})
            args.imports.write_text("\n".join(sorted(requests))+"\n")
            args.report.write_text(json.dumps({"architecture":args.arch,"objects":objects,"target_header_features":features,
                "configuration":config,"archive_sha256":digest(args.archive),"new_system_requests":sorted(imported-existing),"runtime_checked":False},indent=2)+"\n")
            print(f"Native XKB {args.arch}: {len(objects)} Mach-O objects; additional genuine System requests: {sorted(imported-existing)}")
            return
        if not args.system: raise ValueError("native XKB runtime requires its complete source-built System")
        with tarfile.open(args.system) as package: package.extractall(tree,filter="data")
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        providers = {name:set(image["exports"]) for name,image in images.items()}
        needed = set()
        for symbol in imported:
            candidates = {name for name,public in providers.items() if symbol in public}
            if len(candidates) > 1: raise ValueError("ambiguous XKB provider: " + symbol)
            if not candidates and symbol != "dyld_stub_binder": raise ValueError("XKB API needs a real provider: " + symbol)
            needed |= candidates
        program = tree / "usr/bin/xkbcomp"; program.parent.mkdir(parents=True,exist_ok=True)
        run(["/usr/lib/llvm-20/bin/ld64.lld","-arch",args.arch,"-no_fixup_chains","-dead_strip","-dead_strip_dylibs",
             "-platform_version","macos","26.5","26.5","-adhoc_codesign","-pie","-e","_main","-undefined","dynamic_lookup",
             "-o",program,args.archive.resolve(),*[tree / name.lstrip("/") for name in sorted(needed)],
             "-L",tree / "usr/lib/system",tree / "usr/lib/libSystem.B.dylib"])
        sign(program); images["/usr/bin/xkbcomp"] = image_metadata(program.read_bytes())
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        bindings = verify_imports("/usr/bin/xkbcomp",images,loader,flat_namespace_root="/usr/bin/xkbcomp")
        programs = {}
        for name,record in previous["programs"].items():
            path = record["install_path"]; executable = tree / path.lstrip("/")
            if digest(executable) != record["sha256"]: raise ValueError("modified retained native program: " + path)
            images[path] = image_metadata(executable.read_bytes())
            programs[name] = {**record,"import_resolutions":verify_imports(path,images,loader,flat_namespace_root=path)}
        modules = {}
        for name,record in previous.get("modules",{}).items():
            path=record["install_path"]; module=tree / path.lstrip("/")
            if digest(module) != record["sha256"]: raise ValueError("modified retained native module: " + path)
            images[path] = image_metadata(module.read_bytes())
            modules[name] = {**record,"import_resolutions":verify_imports(path,images,loader,flat_namespace_root=path)}
        data = tree / "usr/share/X11/xkb"; data.mkdir(parents=True,exist_ok=True)
        for family in ("compat","geometry","keycodes","types","symbols"):
            for path in sorted((layouts / family).rglob("*")):
                if path.is_file() and path.name not in ("meson.build","custom"):
                    destination = data / path.relative_to(layouts); destination.parent.mkdir(parents=True,exist_ok=True)
                    destination.write_bytes(path.read_bytes())
        # The upstream generator interprets filesystem symlinks as XKB aliases.
        # Bazel symlinks every source input, so give the unchanged generator a
        # private normal-file tree with only the eight original source aliases.
        generator_source = work / "layout-sources"
        shutil.copytree(layouts,generator_source,symlinks=False)
        manifest = json.loads((layouts / "UPSTREAM.json").read_text())
        for name,target in manifest.get("upstream_symlinks",{}).items():
            alias=generator_source / name; alias.unlink(); alias.symlink_to(target)
        environment = {**os.environ,"PYTHONPATH":str(generator_source),"PYTHONDONTWRITEBYTECODE":"1"}
        rules = data / "rules"; rules.mkdir()
        for name in ("base","evdev"):
            command = ["python3","-m","rules.generator","rules","--ruleset",name,"--version","v2","--compat","--output",rules / name, generator_source / "rules/rules.in"]
            result = subprocess.run(list(map(str,command)),env=environment,text=True,capture_output=True)
            if result.returncode: raise RuntimeError(result.stderr)
            for suffix in (".xml",".extras.xml"):
                shutil.copyfile(layouts / ("rules/base" + suffix),rules / (name + suffix))
            listing = subprocess.run(["perl",str(layouts / "rules/xml2lst.pl"),str(rules / (name + ".xml"))],text=True,capture_output=True)
            if listing.returncode: raise RuntimeError(listing.stderr)
            (rules / (name + ".lst")).write_text(listing.stdout)
        result = subprocess.run(["python3","-m","rules.generator","symbols",str(data / "symbols")],env=environment,text=True,capture_output=True)
        if result.returncode: raise RuntimeError(result.stderr)
        for name in ("README","xkb.dtd","xfree98"): shutil.copyfile(layouts / "rules" / name,rules / name)
        for family in ("keycodes","rules"):
            shutil.copyfile(root / "userland/apps/x11/data" / family / "nativehid",data / family / "nativehid")
        for suffix in (".xml",".extras.xml",".lst"): shutil.copyfile(rules / ("base" + suffix),rules / ("nativehid" + suffix))
        pack(tree,args.output)
        program_record = {"install_path":"/usr/bin/xkbcomp","sha256":digest(program),"archive_sha256":digest(args.archive),
                          "linked_dylibs":sorted(needed),"import_resolutions":bindings}
        report = {**previous,"bundle_sha256":digest(args.output),"system_sha256":digest(args.system),"programs":{**programs,"xkbcomp":program_record},"modules":modules,
            "runtime_checked":False,"desktop_checked":False,"xkb":{"version":"1.5.0","layout_version":"2.48",
            "objects":objects,"target_header_features":features,"configuration":config,"data_generation":"Original Python rules/symbols generator and Perl XML registry generator; no Meson/CMake/Make execution",
            "installed_data_sha256":{"/" + str(path.relative_to(tree)):digest(path) for path in sorted(data.rglob("*")) if path.is_file()},
            "dependency_runtime_sha256":digest(args.runtime),"source_manifests_sha256":{str(path.relative_to(root)):digest(path) for path in (source / "UPSTREAM.json",layouts / "UPSTREAM.json")}}}
        args.report.write_text(json.dumps(report,indent=2)+"\n")
        print(f"Native XKB {args.arch}: {len(objects)} Mach-O objects, signed xkbcomp, {len(report['xkb']['installed_data_sha256'])} original/native layout files; guest execution pending")


if __name__ == "__main__": main()
