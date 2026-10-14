#!/usr/bin/env python3
"""Link complete graphics source archives and audit actual Mach-O bindings."""
import argparse
import json
from pathlib import Path
import re
import struct
import sys
import tarfile
import tempfile

from build_libraries import digest, pack, VERSIONS
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign


def library_versions(root):
    """Read original interface metadata; execute no upstream build systems."""
    upstream = root / "src/Applications/graphics"
    result = {}

    def libtool(name, current, revision, age, source):
        if age > current:
            raise ValueError("invalid graphics interface version")
        result[name] = {"major": str(current - age), "compatibility": f"{current + 1}.0.0",
                        "current": f"{current + 1}.{revision}.0", "source": str(source.relative_to(root)),
                        "source_sha256": digest(source)}

    zlib = upstream / "zlib" / VERSIONS["zlib"] / "configure"
    if "-compatibility_version $VER1 -current_version $VER3" not in zlib.read_text():
        raise ValueError("zlib Darwin version convention changed")
    result["libz"] = {"major": "1", "compatibility": "1.0.0", "current": VERSIONS["zlib"],
                      "source": str(zlib.relative_to(root)), "source_sha256": digest(zlib)}
    expat = upstream / "expat" / VERSIONS["expat"] / "configure.ac"
    values = [int(re.search(r"^" + name + r"=(\d+)", expat.read_text(), re.M)[1])
              for name in ("LIBCURRENT", "LIBREVISION", "LIBAGE")]
    libtool("libexpat", *values, expat)
    freetype = upstream / "freetype" / VERSIONS["freetype"] / "builds/unix/configure.raw"
    match = re.search(r"^version_info='(\d+):(\d+):(\d+)'", freetype.read_text(), re.M)
    if not match:
        raise ValueError("missing FreeType interface version")
    libtool("libfreetype", *(int(value) for value in match.groups()), freetype)
    png = upstream / "libpng" / VERSIONS["libpng"] / "configure.ac"
    major, minor, release = [int(re.search(r"^PNGLIB_" + name + r"=(\d+)", png.read_text(), re.M)[1])
                             for name in ("MAJOR", "MINOR", "RELEASE")]
    # Original Makefile.am uses -version-number MAJORMINOR:RELEASE:0;
    # libtool translates it to current=(MAJORMINOR+RELEASE), age=RELEASE.
    png_inventory = png.parent / "Makefile.am"
    if "-version-number @PNGLIB_MAJOR@@PNGLIB_MINOR@:@PNGLIB_RELEASE@:0" not in png_inventory.read_text():
        raise ValueError("libpng interface version convention changed")
    libtool("libpng16", int(str(major) + str(minor)) + release, 0, release, png)
    pixman = upstream / "pixman" / VERSIONS["pixman"] / "pixman/meson.build"
    if "version : meson.project_version()" not in pixman.read_text():
        raise ValueError("Pixman interface version convention changed")
    version = VERSIONS["pixman"]
    result["libpixman-1"] = {"major": version.split(".")[0], "compatibility": "0.0.0", "current": version,
                             "source": str(pixman.relative_to(root)), "source_sha256": digest(pixman)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    versions = library_versions(args.root.resolve())
    dependencies = {"libz": [], "libexpat": [], "libpng16": ["libz"],
                    "libfreetype": ["libz", "libpng16"], "libpixman-1": []}
    with tempfile.TemporaryDirectory(prefix=".native-graphics-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        libraries, tree = work / "libraries", work / "install"
        for source, destination in ((args.libraries, libraries), (args.system, tree)):
            with tarfile.open(source) as archive:
                archive.extractall(destination, filter="data")
        if {path.stem for path in (libraries / "usr/lib").glob("*.a")} != set(dependencies):
            raise ValueError("unexpected graphics archive inventory")
        loader_exports = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes())
                  for path in (tree / "usr/lib").rglob("*.dylib")}
        built, records = {}, {}
        for name, required in dependencies.items():
            version = versions[name]
            source = libraries / "usr/lib" / (name + ".a")
            image = tree / "usr/lib" / (name + "." + version["major"] + ".dylib")
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-dylib", "-no_fixup_chains",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                 "-undefined", "dynamic_lookup", "-install_name", "/usr/lib/" + image.name,
                 "-compatibility_version", version["compatibility"], "-current_version", version["current"],
                 "-force_load", source, *[built[dependency] for dependency in required],
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib", "-o", image])
            sign(image)
            if struct.unpack_from("<4I", image.read_bytes())[::3] != (0xfeedfacf, 6):
                raise ValueError("graphics runtime image is not Mach-O dylib")
            if struct.unpack_from("<I", image.read_bytes(), 4)[0] != (0x100000c if args.arch == "arm64" else 0x1000007):
                raise ValueError("graphics runtime has wrong native CPU")
            exports, imports, weak = symbols(image)
            if exports - {"___dso_handle"} != symbols(source)[0]:
                raise ValueError("graphics source public API inventory changed: " + name)
            install_path = "/usr/lib/" + image.name
            images[install_path] = image_metadata(image.read_bytes())
            resolutions = verify_imports(install_path, images, loader_exports)
            if {entry["symbol"] for entry in resolutions if not entry["weak"]} != imports:
                raise ValueError("graphics binding audits disagree")
            records[name] = {"install_path": install_path, "sha256": digest(image),
                             "source_archive_sha256": digest(source), "exports": sorted(exports),
                             "imports": sorted(imports), "weak_imports": sorted(weak),
                             "import_resolutions": resolutions, "version_metadata": version}
            built[name] = image
        pack(tree, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "versions": VERSIONS,
            "libraries": records, "archives_sha256": digest(args.libraries), "system_sha256": digest(args.system),
            "dyld_sha256": digest(args.dyld), "bundle_sha256": digest(args.output), "runtime_checked": False,
            "validation": "Complete source archives force-loaded; signed native dylibs retain every public API; each strong import resolves through actual dependency ordinal/reexports."}, indent=2) + "\n")
        print(f"Native graphics {args.arch}: five complete signed dylibs, all actual strong bindings verified")


if __name__ == "__main__":
    main()
