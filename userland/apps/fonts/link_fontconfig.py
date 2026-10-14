#!/usr/bin/env python3
"""Link complete native Fontconfig and dependencies, verifying actual bindings."""
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
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    metadata = {**versions(root), **library_versions(root)}
    source = root / "src/Applications/fonts/fontconfig/2.18.3/meson.build"
    recipe = source.read_text()
    if "darwin_versions: osxversion" not in recipe or "osxversion = curversion + 1" not in recipe:
        raise ValueError("Fontconfig Darwin version convention changed")
    release = json.loads((source.parent / "UPSTREAM.json").read_text())["version"]
    major, minor, _ = map(int, release.split("."))
    metadata["libfontconfig"] = {"major": str(major - 1), "compatibility": f"{minor}.0.0", "current": f"{minor}.0.0",
                                "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    dependencies = {"libintl": ["libiconv"], "libpng16": ["libz"], "libfreetype": ["libz", "libpng16"],
                    "libfontconfig": ["libfreetype", "libexpat", "libintl", "libiconv"]}
    ordered, visiting = [], set()
    def visit(name):
        if name in ordered: return
        if name in visiting or name not in metadata:
            raise ValueError("invalid Fontconfig dependency graph: " + name)
        visiting.add(name)
        for dependency in dependencies.get(name, []): visit(dependency)
        visiting.remove(name)
        ordered.append(name)
    for name in metadata: visit(name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-fontconfig-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        libraries, tree = work / "libraries", work / "install"
        for path, destination in ((args.libraries, libraries), (args.system, tree)):
            with tarfile.open(path) as archive:
                archive.extractall(destination, filter="data")
        if {path.stem for path in (libraries / "usr/lib").glob("*.a")} != set(metadata):
            raise ValueError("unexpected Fontconfig archive inventory")
        loader = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes())
                  for path in (tree / "usr/lib").rglob("*.dylib")}
        records, built = {}, {}
        for name in ordered:
            version = metadata[name]
            archive = libraries / "usr/lib" / (name + ".a")
            image = tree / "usr/lib" / (name + "." + version["major"] + ".dylib")
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-dylib", "-no_fixup_chains",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup",
                 "-install_name", "/usr/lib/" + image.name, "-compatibility_version", version["compatibility"],
                 "-current_version", version["current"], "-force_load", archive,
                 *[built[key] for key in dependencies.get(name, [])], "-L", tree / "usr/lib/system",
                 tree / "usr/lib/libSystem.B.dylib", "-o", image])
            sign(image)
            public, imports, weak = symbols(image)
            if public - {"___dso_handle"} != symbols(archive)[0]:
                raise ValueError("native Fontconfig library lost public APIs: " + name)
            path = "/usr/lib/" + image.name
            images[path] = image_metadata(image.read_bytes())
            resolutions = verify_imports(path, images, loader)
            if {entry["symbol"] for entry in resolutions if not entry["weak"]} != imports:
                raise ValueError("Fontconfig import audits disagree")
            records[name] = {"install_path": path, "sha256": digest(image), "archive_sha256": digest(archive),
                             "exports": sorted(public), "imports": sorted(imports), "weak_imports": sorted(weak),
                             "import_resolutions": resolutions, "version_metadata": version}
            built[name] = image
        for path in libraries.rglob("*"):
            relative = path.relative_to(libraries)
            if path.is_file() and (str(relative).startswith("etc/") or str(relative).startswith("usr/share/")):
                destination = tree / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(path.read_bytes())
        pack(tree, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": records,
            "source_bundle_sha256": digest(args.libraries), "system_sha256": digest(args.system), "dyld_sha256": digest(args.dyld),
            "bundle_sha256": digest(args.output), "runtime_checked": False,
            "validation": "Complete source archives; native signed dylibs retain all public APIs; strong imports resolve through actual ordinals/reexports."}, indent=2) + "\n")
        print(f"Native Fontconfig {args.arch}: {len(records)} complete signed dylibs, all strong bindings verified")


if __name__ == "__main__":
    main()
