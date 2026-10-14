#!/usr/bin/env python3
"""Link complete GLib dependency archives, auditing each native dylib import."""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import tarfile
import tempfile

from build_dependencies import digest, VERSIONS
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from sign_diagnostic_macho import sign


def versions(root):
    upstream = root / "src/Applications/glib"
    paths = {
        "libpcre2-8": (upstream / "pcre2" / VERSIONS["pcre2"] / "configure.ac", r"m4_define\(libpcre2_8_version,\s*\[(\d+):(\d+):(\d+)\]\)"),
        "libffi": (upstream / "libffi" / VERSIONS["libffi"] / "libtool-version", r"^(\d+):(\d+):(\d+)$"),
        "libiconv": (upstream / "libiconv" / VERSIONS["libiconv"] / "lib/Makefile.in", r"^LIBICONV_VERSION_INFO = (\d+):(\d+):(\d+)$"),
    }
    result = {}
    for name, (path, pattern) in paths.items():
        match = re.search(pattern, path.read_text(), re.M)
        if not match:
            raise ValueError("missing upstream libtool interface: " + name)
        current, revision, age = map(int, match.groups())
        if current < age:
            raise ValueError("invalid libtool age")
        result[name] = {"major": str(current - age), "compatibility": f"{current + 1}.0.0",
                        "current": f"{current + 1}.{revision}.0", "source": str(path.relative_to(root)),
                        "source_sha256": digest(path)}
    path = upstream / "gettext" / VERSIONS["gettext"] / "gettext-runtime/intl/Makefile.am"
    current, revision, age = [int(re.search(r"^LTV_" + key + r"=(\d+)$", path.read_text(), re.M)[1])
                              for key in ("CURRENT", "REVISION", "AGE")]
    result["libintl"] = {"major": str(current - age), "compatibility": f"{current + 1}.0.0",
                         "current": f"{current + 1}.{revision}.0", "source": str(path.relative_to(root)),
                         "source_sha256": digest(path)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--core", action="store_true")
    parser.add_argument("--gio", action="store_true")
    args = parser.parse_args()
    metadata = versions(args.root.resolve())
    if args.core or args.gio:
        source = args.root.resolve() / "src/Applications/glib/glib/2.90.1/meson.build"
        recipe = source.read_text()
        if "darwin_versions = [current + 1, '@0@.@1@'.format(current + 1, interface_age)]" not in recipe:
            raise ValueError("GLib Darwin interface version convention changed")
        release = json.loads((source.parent / "UPSTREAM.json").read_text())["version"]
        major, minor, micro = map(int, release.split("."))
        interface_age = 0 if minor % 2 else micro
        current = 100 * minor + micro - interface_age
        for name in ("libglib-2.0", "libgobject-2.0", "libgmodule-2.0"):
            metadata[name] = {"major": "0", "compatibility": f"{current + 1}.0.0",
                              "current": f"{current + 1}.{interface_age}.0",
                              "source": str(source.relative_to(args.root.resolve())), "source_sha256": digest(source)}
        if args.gio:
            zlib = args.root.resolve() / "src/Applications/graphics/zlib/1.3.2"
            version = json.loads((zlib / "UPSTREAM.json").read_text())["version"]
            if "-compatibility_version $VER1 -current_version $VER3" not in (zlib / "configure").read_text():
                raise ValueError("zlib Darwin version convention changed")
            metadata["libz"] = {"major": "1", "compatibility": "1.0.0", "current": version,
                                "source": str((zlib / "configure").relative_to(args.root.resolve())),
                                "source_sha256": digest(zlib / "configure")}
            metadata["libgio-2.0"] = dict(metadata["libglib-2.0"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-glib-runtime-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        libraries, tree = work / "libraries", work / "install"
        for source, destination in ((args.libraries, libraries), (args.system, tree)):
            with tarfile.open(source) as archive:
                archive.extractall(destination, filter="data")
        if {path.stem for path in (libraries / "usr/lib").glob("*.a")} != set(metadata):
            raise ValueError("unexpected GLib dependency archive inventory")
        loader_exports = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes())
                  for path in (tree / "usr/lib").rglob("*.dylib")}
        records, built = {}, {}
        dependencies = {"libintl": ["libiconv"], "libglib-2.0": ["libpcre2-8", "libiconv", "libintl"],
                        "libgobject-2.0": ["libglib-2.0", "libffi"], "libgmodule-2.0": ["libglib-2.0"],
                        "libgio-2.0": ["libglib-2.0", "libgobject-2.0", "libgmodule-2.0", "libz"]}
        for name, version in metadata.items():
            source = libraries / "usr/lib" / (name + ".a")
            image = tree / "usr/lib" / (name + "." + version["major"] + ".dylib")
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-dylib", "-no_fixup_chains",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                 "-undefined", "dynamic_lookup", "-install_name", "/usr/lib/" + image.name,
                 "-compatibility_version", version["compatibility"], "-current_version", version["current"],
                 "-force_load", source, *[built[dependency] for dependency in dependencies.get(name, [])],
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib", "-o", image])
            sign(image)
            exports, imports, weak = symbols(image)
            if exports - {"___dso_handle"} != symbols(source)[0]:
                raise ValueError("dependency public API inventory changed: " + name)
            install_path = "/usr/lib/" + image.name
            images[install_path] = image_metadata(image.read_bytes())
            resolutions = verify_imports(install_path, images, loader_exports)
            if {entry["symbol"] for entry in resolutions if not entry["weak"]} != imports:
                raise ValueError("dependency binding audits disagree")
            records[name] = {"install_path": install_path, "sha256": digest(image),
                             "source_archive_sha256": digest(source), "exports": sorted(exports),
                             "imports": sorted(imports), "weak_imports": sorted(weak),
                             "import_resolutions": resolutions, "version_metadata": version}
            built[name] = image
        if args.gio:
            helper = tree / "usr/libexec/gio-launch-desktop"
            helper.parent.mkdir()
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
                 "-undefined", "dynamic_lookup", "-o", helper, libraries / "usr/libexec/gio-launch-desktop.o",
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
            sign(helper)
            install_path = "/usr/libexec/gio-launch-desktop"
            images[install_path] = image_metadata(helper.read_bytes())
            helper_record = {"install_path": install_path, "sha256": digest(helper),
                             "object_sha256": digest(libraries / "usr/libexec/gio-launch-desktop.o"),
                             "import_resolutions": verify_imports(install_path, images, loader_exports)}
        else:
            helper_record = None
        shutil.copytree(libraries / "usr/share/licenses", tree / "usr/share/licenses", dirs_exist_ok=True)
        bundle = work / "runtime.tar"
        with tarfile.open(bundle, "w") as archive:
            for path in sorted(tree.rglob("*")):
                if path.is_file():
                    info = archive.gettarinfo(path, str(path.relative_to(tree)))
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ""
                    info.mode = 0o755 if path.suffix == ".dylib" or path.parent.name == "libexec" else 0o644
                    with path.open("rb") as source:
                        archive.addfile(info, source)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": records,
            "archives_sha256": digest(args.libraries), "system_sha256": digest(args.system),
            "dyld_sha256": digest(args.dyld), "bundle_sha256": digest(bundle), "runtime_checked": False,
            "launch_desktop": helper_record,
            "validation": "Complete archives force-loaded into signed native dylibs; every public API retained; every strong import audited against actual dependency ordinal/reexports."}, indent=2) + "\n")
        bundle.replace(args.output)
        print(f"Native GLib dependencies {args.arch}: {len(records)} complete signed dylibs; actual strong bindings verified")


if __name__ == "__main__":
    main()
