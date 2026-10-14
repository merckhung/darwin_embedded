#!/usr/bin/env python3
"""Build original GdkPixbuf sources and generators as native Mach-O in Bazel."""
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
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glib"))
from build_dependencies import definitions, digest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "fonts"))
from build_cairo import merge_archive
from build_pango import meson_list

VERSION = "2.44.8"
LOADERS = ("png", "bmp", "gif", "ico", "ani", "pnm", "xpm", "xbm", "tga", "icns", "qtif")


def loader_sources(recipe, enabled=LOADERS):
    block = re.search(r"^loaders = \{(.*?)^\}", recipe, re.M | re.S)
    if not block:
        raise ValueError("missing original image loader inventory")
    loaders = {name: re.findall(r"'([^']+\.c)'", body)
               for name, body in re.findall(r"^  '([^']+)': \{(.*?)^  \},", block[1], re.M | re.S)}
    return {name: loaders[name] for name in enabled}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "pango", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--jpeg", type=Path, help="original native JPEG archive and headers")
    args = parser.parse_args()
    loaders = (*LOADERS, "jpeg") if args.jpeg else LOADERS
    root = args.root.resolve()
    source = root / "src/Applications/gtk/gdk-pixbuf" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-pixbuf-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        merge_archive(args.pango, install)
        if args.jpeg:
            merge_archive(args.jpeg, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("GdkPixbuf SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-Wno-deprecated-declarations", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", install / "usr/include", "-I", install / "usr/include/glib-2.0"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        values = {"OS_DARWIN": 1, "USE_GMODULE": 1, "GETTEXT_PACKAGE": '"gdk-pixbuf"',
                  "_GDK_PIXBUF_EXTERN": '__attribute__((visibility("default"))) extern'}
        features = {}
        def probe(name, text):
            path = generated / "feature.c"
            path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if not features[name]:
                raise ValueError(name + ":\n" + result.stderr.decode())
            values[name] = 1
        for header in ("unistd.h", "sys/resource.h", "sys/time.h"):
            probe("HAVE_" + header.replace("/", "_").replace(".", "_").upper(), f"#include <{header}>\n")
        for name, text in {
            "HAVE_ROUND": "#include <math.h>\ndouble f(double x) { return round(x); }\n",
            "HAVE_LRINT": "#include <math.h>\nlong f(double x) { return lrint(x); }\n",
            "HAVE_BIND_TEXTDOMAIN_CODESET": '#include <libintl.h>\nchar *f(void) { return bind_textdomain_codeset("gdk-pixbuf", "UTF-8"); }\n',
            "HAVE_SETRLIMIT": "#include <sys/time.h>\n#include <sys/resource.h>\nint f(struct rlimit *p) { return setrlimit(RLIMIT_NOFILE, p); }\n",
            "HAVE_READLINK": "#include <unistd.h>\nssize_t f(char *p) { return readlink(p, p, 20); }\n",
            "HAVE_SIGSETJMP": "#include <setjmp.h>\nint f(void) { sigjmp_buf b; return sigsetjmp(b, 0); }\n",
        }.items():
            probe(name, text)
        (generated / "config.h").write_text("/* Native Darwin target declarations. */\n" +
            "".join(f"#define {name} {value}\n" for name, value in sorted(values.items())))
        recipe = (source / "gdk-pixbuf/meson.build").read_text()
        headers = meson_list(recipe, "gdkpixbuf_headers") + ["gdk-pixdata.h"]
        include = install / "usr/include/gdk-pixbuf-2.0/gdk-pixbuf"
        include.mkdir(parents=True)
        for header in headers:
            shutil.copyfile(source / "gdk-pixbuf" / header, include / header)
        version_values = {"GDK_PIXBUF_MAJOR": 2, "GDK_PIXBUF_MINOR": 44, "GDK_PIXBUF_MICRO": 8, "GDK_PIXBUF_VERSION": VERSION}
        template = (source / "gdk-pixbuf/gdk-pixbuf-features.h.in").read_text()
        (include / "gdk-pixbuf-features.h").write_text(re.sub(r"@(\w+)@", lambda m: str(version_values[m[1]]), template))
        glib = root / "src/Applications/glib/glib/2.90.1/gobject"
        for name in ("glib-mkenums", "glib-genmarshal"):
            tool = generated / name
            tool.write_text((glib / (name + ".in")).read_text().replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
        for suffix in ("h", "c"):
            destination = generated / ("gdk-pixbuf-enum-types." + suffix)
            with destination.open("w") as stream:
                subprocess.run(["python3", str(generated / "glib-mkenums"), "--template",
                    str(source / "gdk-pixbuf" / (destination.name + ".template")),
                    *[str(source / "gdk-pixbuf" / header) for header in meson_list(recipe, "gdkpixbuf_headers")]], stdout=stream, check=True)
            destination = generated / ("gdk-pixbuf-marshal." + suffix)
            with destination.open("w") as stream:
                subprocess.run(["python3", str(generated / "glib-genmarshal"), "--prefix=_gdk_pixbuf_marshal",
                    "--header" if suffix == "h" else "--body", str(source / "gdk-pixbuf/gdk-pixbuf-marshal.list")], stdout=stream, check=True)
        for path in generated.glob("*.h"):
            if path.name != "config.h":
                shutil.copyfile(path, include / path.name)
        options = ["-I", generated, "-I", include, "-I", include.parent, "-I", source, "-I", source / "gdk-pixbuf",
                   "-fvisibility=hidden", "-DGDK_PIXBUF_COMPILATION", "-DGDK_PIXBUF_ENABLE_BACKEND", "-DG_LOG_STRUCTURED=1",
                   '-DG_LOG_DOMAIN="GdkPixbuf"', '-DGDK_PIXBUF_PREFIX="/usr"', '-DGDK_PIXBUF_LOCALEDIR="/usr/share/locale"',
                   '-DGDK_PIXBUF_LIBDIR="/usr/lib"', '-DGDK_PIXBUF_BINARY_VERSION="2.10.0"',
                   '-DPIXBUF_LIBDIR="/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders"',
                   '-DBUILT_MODULES_DIR="/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders"',
                   *["-DINCLUDE_" + name for name in loaders]]
        files = [source / "gdk-pixbuf" / name for name in meson_list(recipe, "gdkpixbuf_sources") + meson_list(recipe, "gdkpixdata_sources")]
        files += [source / "gdk-pixbuf/pixops/pixops.c", generated / "gdk-pixbuf-enum-types.c", generated / "gdk-pixbuf-marshal.c"]
        for names in loader_sources(recipe, loaders).values():
            files += [source / "gdk-pixbuf" / name for name in names]
        jobs = [(path, generated / f"pixbuf_{index:03d}.o") for index, path in enumerate(files)]
        def compile_one(job):
            path, obj = job
            # Original PNG code supplies an immutable key to libpng's legacy
            # mutable char typedef. Preserve the release source and warning.
            source_options = ["-Wno-error=incompatible-pointer-types-discards-qualifiers"] if path.name == "io-png.c" else []
            run([*flags, *options, *source_options, "-c", path, "-o", obj])
            if struct.unpack_from("<4I", obj.read_bytes())[::3] != (0xfeedfacf, 1) or struct.unpack_from("<I", obj.read_bytes(), 4)[0] != (0x100000c if args.arch == "arm64" else 0x1000007):
                raise ValueError("non-native image source object")
            return {"source": "generated/" + path.name if path.is_relative_to(generated) else str(path.relative_to(root)),
                    "source_sha256": digest(path), "compiler_options": source_options, "member": obj.name, "object_sha256": digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool:
            records = list(pool.map(compile_one, jobs))
        archive = install / "usr/lib/libgdk_pixbuf-2.0.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
        public, undefined, weak = symbols(archive)
        provided = definitions(archive)
        license_dir = install / "usr/share/licenses/gdk-pixbuf"
        license_dir.mkdir(parents=True)
        shutil.copyfile(source / "COPYING", license_dir / "COPYING")
        defined, imports = set(), set()
        for path in (install / "usr/lib").glob("*.a"):
            defined |= definitions(path)
            imports |= symbols(path)[1]
        for path in (install / "usr/libexec").glob("*.o"):
            imports |= symbols(path)[1]
        requests = sorted(imports - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        with tarfile.open(args.output, "w") as bundle:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    info = bundle.gettarinfo(path, str(path.relative_to(install)))
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ""
                    with path.open("rb") as stream:
                        bundle.addfile(info, stream)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION,
            "libraries": {"libgdk_pixbuf-2.0": {"sources": records, "exports": sorted(public), "imports": sorted(undefined - provided), "weak_imports": sorted(weak)}},
            "builtin_loaders": list(loaders), "configuration": values, "sdk_features": features,
            "generated_sources": {path.name: digest(path) for path in generated.glob("*.c") if path.name != "feature.c"},
            "disabled_optional_backends": (["TIFF (native dependency not built)"] if args.jpeg else ["JPEG/TIFF (native dependencies not built)"]) + ["Glycin", "Android", "Windows"],
            "system_requests": requests, "bundle_sha256": digest(args.output), "runtime_checked": False}, indent=2) + "\n")
        print(f"Native GdkPixbuf {args.arch}: {len(records)} original/generated Mach-O objects, {len(public)} exports, {len(loaders)} real builtin loaders")


if __name__ == "__main__":
    main()
