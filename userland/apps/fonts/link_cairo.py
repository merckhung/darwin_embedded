#!/usr/bin/env python3
"""Link the complete native Cairo/font/X11 stack with real import providers."""
import argparse
import json
from pathlib import Path
import re
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glib"))
from link_dependencies import versions
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "graphics"))
from link_libraries import library_versions
from build_libraries import digest, pack
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "native/resolver"))
from link_clients import version as x11_version
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign
from build_dependencies import definitions
from build_harfbuzz import LIBRARIES as HARFBUZZ_LIBRARIES
from build_cairo import X11_RELEASES


def metadata_for(root, names):
    metadata = {**versions(root), **library_versions(root)}
    source = root / "src/Applications/glib/glib/2.90.1/meson.build"
    if "darwin_versions = [current + 1, '@0@.@1@'.format(current + 1, interface_age)]" not in source.read_text():
        raise ValueError("GLib Darwin version convention changed")
    _, minor, micro = map(int, json.loads((source.parent / "UPSTREAM.json").read_text())["version"].split("."))
    interface_age = 0 if minor % 2 else micro
    current = 100 * minor + micro - interface_age
    for name in ("libglib-2.0", "libgobject-2.0", "libgmodule-2.0"):
        metadata[name] = {"major": "0", "compatibility": f"{current + 1}.0.0", "current": f"{current + 1}.{interface_age}.0",
                          "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    source = root / "src/Applications/fonts/harfbuzz/14.6.0/src/meson.build"
    root_source = source.parent.parent / "meson.build"
    if "darwin_versions = [hb_version_int, '@0@.0.0'.format(hb_version_int)]" not in source.read_text() or \
       "hb_version_int = 60000 + hb_version_major*100 + hb_version_minor*10 + hb_version_micro" not in root_source.read_text():
        raise ValueError("HarfBuzz Darwin version convention changed")
    major, minor, micro = map(int, json.loads((source.parent.parent / "UPSTREAM.json").read_text())["version"].split("."))
    version = f"{60000 + major * 100 + minor * 10 + micro}.0.0"
    for name in HARFBUZZ_LIBRARIES:
        metadata[name] = {"major": "0", "compatibility": version, "current": version,
                          "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    source = root / "src/Applications/fonts/fontconfig/2.18.3/meson.build"
    if "darwin_versions: osxversion" not in source.read_text() or "osxversion = curversion + 1" not in source.read_text():
        raise ValueError("Fontconfig Darwin version convention changed")
    major, minor, _ = map(int, json.loads((source.parent / "UPSTREAM.json").read_text())["version"].split("."))
    metadata["libfontconfig"] = {"major": str(major - 1), "compatibility": f"{minor}.0.0", "current": f"{minor}.0.0",
                                "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    x11 = root / "src/Applications/x11"
    for name in names:
        if name in metadata or name.startswith("libcairo"): continue
        if name in ("libXau", "libXdmcp"):
            release = "1.0.12" if name == "libXau" else "1.1.5"
            source = x11 / name / release / "Makefile.am"
        elif name.startswith("libxcb"):
            source = x11 / "libxcb/1.17.0/src/Makefile.am"
        elif name in ("libX11", "libX11-xcb"):
            source = x11 / "libX11/1.8.13/src/Makefile.am"
        elif name in X11_RELEASES:
            source = x11 / name / X11_RELEASES[name] / "src/Makefile.am"
        else: raise ValueError("unknown native library: " + name)
        if name == "libXext":
            configure = source.parent.parent / "configure.ac"
            match = re.search(r"\*\)\s+XEXT_SOREV=(\d+):(\d+):(\d+)", configure.read_text())
            if not match or "-version-number $(XEXT_SOREV)" not in source.read_text():
                raise ValueError("Xext default Darwin interface changed")
            major, minor, revision = map(int, match.groups())
            compatibility, current = f"{major + minor + 1}.0.0", f"{major + minor + 1}.{revision}.0"
            major = str(major)
            source = configure
        else: major, compatibility, current = x11_version(source, name.replace("-", "_"))
        metadata[name] = {"major": major, "compatibility": compatibility, "current": current,
                          "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    source = root / "src/Applications/fonts/cairo/1.18.6/meson.build"
    if "cairo_darwin_versions = ['1.0', '@0@.@1@.@2@'.format(cairo_version_major, cairo_version_minor, cairo_version_micro)]" not in source.read_text():
        raise ValueError("Cairo Darwin interface convention changed")
    version = json.loads((source.parent / "UPSTREAM.json").read_text())["version"]
    major = str(int(version.split(".")[0]) + 1)
    for name in ("libcairo", "libcairo-gobject", "libcairo-script-interpreter"):
        metadata[name] = {"major": major, "compatibility": "1.0.0", "current": version,
                          "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    if set(names) != set(metadata): raise ValueError("unexpected Cairo archive inventory")
    return metadata


def link_stack(args, metadata_provider=metadata_for):
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-cairo-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        libraries, tree = work / "libraries", work / "install"
        for path, destination in ((args.libraries, libraries), (args.system, tree)):
            with tarfile.open(path) as archive: archive.extractall(destination, filter="data")
        archives = {path.stem: path for path in (libraries / "usr/lib").glob("*.a")}
        metadata = metadata_provider(root, archives)
        inventory = {name: symbols(path) for name, path in archives.items()}
        dependencies = {}
        # Derive dependency edges from real public definitions and actual
        # undefined symbols, excluding definitions internal to each archive.
        for name, archive in archives.items():
            imports = inventory[name][1] - definitions(archive) - {"___dso_handle"}
            providers = {symbol: [provider for provider, (exports, _, _) in inventory.items()
                                 if provider != name and symbol in exports] for symbol in imports}
            ambiguous = {symbol: entries for symbol, entries in providers.items() if len(entries) > 1}
            if ambiguous: raise ValueError("ambiguous native providers: " + repr(ambiguous))
            dependencies[name] = sorted({entry for entries in providers.values() for entry in entries})
        ordered, visiting = [], set()
        def visit(name):
            if name in ordered: return
            if name in visiting: raise ValueError("cyclic native Cairo dependencies: " + name)
            visiting.add(name)
            for dependency in dependencies[name]: visit(dependency)
            visiting.remove(name); ordered.append(name)
        for name in sorted(archives): visit(name)
        loader = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes())
                  for path in (tree / "usr/lib").rglob("*.dylib")}
        records, built = {}, {}
        for name in ordered:
            version = metadata[name]
            image = tree / "usr/lib" / (name + "." + version["major"] + ".dylib")
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-dylib", "-no_fixup_chains",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup",
                 "-install_name", "/usr/lib/" + image.name, "-compatibility_version", version["compatibility"],
                 "-current_version", version["current"], "-force_load", archives[name],
                 *[built[key] for key in dependencies[name]], "-L", tree / "usr/lib/system",
                 tree / "usr/lib/libSystem.B.dylib", "-o", image])
            sign(image)
            public, imports, weak = symbols(image)
            if public - {"___dso_handle"} != inventory[name][0]: raise ValueError("native library lost public APIs: " + name)
            path = "/usr/lib/" + image.name
            images[path] = image_metadata(image.read_bytes())
            resolutions = verify_imports(path, images, loader)
            if {entry["symbol"] for entry in resolutions if not entry["weak"]} != imports:
                raise ValueError("Cairo import audits disagree")
            records[name] = {"install_path": path, "sha256": digest(image), "archive_sha256": digest(archives[name]),
                             "exports": sorted(public), "imports": sorted(imports), "weak_imports": sorted(weak),
                             "dependencies": dependencies[name], "import_resolutions": resolutions, "version_metadata": version}
            built[name] = image
        for path in libraries.rglob("*"):
            relative = path.relative_to(libraries)
            if path.is_file() and str(relative).startswith(("etc/", "usr/share/")):
                destination = tree / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(path.read_bytes())
        # A GIO-bearing stack also needs the genuine native subprocess
        # helper. Its object is built by the existing original GIO inventory.
        helper_object = libraries / "usr/libexec/gio-launch-desktop.o"
        helper_record = None
        if helper_object.exists():
            helper = tree / "usr/libexec/gio-launch-desktop"
            helper.parent.mkdir(parents=True, exist_ok=True)
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
                 "-undefined", "dynamic_lookup", "-o", helper, helper_object, "-L", tree / "usr/lib/system",
                 tree / "usr/lib/libSystem.B.dylib"])
            sign(helper)
            path = "/usr/libexec/gio-launch-desktop"
            images[path] = image_metadata(helper.read_bytes())
            helper_record = {"install_path": path, "sha256": digest(helper), "object_sha256": digest(helper_object),
                             "import_resolutions": verify_imports(path, images, loader)}
        pack(tree, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": records,
            "source_bundle_sha256": digest(args.libraries), "system_sha256": digest(args.system), "dyld_sha256": digest(args.dyld),
            "bundle_sha256": digest(args.output), "runtime_checked": False,
            "launch_desktop": helper_record,
            "validation": "Complete signed native archives retain all public APIs; dependency edges derive from real source symbols; every strong ordinal/reexport binding resolves."}, indent=2) + "\n")
        print(f"Native Cairo {args.arch}: {len(records)} complete signed dylibs, all actual strong bindings verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    link_stack(parser.parse_args())


if __name__ == "__main__":
    main()
