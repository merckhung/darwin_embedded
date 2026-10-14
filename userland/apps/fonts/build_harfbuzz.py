#!/usr/bin/env python3
"""Compile upstream HarfBuzz inventories as native Mach-O through Bazel."""
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


LIBRARIES = {
    "libharfbuzz": ("hb_base_sources", "hb_ft_sources", "hb_glib_sources"),
    "libharfbuzz-subset": ("hb_subset_sources",),
    "libharfbuzz-gobject": ("hb_gobject_sources",),
    "libharfbuzz-raster": ("hb_raster_sources",),
    "libharfbuzz-vector": ("hb_vector_sources",),
    "libharfbuzz-gpu": ("hb_gpu_sources",),
}
HEADERS = {
    "libharfbuzz": ("hb_base_headers", "hb_ft_headers", "hb_glib_headers"),
    "libharfbuzz-subset": ("hb_subset_headers",),
    "libharfbuzz-gobject": ("hb_gobject_headers",),
    "libharfbuzz-raster": ("hb_raster_headers",),
    "libharfbuzz-vector": ("hb_vector_headers",),
    "libharfbuzz-gpu": ("hb_gpu_headers",),
}


def inventory(recipe, name):
    match = re.search(r"^" + name + r"\s*=\s*(?:files\(|\[)(.*?)(?:\)|\])", recipe, re.M | re.S)
    if not match:
        raise ValueError("missing upstream HarfBuzz inventory: " + name)
    return re.findall(r"'([^']+)'", match[1])


