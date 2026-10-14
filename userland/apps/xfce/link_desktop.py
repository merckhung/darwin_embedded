#!/usr/bin/env python3
"""Link original desktop libraries/programs and audit every native binding."""
import argparse
import json
from pathlib import Path
import re
import sys
import tarfile
import tempfile

from build_base import run, symbols, digest, definitions, pack
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports, verify_closure
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from sign_diagnostic_macho import sign

def metadata(root, name):
    if name in ("libc++", "libc++abi", "libunwind"):
        project={"libc++":"libcxx","libc++abi":"libcxxabi","libunwind":"libunwind"}[name]
        source=root/"src/Applications/cxx"/project/"20.1.8/src/CMakeLists.txt"
        major,compatibility,current="1","1.0.0","1.0.0"
    elif name=="liblz4":
        source=root/"src/Applications/xfce-prerequisites/lz4/1.10.0/lib/lz4.h"
        values=[re.search(r"#define\s+LZ4_VERSION_"+part+r"\s+(\d+)",source.read_text())[1] for part in ("MAJOR","MINOR","RELEASE")]
        major,compatibility,current=values[0],values[0]+".0.0",".".join(values)
    elif name=="libvte-2.91":
        source=root/"src/Applications/xfce-prerequisites/vte/0.84.1/meson.build"
        text=source.read_text();version=re.search(r"version:\s*'([0-9.]+)'",text)[1];vmajor,vminor,vmicro=map(int,version.split('.'));revision=0 if vminor%2 else vmicro;interface=vmajor+vminor*100+vmicro-revision+1
        major,compatibility,current="0",f"{interface}.0.0",f"{interface}.{revision}.0"
    elif name == "libjpeg":
        source = root / "src/Applications/xfce-prerequisites/libjpeg-turbo/3.2.0/sharedlib/CMakeLists.txt"
        if "VERSION ${SO_MAJOR_VERSION}.${SO_AGE}.${SO_MINOR_VERSION}" not in source.read_text():
            raise ValueError("original JPEG shared-library version convention changed")
        major, compatibility, current = "62", "62.0.0", "62.4.0"
    elif name == "libgdk_pixbuf-2.0":
        source = root / "src/Applications/gtk/gdk-pixbuf/2.44.8/meson.build"
        text = source.read_text(); _, minor, micro = map(int,re.search(r"version:\s*'([0-9.]+)'",text)[1].split("."))
        age = 0 if minor%2 else micro; interface = 100*minor+micro-age+1
        major, compatibility, current = "0", f"{interface}.0.0", f"{interface}.{age}.0"
    elif name == "libdisplay-info":
        source = root / "src/Applications/xfce-prerequisites/libdisplay-info/0.4.0/meson.build"
        match = re.search(r"version:\s*'([0-9.]+)'", source.read_text())
        if not match:
            raise ValueError("original display-info version missing")
        # Upstream installs SONAME equal to the minor version while major is 0.
        major = match[1].split(".")[1]; current = compatibility = match[1]
    elif name == "libyaml-0":
        source = root / "src/Applications/xfce-prerequisites/libyaml/0.2.5/configure.ac"
        interface, revision, age = [int(re.search(r"m4_define\(\[YAML_" + field + r"\], (\d+)\)", source.read_text())[1]) for field in ("CURRENT", "REVISION", "AGE")]
        major = str(interface - age); compatibility, current = f"{interface+1}.0.0", f"{interface+1}.{revision}.0"
    else:
        if name == "libexo-2":
            project, version, variable = "exo", "4.20.0", "libexo_verinfo"
        elif name.startswith("libgarcon"):
            project, version, variable = "garcon", "4.20.0", "garcon_verinfo"
        elif name == "libxfce4panel-2.0":
            project, version, variable = "xfce4-panel", "4.20.8", "libxfce4panel_verinfo"
        else:
            project, version = "libxfce4windowing", "4.20.7"
            variable = "libxfce4windowingui_verinfo" if name == "libxfce4windowingui-0" else "libxfce4windowing_verinfo"
        source = root / "src/Applications/xfce" / project / version / "configure.ac"
        match = re.search(r"m4_define\(\[" + variable + r"\], \[(\d+):(\d+):(\d+)\]\)", source.read_text())
        if not match:
            raise ValueError("original desktop library interface version missing: " + name)
        interface, revision, age = map(int, match.groups()); major = str(interface - age)
        compatibility, current = f"{interface+1}.0.0", f"{interface+1}.{revision}.0"
    return {"major": major, "compatibility": compatibility, "current": current, "source": str(source.relative_to(root)), "source_sha256": digest(source)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "build-report", "runtime", "runtime-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    previous = json.loads(args.runtime_report.read_text()); compiled = json.loads(args.build_report.read_text())
    if previous["architecture"] != args.arch or digest(args.runtime) != previous["bundle_sha256"]:
        raise ValueError("previous native desktop runtime mismatch")
    if compiled["architecture"] != args.arch or digest(args.libraries) != compiled["bundle_sha256"]:
        raise ValueError("native desktop objects mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-xfce-desktop-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); tree, objects = work / "runtime", work / "objects"
        for path, destination in ((args.runtime, tree), (args.libraries, objects), (args.system, tree)):
            with tarfile.open(path) as archive:
                archive.extractall(destination, filter="data")
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        records = dict(previous["libraries"]); programs = dict(previous["programs"]); modules = dict(previous["modules"])
        added = compiled.get("libraries", {}); ordered, visiting = [], set()
        def visit(name):
            if name in ordered:
                return
            if name in visiting:
                raise ValueError("cyclic native desktop library graph: " + name)
            visiting.add(name)
            for other, record in added.items():
                if other != name and set(added[name]["imports"]) & set(record["exports"]):
                    visit(other)
            visiting.remove(name); ordered.append(name)
        for name in added:
            visit(name)
        def providers(archive):
            needed = set()
            for symbol in symbols(archive)[1] - definitions(archive) - {"___dso_handle"} - loader:
                matches = {path for path, image in images.items() if path.endswith(".dylib") and symbol in image["exports"]}
                if len(matches) != 1:
                    raise ValueError("native desktop API requires one genuine provider: " + symbol + ": " + repr(matches))
                needed |= matches
            return needed
        common = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip_dylibs",
                  "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup",
                  "-needed_library", tree / "usr/lib/libSystem.B.dylib"]
        for name in ordered:
            record = added[name]; archive = objects / "usr/lib" / (name + ".a")
            if digest(archive) != record["archive_sha256"]:
                raise ValueError("native desktop library archive changed: " + name)
            version = metadata(root, name); path = "/usr/lib/" + name + "." + version["major"] + ".dylib"
            destination = tree / path.lstrip("/"); needed = providers(archive)
            run([*common, "-dylib", "-install_name", path, "-compatibility_version", version["compatibility"], "-current_version", version["current"],
                 "-force_load", archive, *[tree / p.lstrip("/") for p in sorted(needed)], "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib", "-o", destination])
            sign(destination); images[path] = image_metadata(destination.read_bytes())
            records[name] = {"install_path": path, "sha256": digest(destination), "archive_sha256": digest(archive), "version_metadata": version,
                             "exports": sorted(images[path]["exports"]), "linked_dylibs": sorted(needed)}
        for name, record in compiled.get("programs", {}).items():
            archive = objects / record["archive_path"]; path = record["install_path"]
            if digest(archive) != record["archive_sha256"]:
                raise ValueError("native desktop program archive changed: " + name)
            destination = tree / path.lstrip("/"); destination.parent.mkdir(parents=True, exist_ok=True); needed = providers(archive)
            run([*common, "-dead_strip", "-pie", "-e", "_main", "-o", destination, archive, *[tree / p.lstrip("/") for p in sorted(needed)],
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
            sign(destination); images[path] = image_metadata(destination.read_bytes())
            programs[name] = {"install_path": path, "sha256": digest(destination), "archive_sha256": digest(archive), "linked_dylibs": sorted(needed)}
        for name, record in compiled.get("modules", {}).items():
            archive = objects / record["archive_path"]; path = record["install_path"]
            if digest(archive) != record["archive_sha256"]:
                raise ValueError("native desktop module archive changed: " + name)
            destination = tree / path.lstrip("/"); destination.parent.mkdir(parents=True, exist_ok=True); needed = providers(archive)
            run([*common, "-bundle", "-o", destination, "-force_load", archive, *[tree / p.lstrip("/") for p in sorted(needed)],
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
            sign(destination); images[path] = image_metadata(destination.read_bytes())
            modules[name] = {"install_path": path, "sha256": digest(destination), "archive_sha256": digest(archive), "linked_dylibs": sorted(needed)}
        for directory in ("usr/share", "etc/xdg"):
            for source in (objects / directory).rglob("*"):
                if source.is_file():
                    destination = tree / source.relative_to(objects); destination.parent.mkdir(parents=True, exist_ok=True)
                    if not destination.exists():
                        destination.write_bytes(source.read_bytes())
        all_records = [*records.values(), *programs.values(), *modules.values(), previous["launch_desktop"]]
        for record in all_records:
            path = record["install_path"]; file = tree / path.lstrip("/")
            if digest(file) != record["sha256"]:
                raise ValueError("unexpected retained native image change: " + path)
            images[path] = image_metadata(file.read_bytes())
        for record in all_records:
            path = record["install_path"]
            record["import_resolutions"] = verify_imports(path, images, loader, flat_namespace_root=path)
        for record in [*programs.values(), *modules.values(), previous["launch_desktop"]]:
            record["startup_closure"] = verify_closure(record["install_path"], images, loader)
        pack(tree, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": records, "programs": programs, "modules": modules,
            "launch_desktop": previous["launch_desktop"], "bundle_sha256": digest(args.output), "runtime_checked": False, "desktop_checked": False,
            "scope": "Original native desktop dependencies and programs with complete signed Mach-O binding audits; guest execution is verified separately."}, indent=2) + "\n")
        print(f"Native Xfce desktop runtime {args.arch}: {len(records)} application libraries, {len(programs)} programs, {len(modules)} modules")

if __name__ == "__main__":
    main()
