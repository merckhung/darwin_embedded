#!/usr/bin/env python3
"""Link the genuine Xfce UI, keyboard and ICE/SM session libraries as Mach-O."""
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
    if name in ("libICE", "libSM"):
        version = "1.1.2" if name == "libICE" else "1.2.6"
        source = root / "src/Applications/xfce-prerequisites" / name / version / "src/Makefile.am"
        match = re.search(r"-version-number (\d+):(\d+):(\d+)", source.read_text())
        if not match:
            raise ValueError("original ICE/SM library version missing")
        major, minor, revision = match.groups()
        compatibility, current = f"{major}.{minor}.0", f"{major}.{minor}.{revision}"
    else:
        source = root / "src/Applications/xfce/libxfce4ui/4.20.2/configure.ac"
        variable = "libxfce4ui_verinfo" if name == "libxfce4ui-2" else "libxfce4kbd_private_verinfo"
        match = re.search(r"m4_define\(\[" + variable + r"\],\s*\[(\d+):(\d+):(\d+)\]\)", source.read_text())
        if not match:
            raise ValueError("original Xfce UI interface version missing")
        version, revision, age = map(int, match.groups()); major = str(version - age)
        compatibility, current = f"{version+1}.0.0", f"{version+1}.{revision}.0"
    return {"major": major, "compatibility": compatibility, "current": current,
            "source": str(source.relative_to(root)), "source_sha256": digest(source)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "build-report", "session-report", "runtime", "runtime-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    previous = json.loads(args.runtime_report.read_text()); compiled = json.loads(args.build_report.read_text())
    sessions = json.loads(args.session_report.read_text())
    if previous["architecture"] != args.arch or digest(args.runtime) != previous["bundle_sha256"]:
        raise ValueError("native Xfce base runtime mismatch")
    if compiled["architecture"] != args.arch or digest(args.libraries) != compiled["bundle_sha256"] or sessions["architecture"] != args.arch:
        raise ValueError("native Xfce UI/session object mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-xfce-ui-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); tree, objects = work / "runtime", work / "objects"
        for path, destination in ((args.runtime, tree), (args.libraries, objects), (args.system, tree)):
            with tarfile.open(path) as archive:
                archive.extractall(destination, filter="data")
        definitions_by_library = {**sessions["libraries"], **compiled["libraries"]}
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        records = dict(previous["libraries"]); ordered, visiting = [], set()
        def visit(name):
            if name in ordered:
                return
            if name in visiting:
                raise ValueError("cyclic native UI/session library graph: " + name)
            visiting.add(name)
            for other, record in definitions_by_library.items():
                if other != name and set(definitions_by_library[name]["imports"]) & set(record["exports"]):
                    visit(other)
            visiting.remove(name); ordered.append(name)
        for name in definitions_by_library:
            visit(name)
        for name in ordered:
            archive = objects / "usr/lib" / (name + ".a"); compiled_record = definitions_by_library[name]
            if digest(archive) != compiled_record["archive_sha256"]:
                raise ValueError("native UI/session archive changed: " + name)
            version = metadata(root, name); path = "/usr/lib/" + name + "." + version["major"] + ".dylib"
            destination = tree / path.lstrip("/"); needed = set()
            for symbol in symbols(archive)[1] - definitions(archive) - {"___dso_handle", "dyld_stub_binder"}:
                matches = {candidate for candidate, image in images.items() if symbol in image["exports"]}
                if len(matches) != 1:
                    raise ValueError("UI/session API requires one genuine provider: " + symbol + ": " + repr(matches))
                needed |= matches
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-dylib", "-no_fixup_chains", "-dead_strip_dylibs",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup", "-install_name", path,
                 "-compatibility_version", version["compatibility"], "-current_version", version["current"], "-force_load", archive,
                 *[tree / candidate.lstrip("/") for candidate in sorted(needed)], "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib", "-o", destination])
            sign(destination); images[path] = image_metadata(destination.read_bytes())
            records[name] = {"install_path": path, "sha256": digest(destination), "archive_sha256": digest(archive),
                             "exports": sorted(images[path]["exports"]), "version_metadata": version, "linked_dylibs": sorted(needed)}
        for directory in ("usr/share/licenses", "etc/xdg"):
            for source in (objects / directory).rglob("*"):
                if source.is_file():
                    destination = tree / source.relative_to(objects); destination.parent.mkdir(parents=True, exist_ok=True)
                    if not destination.exists():
                        destination.write_bytes(source.read_bytes())
        font_source = root / "src/Applications/xfce-prerequisites/dejavu-fonts/2.37"
        font_manifest = json.loads((font_source / "UPSTREAM.json").read_text())
        font_records = {}
        for relative, expected in font_manifest["upstream_files_sha256"].items():
            source = font_source / relative
            if digest(source) != expected:
                raise ValueError("modified original desktop font data: " + relative)
            path = ("/usr/share/fonts/dejavu/" + source.name if source.suffix == ".ttf" else
                    "/usr/share/licenses/dejavu-fonts/LICENSE" if relative == "LICENSE" else
                    "/etc/fonts/conf.d/" + source.name if relative.startswith("fontconfig/") and source.suffix == ".conf" else None)
            if path:
                destination = tree / path.lstrip("/"); destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(source.read_bytes())
                font_records[path] = {"source": str(source.relative_to(root)), "sha256": expected}
        config_source = root / "src/Applications/fonts/fontconfig/2.18.3/conf.d"
        recipe = (config_source / "meson.build").read_text()
        selected = re.search(r"conf_links\s*=\s*\[(.*?)\]", recipe, re.S)
        if not selected:
            raise ValueError("original Fontconfig default configuration list missing")
        configs = re.findall(r"'([^']+\.conf)'", selected[1])
        options = (config_source.parent / "meson.options").read_text()
        def default_option(name):
            match = re.search(r"option\('" + name + r"'.*?value:\s*'([^']+)'", options, re.S)
            if not match:
                raise ValueError("original Fontconfig default missing: " + name)
            return match[1]
        bitmap = {"yes": "yes-bitmaps", "no": "no-bitmaps-and-emoji", "no-except-emoji": "no-bitmaps-except-emoji"}
        configs += ["05-macos.conf", "10-hinting-" + default_option("default-hinting") + ".conf",
                    "10-sub-pixel-" + default_option("default-sub-pixel-rendering") + ".conf",
                    "70-" + bitmap[default_option("bitmap-conf")] + ".conf"]
        for name in configs:
            source = config_source / name; path = "/etc/fonts/conf.d/" + name
            destination = tree / path.lstrip("/"); destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(source.read_bytes())
            font_records[path] = {"source": str(source.relative_to(root)), "sha256": digest(source)}
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        programs, modules = dict(previous["programs"]), dict(previous["modules"])
        all_records = [*records.values(), *programs.values(), *modules.values(), previous["launch_desktop"]]
        for record in all_records:
            path = record["install_path"]; file = tree / path.lstrip("/")
            if digest(file) != record["sha256"]:
                raise ValueError("unexpected retained runtime change: " + path)
            images[path] = image_metadata(file.read_bytes())
        for record in all_records:
            path = record["install_path"]
            record["import_resolutions"] = verify_imports(path, images, loader, flat_namespace_root=path)
        pack(tree, args.output)
        args.report.write_text(json.dumps({**previous, "libraries": records, "programs": programs, "modules": modules,
            "bundle_sha256": digest(args.output), "system_sha256": digest(args.system), "runtime_checked": False, "desktop_checked": False,
            "xfce_ui": {"build_report_sha256": digest(args.build_report), "session_report_sha256": digest(args.session_report),
                        "previous_runtime_sha256": digest(args.runtime), "features": compiled["features"], "desktop_font_data": font_records}}, indent=2) + "\n")
        print(f"Native Xfce UI {args.arch}: {len(records)} signed app libraries including original GTK X11 UI, keyboard and ICE/SM")

if __name__ == "__main__":
    main()
