#!/usr/bin/env python3
"""Build unchanged graphics prerequisites as native Darwin objects in Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols

VERSIONS = {"zlib": "1.3.2", "expat": "2.9.0", "libpng": "1.6.59",
            "freetype": "2.15.0", "pixman": "0.46.4"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def all_definitions(path):
    """Keep genuine private extern definitions inside their source archive."""
    result = set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        if "external " in line and "(undefined)" not in line:
            match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
            if not match:
                raise ValueError("unrecognized graphics symbol: " + line)
            result.add(match.group(1))
    return result


def pack(tree, destination):
    with tarfile.open(destination, "w") as archive:
        for path in sorted(tree.rglob("*")):
            if path.is_file():
                info = archive.gettarinfo(path, str(path.relative_to(tree)))
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                with path.open("rb") as stream:
                    archive.addfile(info, stream)


def make_inventory(path, variable):
    text = path.read_text().replace("\\\n", " ")
    match = re.search(r"^" + re.escape(variable) + r"\s*=\s*(.*)$", text, re.M)
    if not match:
        raise ValueError("missing upstream source inventory: " + variable)
    return match[1].split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "headers", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    upstream = {name: root / "src/Applications/graphics" / name / version
                for name, version in VERSIONS.items()}
    # Refuse modified upstream bytes, including headers included indirectly.
    for name, directory in upstream.items():
        manifest = json.loads((directory / "UPSTREAM.json").read_text())
        renames = {dest: original for original, dest in manifest.get("packaging_renames", {}).items()}
        actual = {renames.get(str(path.relative_to(directory)), str(path.relative_to(directory))): digest(path)
                  for path in directory.rglob("*") if path.is_file() and path.name != "UPSTREAM.json"}
        if actual != manifest["upstream_files_sha256"]:
            raise ValueError("upstream source inventory changed: " + name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-graphics-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("graphics SDK architecture mismatch")
        tree = work / "install"
        includes, libraries = tree / "usr/include", tree / "usr/lib"
        includes.mkdir(parents=True)
        libraries.mkdir()
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                 "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-Werror=implicit-function-declaration",
                 "-Wno-availability", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", includes, "-I", includes / "freetype2"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        features, records, outputs = {}, {}, []

        def require(name, source):
            path = work / "feature.c"
            path.write_text(source)
            run([*flags, "-Werror", "-c", path, "-o", work / "feature.o"])
            features[name] = True

        require("native_types", "#include <stdint.h>\n#include <sys/types.h>\n"
                "_Static_assert(sizeof(void*)==8 && sizeof(long)==8 && sizeof(off_t)==8, \"Darwin LP64\");\n"
                "_Static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__, \"little endian\");\n")
        require("native_random", "#include <sys/random.h>\n#include <stdlib.h>\n"
                "void *a=(void*)&getentropy,*b=(void*)&arc4random_buf;\n")
        require("native_posix_memalign", "#include <stdlib.h>\nvoid *a=(void*)&posix_memalign;\n")
        require("native_tls", "__thread int native_tls; int f(void) { return native_tls; }\n")

        def build(name, source, files, definitions=None, extra=None):
            build_dir = work / name
            build_dir.mkdir()
            configuration = definitions or {}
            local_flags = [*flags, "-I", build_dir, "-I", source]
            for key, value in configuration.items():
                local_flags.append(f"-D{key}={value}")
            local_flags += extra or []
            jobs = [(source / filename, build_dir / (f"{index:03d}_" + Path(filename).stem + ".o"))
                    for index, filename in enumerate(files)]

            def compile_one(job):
                path, obj = job
                run([*local_flags, "-c", path, "-o", obj])
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign graphics object: " + str(path))
                return {"source": str(path.relative_to(root)), "source_sha256": digest(path),
                        "member": obj.name, "object_sha256": digest(obj)}

            with ThreadPoolExecutor(max_workers=8) as pool:
                sources = list(pool.map(compile_one, jobs))
            output = libraries / (name + ".a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, obj in jobs]])
            if run(["llvm-ar-20", "t", output]).splitlines() != [obj.name for _, obj in jobs]:
                raise ValueError("graphics archive member inventory mismatch")
            defined, undefined, weak = symbols(output)
            records[name] = {"configuration": configuration, "extra_flags": list(map(str, extra or [])),
                             "sources": sources, "exports": sorted(defined), "imports": sorted(undefined - all_definitions(output)),
                             "weak_imports": sorted(weak), "archive_sha256": digest(output)}
            outputs.append(output)

        zlib = upstream["zlib"]
        for name in ("zlib.h", "zconf.h"):
            shutil.copyfile(zlib / name, includes / name)
        files = [Path(name).with_suffix(".c").name for variable in ("OBJZ", "OBJG")
                 for name in make_inventory(zlib / "Makefile.in", variable)]
        build("libz", zlib, files, {"HAVE_UNISTD_H": 1, "HAVE_STDARG_H": 1, "HAVE_HIDDEN": 1})

        expat = upstream["expat"]
        values = {"BYTEORDER": 1234, "XML_CONTEXT_BYTES": 1024, "XML_DTD": 1, "XML_NS": 1,
                  "XML_GE": 1, "STDC_HEADERS": 1, "HAVE_GETENTROPY": 1, "HAVE_ARC4RANDOM_BUF": 1}
        for header in ("dlfcn.h", "fcntl.h", "inttypes.h", "memory.h", "stdint.h", "stdlib.h",
                       "strings.h", "string.h", "sys/stat.h", "sys/types.h", "unistd.h"):
            require("expat_" + header, "#include <" + header + ">\n")
            values["HAVE_" + header.upper().replace("/", "_").replace(".", "_")] = 1
        expat_config = work / "expat_config.h"
        expat_config.write_text("/* Verified native Darwin features. */\n" +
                                "".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
        files = [name for name in make_inventory(expat / "lib/Makefile.am", "libexpat_la_SOURCES")
                 if name.endswith(".c")]
        files += ["random_arc4random_buf.c", "random_getentropy.c"]
        for name in ("expat.h", "expat_external.h"):
            shutil.copyfile(expat / "lib" / name, includes / name)
        build("libexpat", expat / "lib", files, extra=["-I", work, "-DHAVE_EXPAT_CONFIG_H=1"])
        records["libexpat"]["generated_configuration"] = values

        png = upstream["libpng"]
        for name in ("png.h", "pngconf.h"):
            shutil.copyfile(png / name, includes / name)
        # Upstream ships this exact pre-generated default for custom builders.
        shutil.copyfile(png / "scripts/pnglibconf.h.prebuilt", includes / "pnglibconf.h")
        files = [name for name in make_inventory(png / "Makefile.am", "libpng@PNGLIB_MAJOR@@PNGLIB_MINOR@_la_SOURCES")
                 if name.endswith(".c")]
        build("libpng16", png, files, {"PNG_ARM_NEON_OPT": 0, "PNG_INTEL_SSE_OPT": 0})

        freetype = upstream["freetype"]
        shutil.copytree(freetype / "include", includes / "freetype2")
        # Read the canonical single-object inventory, following INSTALL.ANY;
        # no CMake or other upstream build program is executed.
        match = re.search(r"set\(BASE_SRCS\s+(.*?)\n\)", (freetype / "CMakeLists.txt").read_text(), re.S)
        if not match:
            raise ValueError("missing FreeType single-object inventory")
        files = match[1].split() + ["src/base/ftsystem.c", "src/base/ftdebug.c"]
        build("libfreetype", freetype, files,
              {"FT2_BUILD_LIBRARY": 1, "FT_CONFIG_OPTION_SYSTEM_ZLIB": 1, "FT_CONFIG_OPTION_USE_PNG": 1})

        pixman = upstream["pixman"]
        config = {"SIZEOF_LONG": 8, "PACKAGE": '"pixman"', "TLS": "__thread", "HAVE_PTHREADS": 1,
                  "HAVE_POSIX_MEMALIGN": 1, "TOOLCHAIN_SUPPORTS_ATTRIBUTE_CONSTRUCTOR": 1,
                  "TOOLCHAIN_SUPPORTS_ATTRIBUTE_DESTRUCTOR": 1, "HAVE_GCC_VECTOR_EXTENSIONS": 1}
        (work / "pixman-config.h").write_text("/* Native Darwin, upstream generic C raster paths. */\n" +
                                            "".join(f"#define {key} {value}\n" for key, value in sorted(config.items())))
        version = (pixman / "pixman/pixman-version.h.in").read_text()
        for name, value in zip(("MAJOR", "MINOR", "MICRO"), VERSIONS["pixman"].split(".")):
            version = version.replace("@PIXMAN_VERSION_" + name + "@", value)
        (work / "pixman-version.h").write_text(version)
        dest = includes / "pixman-1"
        dest.mkdir()
        shutil.copyfile(pixman / "pixman/pixman.h", dest / "pixman.h")
        shutil.copyfile(work / "pixman-version.h", dest / "pixman-version.h")
        match = re.search(r"pixman_files = files\((.*?)\n\)", (pixman / "pixman/meson.build").read_text(), re.S)
        if not match:
            raise ValueError("missing Pixman generic source inventory")
        files = re.findall(r"'([^']+\.c)'", match[1])
        build("libpixman-1", pixman / "pixman", files,
              extra=["-I", work, "-DHAVE_CONFIG_H=1", "-fno-strict-aliasing", "-fvisibility=hidden", "-ftrapping-math"])
        records["libpixman-1"]["generated_configuration"] = config

        defined, undefined = set(), set()
        for output in outputs:
            exports, imports, _ = symbols(output)
            defined |= all_definitions(output)
            undefined |= imports
        # ld64 supplies the per-image destructor registration handle. The
        # final signed dylib audit proves its definition in each linked image.
        linker_symbols = undefined - defined & {"___dso_handle"}
        required = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(required) + "\n")
        pack(tree, args.output)
        header_tree = work / "headers"
        shutil.copytree(tree / "usr/include", header_tree / "usr/include")
        pack(header_tree, args.headers)
        args.report.write_text(json.dumps({"architecture": args.arch, "versions": VERSIONS,
            "components": records, "native_system_imports": required, "sdk_features": features,
            "linker_supplied_imports": sorted(linker_symbols),
            "sdk_sha256": digest(args.sdk), "source_manifests": {name: digest(path / "UPSTREAM.json")
                                                                    for name, path in upstream.items()},
            "bundle_sha256": digest(args.output), "headers_sha256": digest(args.headers),
            "runtime_checked": False,
            "validation": "Unchanged upstream sources compiled by Bazel actions; every member native Mach-O; separate strong native link and guest execution required."}, indent=2) + "\n")
        print(f"Native graphics {args.arch}: {sum(len(r['sources']) for r in records.values())} Mach-O objects, {len(required)} native runtime imports")


if __name__ == "__main__":
    main()
