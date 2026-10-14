#!/usr/bin/env python3
"""Bazel-managed native Darwin PCRE2/libffi dependency build, without upstream build drivers."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
import build_intl

VERSIONS = {"pcre2": "10.49", "libffi": "3.8.0", "libiconv": "1.19", "gettext": "1.0"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(path, variable):
    text = path.read_text().replace("\\\n", " ")
    match = re.search(r"^" + re.escape(variable) + r"\s*=\s*(.*)$", text, re.M)
    if not match:
        raise ValueError("missing genuine source inventory: " + variable)
    return match[1].split()


def definitions(path):
    result = set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        if "external " in line and "(undefined)" not in line:
            match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
            if not match:
                raise ValueError("unrecognized Mach-O symbol " + line)
            result.add(match[1])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    upstream = {name: root / "src/Applications/glib" / name / version for name, version in VERSIONS.items()}
    for name, directory in upstream.items():
        manifest = json.loads((directory / "UPSTREAM.json").read_text())
        renames = {new: old for old, new in manifest["packaging_renames"].items()}
        actual = {renames.get(str(path.relative_to(directory)), str(path.relative_to(directory))): digest(path)
                  for path in directory.rglob("*") if path.is_file() and path.name != "UPSTREAM.json"}
        if actual != manifest["upstream_files_sha256"]:
            raise ValueError("upstream bytes changed: " + name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-glib-dependencies-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk = work / "sdk"
        for source in (args.sdk, args.mach_headers):
            with tarfile.open(source) as archive:
                archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("native SDK architecture mismatch")
        install = work / "install"
        headers, libraries = install / "usr/include", install / "usr/lib"
        headers.mkdir(parents=True)
        libraries.mkdir()
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-O2", "-fPIC", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include", "-I", headers]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        records, feature_records = {}, []

        def feature(name, text):
            path = work / ("feature_" + name + ".c")
            path.write_text(text)
            run([*flags, "-std=gnu11", "-Werror", "-c", path, "-o", path.with_suffix(".o")])
            feature_records.append({"name": name, "source": text, "object_sha256": digest(path.with_suffix(".o"))})

        feature("Darwin_ABI", "#include <sys/types.h>\n#include <stdint.h>\n"
                "_Static_assert(__APPLE__ && __MACH__, \"real Darwin target\");\n"
                "_Static_assert(sizeof(void*)==8 && sizeof(size_t)==8 && sizeof(long)==8 && sizeof(double)==8, \"LP64\");\n"
                f"_Static_assert(sizeof(long double)=={8 if args.arch == 'arm64' else 16}, \"native long double\");\n"
                "__int128 native_int128;\n")
        feature("native_threads", "#include <pthread.h>\npthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;\n")

        def build(name, project, sources, extra=(), config=None):
            build_dir = work / name
            build_dir.mkdir()
            jobs = [(original, compiled, build_dir / (f"{index:03}_" + original.stem + ".o"))
                    for index, (original, compiled) in enumerate(sources)]

            def compile_one(job):
                original, compiled, obj = job
                options = [*flags, *extra]
                if compiled.suffix != ".S": options += ["-std=gnu11"]
                run([*options, "-c", compiled, "-o", obj])
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("non-native dependency object: " + str(original))
                return {"source": str(original.relative_to(root)), "source_sha256": digest(original),
                        "compiled_sha256": digest(compiled), "member": obj.name, "object_sha256": digest(obj)}

            with ThreadPoolExecutor(max_workers=8) as pool:
                entries = list(pool.map(compile_one, jobs))
            output = libraries / (name + ".a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, _, obj in jobs]])
            if run(["llvm-ar-20", "t", output]).splitlines() != [obj.name for _, _, obj in jobs]:
                raise ValueError("archive inventory mismatch: " + name)
            public, imports, weak = symbols(output)
            records[name] = {"project": project, "version": VERSIONS[project], "sources": entries,
                "exports": sorted(public), "imports": sorted(imports - definitions(output)), "weak_imports": sorted(weak),
                "configuration": config or {}, "archive_sha256": digest(output)}

        pcre = upstream["pcre2"]
        generated = work / "pcre2-config"
        generated.mkdir()
        shutil.copyfile(pcre / "src/pcre2.h.generic", headers / "pcre2.h")
        pcre_config = (pcre / "src/config.h.generic").read_text()
        if pcre_config.count("#define PCRE2_EXPORT\n") != 1:
            raise ValueError("upstream generic PCRE2 visibility default changed")
        # The generic template explicitly strips visibility. Restore the
        # actual m4/pcre2_visibility.m4 choice supported by Darwin Clang.
        pcre_config = pcre_config.replace("#define PCRE2_EXPORT\n",
            '#define PCRE2_EXPORT __attribute__ ((visibility ("default")))\n')
        (generated / "config.h").write_text(pcre_config)
        shutil.copyfile(pcre / "src/pcre2_chartables.c.dist", generated / "pcre2_chartables.c")
        config = {"HAVE_CONFIG_H": 1, "PCRE2_CODE_UNIT_WIDTH": 8, "SUPPORT_PCRE2_8": 1,
                  "SUPPORT_UNICODE": 1, "HAVE_VISIBILITY": 1, "HAVE_PTHREAD": 1,
                  "HAVE_BUILTIN_MUL_OVERFLOW": 1, "HAVE_BUILTIN_UNREACHABLE": 1}
        for header in ("assert.h", "inttypes.h", "limits.h", "stdint.h", "stdio.h", "stdlib.h",
                       "strings.h", "string.h", "sys/stat.h", "sys/types.h", "unistd.h"):
            macro = "HAVE_" + header.upper().replace("/", "_").replace(".", "_")
            feature(macro, "#include <" + header + ">\n")
            config[macro] = 1
        sources = [(pcre / name, pcre / name) for name in inventory(pcre / "Makefile.am", "COMMON_SOURCES") if name.endswith(".c")]
        sources.append((pcre / "src/pcre2_chartables.c.dist", generated / "pcre2_chartables.c"))
        build("libpcre2-8", "pcre2", sources,
              ["-I", generated, "-I", pcre / "src", "-fvisibility=hidden",
               *[f"-D{key}={value}" for key, value in config.items()]], config)
        records["libpcre2-8"]["visibility"] = "upstream m4/pcre2_visibility.m4 hidden default and public PCRE2_EXPORT attribute"
        records["libpcre2-8"]["generated_config_sha256"] = digest(generated / "config.h")
        records["libpcre2-8"]["jit"] = "disabled using upstream supported interpreter configuration"
        records["libpcre2-8"]["character_tables"] = "unchanged upstream default ASCII/Unicode tables"

        ffi = upstream["libffi"]
        generated = work / "ffi-config"
        generated.mkdir()
        arch_dir = "aarch64" if args.arch == "arm64" else "x86"
        target = "AARCH64" if args.arch == "arm64" else "X86_64"
        config = {"HAVE_ALLOCA_H": 1, "HAVE_AS_CFI_PSEUDO_OP": 1, "HAVE_HIDDEN_VISIBILITY_ATTRIBUTE": 1,
                  "HAVE_INT128": 1, "HAVE_MEMCPY": 1, "SIZEOF_DOUBLE": 8, "SIZEOF_SIZE_T": 8,
                  "SIZEOF_LONG_DOUBLE": 8 if args.arch == "arm64" else 16, "STDC_HEADERS": 1,
                  "SYMBOL_UNDERSCORE": 1, "PACKAGE": '"libffi"', "PACKAGE_NAME": '"libffi"',
                  "PACKAGE_VERSION": '"3.8.0"', "VERSION": '"3.8.0"'}
        if args.arch == "arm64": config["FFI_EXEC_TRAMPOLINE_TABLE"] = 1
        else: config.update(FFI_MMAP_EXEC_WRIT=1, HAVE_LONG_DOUBLE=1, HAVE_AS_X86_PCREL=1)
        for header in ("alloca.h", "dlfcn.h", "inttypes.h", "stdint.h", "stdio.h", "stdlib.h",
                       "strings.h", "string.h", "sys/stat.h", "sys/types.h", "unistd.h"):
            macro = "HAVE_" + header.upper().replace("/", "_").replace(".", "_")
            feature("ffi_" + macro, "#include <" + header + ">\n")
            config[macro] = 1
        text = (ffi / "fficonfig.h.in").read_text()
        text = re.sub(r"^#undef (\w+)$", lambda match: "#define " + match[1] + " " + str(config[match[1]])
                      if match[1] in config else "/* #undef " + match[1] + " */", text, flags=re.M)
        (generated / "fficonfig.h").write_text(text)
        values = {"VERSION": "3.8.0", "TARGET": target, "HAVE_LONG_DOUBLE": "0" if args.arch == "arm64" else "1",
                  "FFI_VERSION_STRING": "3.8.0", "FFI_VERSION_NUMBER": "30800",
                  "FFI_EXEC_TRAMPOLINE_TABLE": "1" if args.arch == "arm64" else "0"}
        text = (ffi / "include/ffi.h.in").read_text()
        for key, value in values.items(): text = text.replace("@" + key + "@", value)
        if re.search(r"@\w+@", text): raise ValueError("unresolved ffi header substitution")
        (headers / "ffi.h").write_text(text)
        shutil.copyfile(ffi / "src" / arch_dir / "ffitarget.h", headers / "ffitarget.h")
        names = inventory(ffi / "Makefile.am", "libffi_la_SOURCES")
        architecture = ["ffi.c", "sysv.S"] if args.arch == "arm64" else ["ffi64.c", "unix64.S", "ffiw64.c", "win64.S"]
        names += ["src/" + arch_dir + "/" + name for name in architecture]
        sources = [(ffi / name, ffi / name) for name in names]
        build("libffi", "libffi", sources, ["-I", generated, "-I", ffi / "include", "-I", ffi / "src" / arch_dir], config)
        records["libffi"]["architecture_recipe"] = "unchanged configure.host Darwin LP64 inventory"

        iconv = upstream["libiconv"]
        generated = work / "iconv-config"
        generated.mkdir()
        config = {"ENABLE_EXTRA": 1, "HAVE_GETC_UNLOCKED": 1, "HAVE_LANGINFO_CODESET": 1,
                  "HAVE_MBRTOWC": 1, "HAVE_SETLOCALE": 1, "HAVE_STDDEF_H": 1,
                  "HAVE_STDLIB_H": 1, "HAVE_STRING_H": 1, "HAVE_VISIBILITY": 1,
                  "HAVE_WCRTOMB": 1, "HAVE_WORKING_O_NOFOLLOW": 1,
                  "WORDS_LITTLEENDIAN": 1, "INSTALLPREFIX": '"/usr"'}
        feature("iconv_native_multibyte", "#include <stdio.h>\n#include <locale.h>\n#include <langinfo.h>\n"
                "#include <wchar.h>\n#include <errno.h>\n#include <fcntl.h>\n"
                "_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, \"native byte order\");\n"
                "_Static_assert(EILSEQ == 92 && O_NOFOLLOW != 0, \"Darwin errno and open flags\");\n"
                "void *functions[]={(void*)getc_unlocked,(void*)nl_langinfo,(void*)mbrtowc,(void*)wcrtomb,(void*)setlocale};\n"
                "int charset_item=CODESET;\n")
        text = (iconv / "lib/config.h.in").read_text()
        text = re.sub(r"^#undef (\w+)$", lambda match: "#define " + match[1] + " " + str(config[match[1]])
                      if match[1] in config else "/* #undef " + match[1] + " */", text, flags=re.M)
        (generated / "config.h").write_text(text)
        text = (iconv / "include/iconv.h.in").read_text()
        for key, value in {"DLL_VARIABLE": "", "EILSEQ": "92", "ICONV_CONST": "",
                           "USE_MBSTATE_T": "1", "BROKEN_WCHAR_H": "0"}.items():
            text = text.replace("@" + key + "@", value)
        if re.search(r"@\w+@", text): raise ValueError("unresolved iconv header substitution")
        (headers / "iconv.h").write_text(text)
        shutil.copyfile(iconv / "libcharset/include/localcharset.h.in", headers / "localcharset.h")
        names = inventory(iconv / "lib/Makefile.in", "SOURCES")
        sources = []
        for name in names:
            original = Path(os.path.normpath(iconv / "lib" / name.removeprefix("$(srcdir)/")))
            sources.append((original, original))
        build("libiconv", "libiconv", sources,
              ["-I", generated, "-I", iconv / "lib", "-DBUILDING_LIBICONV=1", "-DBUILDING_LIBCHARSET=1"], config)
        records["libiconv"]["encodings"] = "complete upstream converter tables with ENABLE_EXTRA"
        records["libintl"] = build_intl.build(root, args.arch, sdk, work, headers, libraries, flags)
        license_sources = {"pcre2": ["COPYING"], "libffi": ["LICENSE"], "libiconv": ["COPYING.LIB", "libcharset/COPYING.LIB"],
                           "gettext": ["gettext-runtime/intl/COPYING.LIB"]}
        for project, names in license_sources.items():
            for name in names:
                path = install / "usr/share/licenses" / project / name
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(upstream[project] / name, path)
        report = {"architecture": args.arch, "components": records, "feature_checks": feature_records,
                  "runtime_checked": False, "target": args.arch + "-apple-macos26.5",
                  "sdk_sha256": digest(args.sdk), "mach_headers_sha256": digest(args.mach_headers),
                  "compiler": run(["clang-20", "--version"]).splitlines()[0],
                  "installed_headers_sha256": {str(path.relative_to(headers)): digest(path) for path in sorted(headers.rglob("*")) if path.is_file()}}
        imports = set().union(*(set(record["imports"]) for record in records.values()))
        exported = set().union(*(set(record["exports"]) for record in records.values()))
        report["imports"] = sorted(imports - exported)
        args.imports.write_text("\n".join(report["imports"]) + "\n")
        output = work / "dependencies.tar"
        with tarfile.open(output, "w") as archive:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    info = archive.gettarinfo(path, str(path.relative_to(install)))
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ""
                    info.mode = 0o644
                    with path.open("rb") as source: archive.addfile(info, source)
        report["bundle_sha256"] = digest(output)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        output.replace(args.output)
        print(f"Native GLib dependencies {args.arch}: " + ", ".join(f"{name} {len(record['sources'])} objects" for name, record in records.items()))


if __name__ == "__main__":
    main()