def configure_libcxx(source, generated):
    """Configure the pinned header template; no CMake or C++ runtime build."""
    values = {
        "_LIBCPP_ABI_VERSION": 1, "_LIBCPP_ABI_NAMESPACE": "__1",
        "_LIBCPP_ABI_FORCE_ITANIUM": 1, "_LIBCPP_ABI_FORCE_MICROSOFT": 0,
        "_LIBCPP_HAS_THREADS": 1, "_LIBCPP_HAS_MONOTONIC_CLOCK": 1,
        "_LIBCPP_HAS_TERMINAL": 1, "_LIBCPP_HAS_MUSL_LIBC": 0,
        "_LIBCPP_HAS_THREAD_API_PTHREAD": 1, "_LIBCPP_HAS_THREAD_API_EXTERNAL": 0,
        "_LIBCPP_HAS_THREAD_API_WIN32": 0, "_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS": 0,
        "_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION": 1,
        "_LIBCPP_HAS_FILESYSTEM": 1, "_LIBCPP_HAS_RANDOM_DEVICE": 1,
        "_LIBCPP_HAS_LOCALIZATION": 1, "_LIBCPP_HAS_UNICODE": 1,
        "_LIBCPP_HAS_WIDE_CHARACTERS": 1, "_LIBCPP_HAS_NO_STD_MODULES": 1,
        "_LIBCPP_HAS_TIME_ZONE_DATABASE": 0, "_LIBCPP_INSTRUMENTED_WITH_ASAN": 0,
        "_LIBCPP_PSTL_BACKEND_SERIAL": 1,
        "_LIBCPP_HARDENING_MODE_DEFAULT": "_LIBCPP_HARDENING_MODE_NONE",
        "_LIBCPP_ABI_DEFINES": "", "_LIBCPP_EXTRA_SITE_DEFINES": "",
    }
    text = (source / "include/__config_site.in").read_text()
    text = re.sub(r"^#cmakedefine01 (\w+)$", lambda m: f"#define {m[1]} {values[m[1]]}", text, flags=re.M)
    text = re.sub(r"^#cmakedefine (\w+)([^\n]*)$",
                  lambda m: "#define " + m[1] + (m[2] or " 1") if values.get(m[1]) else "/* #undef " + m[1] + " */",
                  text, flags=re.M)
    text = re.sub(r"@(\w+)@", lambda m: str(values[m[1]]), text)
    if "#cmakedefine" in text or re.search(r"@\w+@", text):
        raise ValueError("incomplete libc++ header configuration")
    (generated / "__config_site").write_text(text)
    shutil.copyfile(source / "vendor/llvm/default_assertion_handler.in", generated / "__assertion_handler")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "glib-core", "graphics", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/fonts/harfbuzz/14.6.0"
    libcxx = root / "src/Toolchains/libcxx/20.1.8"
    recipe = (source / "src/meson.build").read_text()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-harfbuzz-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        for path, destination in ((args.sdk, sdk), (args.glib_core, install), (args.graphics, install)):
            with tarfile.open(path) as archive:
                archive.extractall(destination, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("HarfBuzz SDK architecture mismatch")
        libcxx_configuration = configure_libcxx(libcxx, generated)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=c++11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-exceptions", "-fno-rtti",
                 "-fno-threadsafe-statics", "-fvisibility-inlines-hidden", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Wno-availability",
                 "-I", generated, "-isystem", libcxx / "include", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", source / "src", "-I", install / "usr/include",
                 "-I", install / "usr/include/freetype2", "-I", install / "usr/include/glib-2.0"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        configuration, features = {}, {}

        def probe(name, text, required=False):
            path = generated / "feature.cc"
            path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode:
                raise ValueError(name + ":\n" + result.stderr.decode())
            return result.returncode == 0

        probe("Darwin_LP64_libcxx", "#include <memory>\n#include <algorithm>\n#include <cmath>\n#include <pthread.h>\n"
              "static_assert(__APPLE__ && __MACH__ && sizeof(void*)==8 && sizeof(long)==8, \"Darwin LP64\");\n"
              "static_assert(_LIBCPP_VERSION==200100, \"pinned libc++ release family\");\n"
              "static_assert(M_PI > 3.14159 && M_PI < 3.14160, \"OpenLibm BSD constants\");\n"
              "static_assert(__atomic_always_lock_free(8,0), \"native atomics\");\n", True)
        root_recipe = (source / "meson.build").read_text()
        headers = re.search(r"^check_headers = \[(.*?)^\]", root_recipe, re.M | re.S)
        functions = re.search(r"^check_funcs = \[(.*?)^\]", root_recipe, re.M | re.S)
        if not headers or not functions:
            raise ValueError("missing HarfBuzz feature inventories")
        for header in re.findall(r"\['([^']+)'", headers[1]):
            macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
            if probe(macro, "#include <" + header + ">\n"):
                configuration[macro] = 1
        prefix = "#include <cstdlib>\n#include <unistd.h>\n#include <sys/mman.h>\n#include <locale.h>\n#include <xlocale.h>\n#include <cmath>\n"
        for function in re.findall(r"\['([^']+)'", functions[1]):
            macro = "HAVE_" + function.upper()
            if probe(macro, prefix + "auto native_feature=&" + function + ";\n"):
                configuration[macro] = 1
        for function in ("FT_Get_Var_Blend_Coordinates", "FT_Set_Var_Blend_Coordinates", "FT_Done_MM_Var", "FT_Get_Transform"):
            macro = "HAVE_" + function.upper()
            if probe(macro, "#include <ft2build.h>\n#include FT_FREETYPE_H\n#include FT_MULTIPLE_MASTERS_H\nauto f=&" + function + ";\n"):
                configuration[macro] = 1
        probe("native_pthread", "#include <pthread.h>\nauto f=&pthread_mutex_lock;\n", True)
        configuration.update(HAVE_PTHREAD=1, HAVE_GLIB=1, HAVE_GOBJECT=1, HAVE_FREETYPE=1,
                             HAVE_SUBSET=1, HAVE_RASTER=1, HAVE_VECTOR=1, HAVE_GPU=1, HAVE_PNG=1, HAVE_ZLIB=1)
        (generated / "config.h").write_text("/* Native Darwin SDK-probed configuration. */\n" +
            "".join(f"#define {key} {value}\n" for key, value in sorted(configuration.items())))
        includes = install / "usr/include/harfbuzz"
        includes.mkdir()
        header_groups = {name: [source / "src" / header for variable in variables
                              for header in inventory(recipe, variable)] for name, variables in HEADERS.items()}
        for headers in header_groups.values():
            for path in headers:
                shutil.copyfile(path, includes / path.name)
        # This upstream generator may update its baseline; use a private copy.
        for name in ("hb-version.h.in", "hb-version.h"):
            shutil.copyfile(source / "src" / name, generated / name)
        run(["python3", source / "src/gen-hb-version.py", "14.6.0", includes / "hb-version.h", generated / "hb-version.h.in"])
        header_groups["libharfbuzz"].append(includes / "hb-version.h")
        text = (source / "src/hb-features.h.in").read_text()
        text = re.sub(r"^#mesondefine (HB_HAS_(\w+))$", lambda m: "#define " + m[1] + " " +
                      str(configuration.get("HAVE_" + m[2], 0)), text, flags=re.M)
        (includes / "hb-features.h").write_text(text)
        shutil.copyfile(includes / "hb-features.h", generated / "hb-features.h")
        shutil.copyfile(includes / "hb-version.h", generated / "hb-version.h")
        generator = generated / "glib-mkenums"
        generator.write_text((root / "src/Applications/glib/glib/2.90.1/gobject/glib-mkenums.in").read_text()
                             .replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
        for suffix in ("cc", "h"):
            temporary = generated / ("hb-gobject-enums.tmp." + suffix)
            with temporary.open("w") as stream:
                subprocess.run(["python3", str(generator), "--identifier-prefix", "hb_", "--symbol-prefix", "hb_gobject",
                    "--template", str(source / "src" / ("hb-gobject-enums." + suffix + ".tmpl")),
                    *map(str, header_groups["libharfbuzz"])], stdout=stream, check=True)
            run(["python3", source / "src/fix_get_types.py", temporary, generated / ("hb-gobject-enums." + suffix)])
        shutil.copyfile(generated / "hb-gobject-enums.h", includes / "hb-gobject-enums.h")
        header_groups["libharfbuzz-gobject"].append(includes / "hb-gobject-enums.h")
        shader_list = re.search(r"^hb_gpu_shader_sources = \[(.*?)^\]", recipe, re.M | re.S)
        if not shader_list:
            raise ValueError("missing upstream GPU shader inventory")
        for shader, header in re.findall(r"\[\s*'([^']+)',\s*'([^']+)'\s*\]", shader_list[1]):
            run(["python3", source / "src/gen-gpu-shader-artifacts.py", generated / header, generated, source / "src" / shader])
        api_dir = install / "usr/share/harfbuzz/api"
        api_dir.mkdir(parents=True)
        for name, headers in header_groups.items():
            run(["python3", source / "src/gen-def.py", api_dir / (name + ".def"), *headers])
        records = {}
        for name, variables in LIBRARIES.items():
            files = [source / "src" / path for variable in variables for path in inventory(recipe, variable) if path.endswith(".cc")]
            if name == "libharfbuzz-gobject":
                files.append(generated / "hb-gobject-enums.cc")
            jobs = [(path, generated / (name + f"_{index:03d}.o")) for index, path in enumerate(files)]
            def compile_one(job):
                path, obj = job
                try:
                    run([*flags, "-I", includes, "-c", path, "-o", obj])
                except RuntimeError as error:
                    return {"error": str(error)}
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("non-native HarfBuzz object")
                return {"source": "generated/" + str(path.relative_to(generated)) if path.is_relative_to(generated) else str(path.relative_to(root)),
                        "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                entries = list(pool.map(compile_one, jobs))
            errors = [entry["error"] for entry in entries if "error" in entry]
            if errors:
                raise ValueError(name + " compilation failed:\n" + "\n".join(errors))
            output = install / "usr/lib" / (name + ".a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, obj in jobs]])
            public, imports, weak = symbols(output)
            declared = set(re.findall(r"^hb_\w+$", (api_dir / (name + ".def")).read_text(), re.M))
            if not declared or not {"_" + entry for entry in declared} <= public:
                raise ValueError("HarfBuzz public header API missing: " + name + ": " +
                                 ", ".join(sorted({"_" + entry for entry in declared} - public)))
            records[name] = {"sources": entries, "exports": sorted(public), "imports": sorted(imports - definitions(output)),
                             "weak_imports": sorted(weak), "header_api": sorted(declared)}
        licenses = install / "usr/share/licenses/harfbuzz"
        licenses.mkdir(parents=True)
        shutil.copyfile(source / "COPYING", licenses / "COPYING")
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
        args.report.write_text(json.dumps({"architecture": args.arch, "version": "14.6.0", "libraries": records,
            "configuration": configuration, "sdk_features": features, "libcxx_header_configuration": libcxx_configuration,
            "generated_headers": {path.name: digest(path) for path in generated.glob("*.h*")},
            "generated_sources": {path.name: digest(path) for path in generated.glob("*.cc") if path.name != "feature.cc"},
            "libcxx_config_site_sha256": digest(generated / "__config_site"),
            "openlibm_header_selection": "__BSD_VISIBLE=1 selects original BSD declarations/constants alongside _DARWIN_C_SOURCE",
            "bundle_sha256": digest(args.output), "system_requests": requests, "runtime_checked": False}, indent=2) + "\n")
        print(f"Native HarfBuzz {args.arch}: {sum(len(record['sources']) for record in records.values())} genuine Mach-O source objects")


if __name__ == "__main__":
    main()
