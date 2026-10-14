#!/usr/bin/env python3
"""Build real Fontconfig with upstream Python/gperf generators through Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "glib-dependencies", "graphics", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/fonts/fontconfig/2.18.3"
    recipe = (source / "meson.build").read_text()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-fontconfig-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        (generated / "src").mkdir()
        (generated / "fc-case").mkdir()
        for path, destination in ((args.sdk, sdk), (args.glib_dependencies, install), (args.graphics, install)):
            with tarfile.open(path) as archive:
                archive.extractall(destination, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("Fontconfig SDK architecture mismatch")
        includes = install / "usr/include/fontconfig"
        includes.mkdir()
        for path in (source / "fontconfig").glob("*.h"):
            shutil.copyfile(path, includes / path.name)
        values = {"CACHE_VERSION": 12, "CACHE_SNAP_VERSION": 0, "NEXT_CACHE_VERSION": 0, "CACHE_MIN_COMPAT_VERSION": 9}
        text = re.sub(r"@(\w+)@", lambda match: str(values[match[1]]), (source / "fontconfig/fontconfig.h.in").read_text())
        (includes / "fontconfig.h").write_text(text)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration",
                 "-Werror=incompatible-pointer-types", "-Wno-availability", "-Wno-deprecated-declarations",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include", "-I", generated, "-I", generated / "src",
                 "-I", install / "usr/include", "-I", install / "usr/include/freetype2", "-I", source,
                 "-I", source / "src", "-I", source / "fc-lang"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        configuration, features = {}, {}

        def probe(name, code, required=False):
            path = generated / "feature.c"
            path.write_text(code)
            result = subprocess.run([*map(str, flags), "-Werror", "-Wno-deprecated-declarations", "-c", str(path),
                                     "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode:
                raise ValueError(name + ":\n" + result.stderr.decode())
            return result.returncode == 0

        probe("Darwin_LP64", "#include <stdint.h>\n#include <stddef.h>\n"
              "_Static_assert(__APPLE__ && __MACH__ && sizeof(void*)==8 && sizeof(double)==8 && _Alignof(double)==8, \"Darwin cache ABI\");\n", True)
        header_match = re.search(r"check_headers = \[(.*?)\n\]", recipe, re.S)
        function_match = re.search(r"check_funcs = \[(.*?)\n\]", recipe, re.S)
        if not header_match or not function_match:
            raise ValueError("missing Fontconfig feature inventories")
        available = []
        for header in re.findall(r"\['([^']+)'\]", header_match[1]):
            macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
            if probe(macro, "#include <" + header + ">\n"):
                available.append(header)
                configuration[macro] = 1
        prefix = "".join("#include <" + header + ">\n" for header in available)
        prefix += "#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n#include <sys/mman.h>\n#include <getopt.h>\n#include <locale.h>\n#include <libintl.h>\n"
        for name in dict.fromkeys(re.findall(r"\['([^']+)'", function_match[1])):
            macro = "HAVE_" + name.upper()
            if probe(macro, prefix + "void *native_feature=(void*)&" + name + ";\n"):
                configuration[macro] = 1
        for name in ("FT_Get_BDF_Property", "FT_Get_PS_Font_Info", "FT_Has_PS_Glyph_Names", "FT_Get_X11_Font_Format", "FT_Done_MM_Var"):
            macro = "HAVE_" + name.upper()
            if probe(macro, "#include <ft2build.h>\n#include FT_FREETYPE_H\n#include FT_BDF_H\n#include FT_TYPE1_TABLES_H\n#include FT_XFREE86_H\n#include FT_MULTIPLE_MASTERS_H\nvoid *f=(void*)&" + name + ";\n"):
                configuration[macro] = 1
        for name in ("stdatomic-primitives", "intel-atomic-primitives", "pthread-prio-inherit", "flexible-array-member"):
            if probe(name, (source / "meson-cc-tests" / (name + "-test.c")).read_text()):
                key = {"stdatomic-primitives": "HAVE_STDATOMIC_PRIMITIVES", "intel-atomic-primitives": "HAVE_INTEL_ATOMIC_PRIMITIVES",
                       "pthread-prio-inherit": "HAVE_PTHREAD_PRIO_INHERIT", "flexible-array-member": "FLEXIBLE_ARRAY_MEMBER"}[name]
                configuration[key] = 1 if name != "flexible-array-member" else "/**/"
        for name, code in (
            ("HAVE_STRUCT_STATFS_F_FLAGS", "#include <sys/mount.h>\nvoid f(struct statfs *s){s->f_flags=0;}"),
            ("HAVE_STRUCT_STATFS_F_FSTYPENAME", "#include <sys/mount.h>\nvoid f(struct statfs *s){s->f_fstypename[0]=0;}"),
            ("HAVE_STRUCT_DIRENT_D_TYPE", "#include <dirent.h>\nvoid f(struct dirent *s){s->d_type=0;}"),
        ):
            if probe(name, code): configuration[name] = 1
        configuration.update(FC_VERSION_MAJOR=2, FC_VERSION_MINOR=18, FC_VERSION_MICRO=3,
            ENABLE_FREETYPE=1, HAVE_PTHREAD=1, HAVE_C99_VSNPRINTF=1, ENABLE_NLS=1,
            USE_ICONV=1, LIBICONV_PLUG=1, SIZEOF_VOID_P=8, ALIGNOF_VOID_P=8, ALIGNOF_DOUBLE=8,
            FC_GPERF_SIZE_T="size_t", GETTEXT_PACKAGE='"fontconfig"', PACKAGE_NAME='"fontconfig"',
            PACKAGE_VERSION='"2.18.3"', PACKAGE_STRING='"fontconfig 2.18.3"', WORDS_BIGENDIAN=0,
            CONFIGDIR='"/etc/fonts/conf.d"', FC_CACHEDIR='"/var/cache/fontconfig"',
            FONTCONFIG_PATH='"/etc/fonts"', FC_TEMPLATEDIR='"/usr/share/fontconfig/conf.avail"',
            FC_DEFAULT_FONTS='"\\t<dir>/usr/share/fonts</dir>\\n"', FC_FONTPATH='""')
        (generated / "config.h").write_text("/* Native Darwin SDK-probed Fontconfig configuration. */\n" +
            "".join(f"#define {key} {value}\n" for key, value in sorted(configuration.items())) + '#include "config-fixups.h"\n')
        shutil.copyfile(source / "src/fcstdint.h.in", generated / "fcstdint.h")
        run(["python3", source / "src/makealias.py", source / "src", generated / "fcalias.h", generated / "fcaliastail.h",
             includes / "fontconfig.h", source / "src/fcdeprecate.h", includes / "fcprivate.h"])
        run(["python3", source / "src/makealias.py", source / "src", generated / "fcftalias.h", generated / "fcftaliastail.h",
             includes / "fcfreetype.h"])
        run(["python3", source / "fc-case/fc-case.py", source / "fc-case/CaseFolding.txt", "--template",
             source / "fc-case/fccase.tmpl.h", "--output", generated / "fc-case/fccase.h"])
        orth = re.search(r"orth_files = \[(.*?)\n\]", (source / "fc-lang/meson.build").read_text(), re.S)
        if not orth: raise ValueError("missing ordered Fontconfig orthography inventory")
        run(["python3", source / "fc-lang/fc-lang.py", *re.findall(r"'([^']+\.orth)'", orth[1]), "--template",
             source / "fc-lang/fclang.tmpl.h", "--output", generated / "fclang.h", "--directory", source / "fc-lang"])
        run(["python3", source / "fc-const/fc-const.py", source / "fc-const/fcconst.list", source / "src/fcobjs.h",
             "--output", generated / "fcconst.h"])
        run(["python3", source / "fc-genericfamily/fc-genericfamily.py", "-d", source / "fc-genericfamily",
             "-o", generated / "fcgenericfamily.gperf"])
        run([*flags, "-E", "-P", source / "src/fcobjshash.gperf.h", "-o", generated / "fcobjshash.preprocessed"])
        run(["python3", source / "src/cutout.py", generated / "fcobjshash.preprocessed", generated / "fcobjshash.gperf"])
        for name in ("fcgenericfamily", "fcobjshash"):
            run(["gperf", "--pic", "-m", "100", generated / (name + ".gperf"), "--output-file", generated / (name + ".h")])
        recipe = (source / "src/meson.build").read_text()
        match = re.search(r"fc_sources = files\(\[(.*?)\n\]\)", recipe, re.S)
        if not match: raise ValueError("missing Fontconfig source inventory")
        files = [source / "src" / name for name in re.findall(r"'([^']+\.c)'", match[1])] + [source / "src/fcpat.c"]
        jobs = [(path, generated / f"fontconfig_{index:03d}.o") for index, path in enumerate(files)]
        def compile_one(job):
            path, obj = job
            try:
                run([*flags, "-c", path, "-o", obj])
            except RuntimeError as error:
                return {"error": str(error)}
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native Fontconfig object")
            return {"source": str(path.relative_to(root)), "source_sha256": digest(path),
                    "member": obj.name, "object_sha256": digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool:
            records = list(pool.map(compile_one, jobs))
        errors = [record["error"] for record in records if "error" in record]
        if errors:
            raise ValueError("Fontconfig compilation failed:\n" + "\n".join(errors))
        output = install / "usr/lib/libfontconfig.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, obj in jobs]])
        # Compile upstream's actual cache-layout assertions for the target;
        # never substitute the host's architecture or execute target code here.
        run([*flags, "-c", source / "src/fcarch.c", "-o", generated / "fcarch.o"])
        configuration_dir = install / "etc/fonts"
        configuration_dir.mkdir(parents=True)
        replacements = {"FC_DEFAULT_FONTS": "\t<dir>/usr/share/fonts</dir>\n", "FC_FONTPATH": "",
                        "CONFIGDIR": "conf.d", "FC_CACHEDIR": "/var/cache/fontconfig"}
        text = re.sub(r"@(\w+)@", lambda match: replacements[match[1]], (source / "fonts.conf.in").read_text())
        (configuration_dir / "fonts.conf").write_text(text)
        license_dir = install / "usr/share/licenses/fontconfig"
        license_dir.mkdir(parents=True)
        shutil.copyfile(source / "COPYING", license_dir / "COPYING")
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
        public, imports, weak = symbols(output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": "2.18.3", "sources": records,
            "exports": sorted(public), "imports": sorted(imports - definitions(output)), "weak_imports": sorted(weak),
            "configuration": configuration, "sdk_features": features, "native_cache_layout_object_sha256": digest(generated / "fcarch.o"),
            "generated_headers": {str(path.relative_to(generated)): digest(path) for path in generated.rglob("*.h")},
            "bundle_sha256": digest(args.output), "system_requests": requests, "runtime_checked": False}, indent=2) + "\n")
        print(f"Native Fontconfig {args.arch}: {len(records)} genuine Mach-O source objects")


if __name__ == "__main__":
    main()
