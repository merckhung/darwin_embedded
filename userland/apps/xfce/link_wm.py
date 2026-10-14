#!/usr/bin/env python3
"""Link original Xfce window-manager programs and native windowing providers."""
import argparse
import json
from pathlib import Path
import re
import sys
import tarfile
import tempfile

from build_base import run, symbols, digest, definitions, pack
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from sign_diagnostic_macho import sign

def metadata(root, name):
    if name == "libxcb-util":
        source = root / "src/Applications/xfce-prerequisites/xcb-util/0.4.1/src/Makefile.am"
        version = re.search(r"-version-info (\d+):(\d+):(\d+)", source.read_text())
        if not version:
            raise ValueError("original XCB utility interface version missing")
        current, revision, age = map(int, version.groups())
    elif name == "libstartup-notification-1":
        source = root / "src/Applications/xfce-prerequisites/startup-notification/0.12/libsn/Makefile.am"
        if "-version-info" in source.read_text():
            raise ValueError("startup notification default interface version changed")
        current = revision = age = 0
    else:
        source = root / "src/Applications/xfce-prerequisites/libwnck/43.3/meson.build"
        text = source.read_text()
        current, revision, age = [int(re.search(r"^LIBWNCK_" + variable + r"\s*=\s*(\d+)", text, re.M)[1]) for variable in ("CURRENT", "REVISION", "AGE")]
    return {"major": str(current-age), "compatibility": f"{current+1}.0.0", "current": f"{current+1}.{revision}.0",
            "source": str(source.relative_to(root)), "source_sha256": digest(source)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "build-report", "windowing-report", "runtime", "runtime-report", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    previous = json.loads(args.runtime_report.read_text()); compiled = json.loads(args.build_report.read_text()); windowing = json.loads(args.windowing_report.read_text())
    if previous["architecture"] != args.arch or digest(args.runtime) != previous["bundle_sha256"]:
        raise ValueError("native Xfce UI runtime mismatch")
    if compiled["architecture"] != args.arch or digest(args.libraries) != compiled["bundle_sha256"] or windowing["architecture"] != args.arch:
        raise ValueError("native window-manager object mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-xfwm-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); tree, objects = work / "runtime", work / "objects"
        for path, destination in ((args.runtime, tree), (args.libraries, objects)):
            with tarfile.open(path) as archive:
                archive.extractall(destination, filter="data")
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        records = dict(previous["libraries"]); programs = dict(previous["programs"])
        ordered, visiting = [], set()
        def visit(name):
            if name in ordered:
                return
            if name in visiting:
                raise ValueError("cyclic native windowing libraries")
            visiting.add(name)
            for other, record in windowing["libraries"].items():
                if other != name and set(windowing["libraries"][name]["imports"]) & set(record["exports"]):
                    visit(other)
            visiting.remove(name); ordered.append(name)
        for name in windowing["libraries"]:
            visit(name)
        def providers(archive):
            needed = set()
            for symbol in symbols(archive)[1] - definitions(archive) - {"___dso_handle", "dyld_stub_binder"}:
                matches = {path for path, image in images.items() if path.endswith(".dylib") and symbol in image["exports"]}
                if len(matches) != 1:
                    raise ValueError("native window-manager API requires one genuine provider: " + symbol + ": " + repr(matches))
                needed |= matches
            return needed
        common = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip_dylibs",
                  "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup"]
        for name in ordered:
            archive = objects / "usr/lib" / (name + ".a"); record = windowing["libraries"][name]
            if digest(archive) != record["archive_sha256"]:
                raise ValueError("native windowing archive changed")
            version = metadata(root, name); path = "/usr/lib/" + name + "." + version["major"] + ".dylib"
            destination = tree / path.lstrip("/"); needed = providers(archive)
            run([*common, "-dylib", "-install_name", path, "-compatibility_version", version["compatibility"], "-current_version", version["current"],
                 "-force_load", archive, *[tree / p.lstrip("/") for p in sorted(needed)], "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib", "-o", destination])
            sign(destination); images[path] = image_metadata(destination.read_bytes())
            records[name] = {"install_path": path, "sha256": digest(destination), "archive_sha256": digest(archive), "version_metadata": version,
                             "exports": sorted(images[path]["exports"]), "linked_dylibs": sorted(needed)}
        for name, record in compiled["programs"].items():
            archive = objects / "usr/libexec/xfce" / (name + ".a")
            if digest(archive) != record["archive_sha256"]:
                raise ValueError("native xfwm4 program archive changed")
            path = "/usr/libexec/xfce4/xfwm4/helper-dialog" if name == "helper-dialog" else "/usr/bin/" + name
            destination = tree / path.lstrip("/"); destination.parent.mkdir(parents=True, exist_ok=True); needed = providers(archive)
            run([*common, "-dead_strip", "-pie", "-e", "_main", "-o", destination, archive, *[tree / p.lstrip("/") for p in sorted(needed)],
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
            sign(destination); images[path] = image_metadata(destination.read_bytes())
            programs[name] = {"install_path": path, "sha256": digest(destination), "archive_sha256": digest(archive), "linked_dylibs": sorted(needed)}
        for directory in ("usr/share", "etc/xdg"):
            for source in (objects / directory).rglob("*"):
                if source.is_file():
                    destination = tree / source.relative_to(objects); destination.parent.mkdir(parents=True, exist_ok=True)
                    if not destination.exists():
                        destination.write_bytes(source.read_bytes())
        modules = dict(previous["modules"]); loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        all_records = [*records.values(), *programs.values(), *modules.values(), previous["launch_desktop"]]
        for record in all_records:
            path = record["install_path"]; file = tree / path.lstrip("/")
            if digest(file) != record["sha256"]:
                raise ValueError("unexpected retained native image change: " + path)
            images[path] = image_metadata(file.read_bytes())
        for record in all_records:
            path = record["install_path"]; record["import_resolutions"] = verify_imports(path, images, loader, flat_namespace_root=path)
        pack(tree, args.output)
        args.report.write_text(json.dumps({**previous, "libraries": records, "programs": programs, "modules": modules, "bundle_sha256": digest(args.output),
            "runtime_checked": False, "desktop_checked": False, "xfwm4": {"build_report_sha256": digest(args.build_report),
            "windowing_report_sha256": digest(args.windowing_report), "previous_runtime_sha256": digest(args.runtime), "features": compiled["features"]}}, indent=2) + "\n")
        print(f"Native xfwm4 {args.arch}: {len(records)} signed app libraries and {len(programs)} native programs; original manager/helper/settings linked")

if __name__ == "__main__":
    main()
