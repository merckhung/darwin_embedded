#!/usr/bin/env python3
"""Build complete selected Cairo/Xext/Xrender sources as native Mach-O."""
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

X11_RELEASES = {"libXext": "1.3.7", "libXrender": "0.9.12"}
SELECTED = ("cairo-png", "cairo-ft", "cairo-xlib", "cairo-xlib-xrender", "cairo-xcb",
            "cairo-xlib-xcb", "cairo-xcb-shm", "cairo-script", "cairo-ps", "cairo-pdf", "cairo-svg", "cairo-tee")


def list_inventory(recipe, name):
    match = re.search(r"^" + re.escape(name) + r"\s*=\s*\[(.*?)^\]", recipe, re.M | re.S)
    if not match: raise ValueError("missing original Cairo inventory: " + name)
    return re.findall(r"'([^']+)'", match[1])


def feature_inventory(recipe, name):
    match = re.search(r"^" + re.escape(name) + r"\s*=\s*\{(.*?)^\}", recipe, re.M | re.S)
    if not match: raise ValueError("missing original Cairo feature inventory: " + name)
    return {key: re.findall(r"'([^']+)'", contents)
            for key, contents in re.findall(r"'([^']+)':\s*\[(.*?)\]", match[1], re.S)}


def merge_archive(path, destination):
    """Overlapping dependency inputs must carry identical original artifacts."""
    with tarfile.open(path) as archive:
        for member in archive.getmembers():
            if not member.isfile(): continue
            relative = Path(member.name)
            if relative.is_absolute() or ".." in relative.parts: raise ValueError(member.name)
            target = destination / relative
            data = archive.extractfile(member).read()
            if target.exists() and target.read_bytes() != data:
                raise ValueError("conflicting native dependency input: " + member.name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "harfbuzz", "fontconfig", "x11", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/fonts/cairo/1.18.6"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-cairo-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        # The native SDK carries Apple's public header symlink layout.
        with tarfile.open(args.sdk) as archive: archive.extractall(sdk, filter="data")
        for bundle in (args.harfbuzz, args.fontconfig, args.x11): merge_archive(bundle, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("Cairo SDK architecture mismatch")
        # These genuine Libc/support objects belong inside LibSystem, whose
        # Bazel target receives them separately, rather than application dylibs.
        for archive in (install / "usr/lib").glob("libnative_*.a"): archive.unlink()
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-fno-common",
                 "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_GNU_SOURCE=1", "-D_FORTIFY_SOURCE=0",
                 "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-Wno-deprecated-declarations", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", generated, "-I", install / "usr/include",
                 "-I", install / "usr/include/freetype2", "-I", install / "usr/include/glib-2.0",
                 "-I", install / "usr/include/pixman-1"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        features, records = {}, {}

        def probe(name, text, required=False, extra=()):
            path = generated / "feature.c"
            path.write_text(text)
            result = subprocess.run([*map(str, [*flags, *extra]), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode: raise ValueError(name + ":\n" + result.stderr.decode())
            return result.returncode == 0

        adaptations = {}
        original_shm = source / "src/cairo-xcb-connection-shm.c"
        shm_text = original_shm.read_text()
        before = "    xcb_shm_attach (connection->xcb_connection, segment, id, readonly);\n    return segment;"
        after = '''    xcb_shm_attach (connection->xcb_connection, segment, id, readonly);
#if !IPC_RMID_DEFERRED_RELEASE
    /* XNU rejects attaching a segment after IPC_RMID. Wait for the server
     * to process this request before the pool marks the segment removed. */
    xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply (
        connection->xcb_connection, xcb_get_input_focus (connection->xcb_connection), NULL);
    free (reply);
#endif
    return segment;'''
        if shm_text.count(before) != 1:
            raise ValueError("original Cairo XCB shared-memory attach changed")
        compiled_shm = generated / "cairo-xcb-connection-shm.c"
        compiled_shm.write_text(shm_text.replace(before, after)); adaptations[original_shm] = compiled_shm
        def compile_library(name, files, options=()):
            jobs = [(path, generated / (name + f"_{index:03d}.o")) for index, path in enumerate(files)]
            def compile_one(job):
                path, obj = job
                try: run([*flags, *options, "-c", adaptations.get(path,path), "-o", obj])
                except RuntimeError as error: return {"error": str(error)}
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign Cairo/X11 object: " + str(path))
                return {"source": str(path.relative_to(root)), "source_sha256": digest(path),
                        "compiled_source_sha256": digest(adaptations.get(path,path)),
                        "adaptation": "Wait for real X server processing before IPC_RMID on non-deferred-release kernels" if path in adaptations else None,
                        "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool: entries = list(pool.map(compile_one, jobs))
            errors = [entry["error"] for entry in entries if "error" in entry]
            if errors: raise ValueError(name + " compilation failed:\n" + "\n".join(errors))
            output = install / "usr/lib" / (name + ".a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, obj in jobs]])
            public, imports, weak = symbols(output)
            records[name] = {"sources": entries, "exports": sorted(public), "imports": sorted(imports - definitions(output)),
                             "weak_imports": sorted(weak)}

        probe("Darwin_LP64", "#include <stdint.h>\n#include <stddef.h>\n#include <math.h>\n"
              "_Static_assert(__APPLE__ && __MACH__ && sizeof(void*)==8 && sizeof(long)==8 && sizeof(int)==4, \"Darwin LP64\");\n"
              "_Static_assert(sizeof(size_t)==8 && sizeof(long long)==8 && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__, \"native layout\");\n"
              "_Static_assert(M_PI > 3.14159 && M_PI < 3.14160, \"OpenLibm BSD constants\");\n", True)
        for name, release in X11_RELEASES.items():
            upstream = root / "src/Applications/x11" / name / release
            config_dir = generated / name
            config_dir.mkdir()
            config = {}
            for header in ("dlfcn.h", "inttypes.h", "stdint.h", "stdio.h", "stdlib.h", "strings.h", "string.h", "sys/stat.h", "sys/types.h", "unistd.h", "wchar.h"):
                macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
                if probe(name + ":" + macro, "#include <" + header + ">\n"): config[macro] = 1
            if probe(name + ":HAVE_REALLOCARRAY", "#include <stdlib.h>\nvoid *f=(void*)&reallocarray;\n"): config["HAVE_REALLOCARRAY"] = 1
            if probe(name + ":HAVE___BUILTIN_POPCOUNTL", "int f(unsigned long x){return __builtin_popcountl(x);}\n"):
                config["HAVE___BUILTIN_POPCOUNTL"] = 1
            config.update(STDC_HEADERS=1, PACKAGE_VERSION='"' + release + '"')
            (config_dir / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(config.items())))
            for header in (upstream / "include").rglob("*.h"):
                target = install / "usr/include" / header.relative_to(upstream / "include")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(header.read_bytes())
            recipe = (upstream / "src/Makefile.am").read_text().replace("\\\n", " ")
            match = re.search(r"^" + name + r"_la_SOURCES\s*=\s*(.*)$", recipe, re.M)
            if not match: raise ValueError("missing original X11 extension inventory: " + name)
            files = [upstream / "src" / path for path in match[1].split() if path.endswith(".c")]
            if name == "libXext" and not config.get("HAVE_REALLOCARRAY"):
                if "AC_REPLACE_FUNCS([reallocarray])" not in (upstream / "configure.ac").read_text():
                    raise ValueError("Xext replacement source selection changed")
                files.append(upstream / "src/reallocarray.c")
            # Use this package's config before the Cairo-generated config.
            compile_library(name, files, ["-I", config_dir, "-I", upstream / "include", "-I", upstream / "include/X11/extensions"])
            records[name]["configuration"] = config
            licenses = install / "usr/share/licenses" / name
            licenses.mkdir(parents=True)
            shutil.copyfile(upstream / "COPYING", licenses / "COPYING")
        recipe = (source / "meson.build").read_text()
        configuration = {}
        # This inventory also contains nested test names; select its header
        # entries explicitly, retaining the upstream ordering and spelling.
        headers = re.findall(r"\['([^']+)'", re.search(r"^check_headers = \[(.*?)^\]", recipe, re.M | re.S)[1])
        for header in dict.fromkeys(headers):
            macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
            if probe(macro, "#include <" + header + ">\n"): configuration[macro] = 1
        for header in ("X11/extensions/XShm.h", "X11/extensions/shmproto.h", "X11/extensions/shmstr.h"):
            macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
            if probe(macro, "#include <X11/Xlibint.h>\n#include <X11/Xproto.h>\n#include <" + header + ">\n"):
                configuration[macro] = 1
        prefix = "".join("#include <" + header + ">\n" for header in headers if configuration.get("HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())))
        prefix += "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n#include <time.h>\n#include <math.h>\n#include <locale.h>\n#include <xlocale.h>\n#include <dlfcn.h>\n#include <sys/mman.h>\n#include <sys/shm.h>\n#include <fontconfig/fontconfig.h>\n#include <X11/Xlib.h>\n#include <X11/extensions/Xrender.h>\n"
        functions = list_inventory(recipe, "check_funcs") + ["mmap", "clock_gettime", "feclearexcept", "feenableexcept", "fedisableexcept",
            "sched_getaffinity", "strndup", "dlsym", "FcInit", "FcFini", "XRenderCreateSolidFill", "XRenderCreateLinearGradient",
            "XRenderCreateRadialGradient", "XRenderCreateConicalGradient", "shmget", "shmat", "shmdt", "shmctl"]
        for name in functions:
            macro = "HAVE_" + name.upper()
            if probe(macro, prefix + "void *f=(void*)&" + name + ";\n"): configuration[macro] = 1
        for name in ("uint64_t", "uint128_t", "__uint128_t"):
            if probe("HAVE_" + name.upper(), "#include <stdint.h>\n" + name + " native_type;\n"):
                configuration["HAVE_" + name.upper()] = 1
        for name in ("pthread", "atomic-ops-c11"):
            probe(name, (source / "meson-cc-tests" / (name + ".c")).read_text(), True)
        if probe("mkdir-variant-1", (source / "meson-cc-tests/mkdir-variant-1.c").read_text(), extra=["-DHAVE_SYS_STAT_H"]):
            mkdir_variant = 1
        else:
            probe("mkdir-variant-2", (source / "meson-cc-tests/mkdir-variant-2.c").read_text(), True, ["-DHAVE_SYS_STAT_H"])
            mkdir_variant = 2
        ft_prefix = "#include <ft2build.h>\n#include FT_FREETYPE_H\n#include FT_COLOR_H\n#include FT_OTSVG_H\n"
        for macro, code in (("HAVE_FT_SVG_DOCUMENT", "FT_SVG_Document document;"), ("HAVE_FT_LOAD_NO_SVG", "int value=FT_LOAD_NO_SVG;"),
                            ("HAVE_FT_COLR_V1", "void *f=(void*)&FT_Get_Color_Glyph_Paint;")):
            if probe(macro, ft_prefix + code + "\n"): configuration[macro] = 1
        feature_values = {"CAIRO_FEATURES_H": 1, **{"CAIRO_HAS_" + name: 1 for name in (
            "PNG_FUNCTIONS", "SVG_SURFACE", "FC_FONT", "FT_FONT", "XLIB_SURFACE", "XLIB_XRENDER_SURFACE", "XCB_SURFACE",
            "XLIB_XCB_FUNCTIONS", "XCB_SHM_FUNCTIONS", "GOBJECT_FUNCTIONS", "SCRIPT_SURFACE", "PS_SURFACE", "PDF_SURFACE",
            "IMAGE_SURFACE", "USER_FONT", "MIME_SURFACE", "RECORDING_SURFACE", "OBSERVER_SURFACE", "TEE_SURFACE")}}
        configuration.update(SIZEOF_VOID_P=8, SIZEOF_INT=4, SIZEOF_LONG=8, SIZEOF_LONG_LONG=8, SIZEOF_SIZE_T=8,
            CAIRO_HAS_PTHREAD=1, CAIRO_HAS_REAL_PTHREAD=1, HAVE_C11_ATOMIC_PRIMITIVES=1, HAVE_MKDIR=mkdir_variant,
            HAVE_ZLIB=1, CAIRO_HAS_INTERPRETER=1, CAIRO_HAS_DLSYM=1, HAS_PIXMAN_GLYPHS=1, HAS_PIXMAN_r8g8b8_sRGB=1,
            IPC_RMID_DEFERRED_RELEASE=0, WARN_UNUSED_RESULT="__attribute__((__warn_unused_result__))")
        if args.arch == "arm64": configuration["ATOMIC_OP_NEEDS_MEMORY_BARRIER"] = 1
        (generated / "config.h").write_text("/* Native Darwin SDK-probed Cairo configuration. */\n" +
            "".join(f"#define {key} {value}\n" for key, value in sorted(configuration.items())))
        includes = install / "usr/include/cairo"
        includes.mkdir()
        (includes / "cairo-features.h").write_text("/* Selected genuine native backends. */\n" +
            "".join(f"#define {key} {value}\n" for key, value in sorted(feature_values.items())))
        shutil.copyfile(includes / "cairo-features.h", generated / "cairo-features.h")
        source_recipe = (source / "src/meson.build").read_text()
        files = list_inventory(source_recipe, "cairo_sources")
        additional = feature_inventory(source_recipe, "cairo_feature_sources")
        headers = list_inventory(source_recipe, "cairo_headers")
        additional_headers = feature_inventory(source_recipe, "cairo_feature_headers")
        for feature in SELECTED:
            files += additional.get(feature, []); headers += additional_headers.get(feature, [])
        for header in headers: shutil.copyfile(source / "src" / header, includes / header)
        options = ["-DCAIRO_COMPILATION", "-I", includes, "-I", source / "src", "-I", source]
        builtins = root / "src/Toolchains/compiler-rt/20.1.8/lib/builtins"
        compile_library("libcairo", [*[source / "src" / path for path in files],
                        builtins / "udivti3.c", builtins / "udivmodti4.c"], [*options, "-fvisibility=hidden"])
        for name, subdir, variable in (("libcairo-gobject", "cairo-gobject", "cairo_gobject"),
                                      ("libcairo-script-interpreter", "cairo-script", "cairoscript_interpreter")):
            directory = source / "util" / subdir
            text = (directory / "meson.build").read_text()
            files = list_inventory(text, variable + "_sources")
            headers = list_inventory(text, variable + "_headers")
            for header in headers: shutil.copyfile(directory / header, includes / header)
            compile_library(name, [directory / path for path in files], [*options, "-I", directory] +
                            (["-fvisibility=hidden"] if name == "libcairo-gobject" else []))
        license_dir = install / "usr/share/licenses/cairo"
        license_dir.mkdir(parents=True)
        for path in source.glob("COPYING*"): shutil.copyfile(path, license_dir / path.name)
        defined, undefined = set(), set()
        for archive in (install / "usr/lib").glob("*.a"):
            defined |= definitions(archive); undefined |= symbols(archive)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        with tarfile.open(args.output, "w") as archive:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    info = archive.gettarinfo(path, str(path.relative_to(install)))
                    info.uid = info.gid = info.mtime = 0; info.uname = info.gname = ""
                    with path.open("rb") as stream: archive.addfile(info, stream)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": "1.18.6", "libraries": records,
            "configuration": configuration, "feature_configuration": feature_values, "sdk_features": features,
            "selected_features": SELECTED, "system_requests": requests, "bundle_sha256": digest(args.output), "runtime_checked": False,
            "disabled_optional_services": ["Quartz/CoreGraphics/CoreText frameworks", "LZO compression", "BFD symbol lookup",
                                           "preload-based cairo-trace tool", "external PDF/SVG test renderers"]}, indent=2) + "\n")
        print(f"Native Cairo/Xext/Xrender {args.arch}: {sum(len(record['sources']) for record in records.values())} genuine Mach-O source objects")


if __name__ == "__main__":
    main()
