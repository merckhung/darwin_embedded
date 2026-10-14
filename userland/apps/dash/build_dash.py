#!/usr/bin/env python3
"""Compile the actual upstream dash shell for Darwin inside Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile


def run(command, cwd=None):
    result = subprocess.run(list(map(str, command)), cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "exports", "report", "probe-output", "system-exports"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "src/Applications/dash/0.5.13.5"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-dash-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("dash SDK architecture mismatch")
        src = work / "src"
        shutil.copytree(source / "src", src)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-stack-check",
                 "-fPIC", "-nostdinc", "-Wall", "-Wextra", "-Wno-availability",
                 "-D_FORTIFY_SOURCE=0", "-D_DARWIN_C_SOURCE=1", "-DBSD=1", "-DSHELL=1",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include", "-I", src]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        # Test declarations against the actual Darwin headers. The link stage
        # separately requires real implementations for every resulting import.
        features = {}
        headers = "#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n#include <signal.h>\n#include <sys/resource.h>\n#include <sys/wait.h>\n#include <pwd.h>\n#include <ctype.h>\n#include <glob.h>\n#include <fnmatch.h>\n"
        for function in ("bsearch", "faccessat", "getpwnam", "getrlimit", "isalpha", "killpg",
                         "mempcpy", "memrchr", "sigsetmask", "stpcpy", "strchrnul", "strsignal",
                         "strtod", "strtoimax", "strtoumax", "sysconf", "wait3", "fnmatch", "glob"):
            probe = src / "feature.c"
            probe.write_text(headers + "#include <inttypes.h>\nvoid *native_feature = (void *)&" + function + ";\n")
            result = subprocess.run([*map(str, flags), "-Werror", "-Wno-deprecated-declarations",
                                     "-c", str(probe), "-o", str(src / "feature.o")], capture_output=True)
            features["HAVE_" + function.upper()] = result.returncode == 0
        for header in ("alloca.h", "paths.h", "sys/wait.h"):
            probe.write_text("#include <" + header + ">\n")
            result = subprocess.run([*map(str, flags), "-c", str(probe), "-o", str(src / "feature.o")], capture_output=True)
            features["HAVE_" + header.upper().replace("/", "_").replace(".", "_")] = result.returncode == 0
        probe.write_text("#include <stdint.h>\n#include <limits.h>\n#include <fcntl.h>\n#include <ctype.h>\n"
                         "_Static_assert(sizeof(intmax_t)==8 && sizeof(long long)==8 && CHAR_BIT==8 && CHAR_MIN==-128, \"Darwin shell types\");\n"
                         "int native_features = F_DUPFD_CLOEXEC;\nint native_blank(int c) { return isblank(c); }\n")
        run([*flags, "-Werror", "-c", probe, "-o", src / "feature.o"])
        features["HAVE_F_DUPFD_CLOEXEC"] = features["HAVE_DECL_ISBLANK"] = True
        declared_features = dict(features)
        # configure.ac enables these optional libc integrations only on an
        # explicit --enable-fnmatch/--enable-glob. Keep the upstream defaults:
        # the shell's own portable pattern and directory expansion code.
        # Darwin's GLOB_NOMAGIC preserves escaped original patterns, unlike
        # the glibc flag behavior assumed by the optional glob branch.
        features["HAVE_FNMATCH"] = features["HAVE_GLOB"] = False
        config = ["/* Native Darwin configuration verified by SDK compile checks. */",
                  "#include <sys/stat.h>", "#include <dirent.h>",
                  "_Static_assert(sizeof(off_t)==8 && sizeof(ino_t)==8, \"Native 64-bit file API\");"]
        config += [f"#define {key} 1" for key, enabled in sorted(features.items()) if enabled]
        config += ["#define SMALL 1", "#define WITH_LINENO 1", "#define USE_TEE 0", "#define USE_MEMFD_CREATE 0",
                   "#define SIZEOF_INTMAX_T 8", "#define SIZEOF_LONG_LONG_INT 8",
                   "#define PACKAGE_VERSION \"0.5.13.5\"",
                   "#define open64 open", "#define readdir64 readdir", "#define dirent64 dirent",
                   "#define fstat64 fstat", "#define lstat64 lstat", "#define stat64 stat", ""]
        # Darwin's selected ino64 APIs already have full-width off_t/ino_t.
        (src / "config.h").write_text("\n".join(config))
        (src / "config_defines.h").write_text("\n".join(line for line in config if line.startswith("#define")) + "\n")
        flags += ["-include", src / "config.h", "-D__DARWIN_64_BIT_INO_T=1"]
        run(["bash", src / "mktokens"], src)
        recipe = (src / "Makefile.am").read_text()
        match = re.search(r"dash_CFILES\s*=\s*((?:.*\\\n)*[^\n]+)", recipe)
        if not match:
            raise ValueError("upstream dash source inventory missing")
        sources = match.group(1).replace("\\\n", " ").split()
        if not sources or any(not (src / name).is_file() for name in sources):
            raise ValueError("incomplete upstream dash source inventory")
        # Host tools generate source only. Signal numbers are extracted from
        # the target SDK, so Linux host numbering can never enter the guest.
        target_macros = run([*flags, "-dM", "-E", "-x", "c", "-include", "signal.h", "/dev/null"])
        signals = dict(re.findall(r"^#define\s+(SIG[A-Z0-9]+|NSIG)\s+([^\n]+)$", target_macros, re.M))
        macro_values = dict(re.findall(r"^#define[ \t]+(\w+)[ \t]+([^\n]+)$", target_macros, re.M))
        for name, value in signals.items():
            for _ in range(16):
                expanded = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: macro_values.get(m[0], m[0]), value)
                if expanded == value:
                    break
                value = expanded
            if not re.fullmatch(r"[0-9xXa-fA-F() +\-*/|&<>~]+", value):
                raise ValueError("unresolved Darwin signal macro: " + name + "=" + value)
            signals[name] = value
        host_macros = run(["clang-20", "-dM", "-E", "-x", "c", "-include", "signal.h", "/dev/null"])
        host_signals = re.findall(r"^#define\s+(SIG[A-Z0-9]+|NSIG)\s+", host_macros, re.M)
        signal_header = ["/* Target signal values from the genuine Darwin SDK. */"]
        signal_header += [f"#undef {name}" for name in host_signals]
        signal_header += [f"#define {name} {value}" for name, value in sorted(signals.items())]
        (src / "native_signals.h").write_text("\n".join(signal_header) + "\n")
        generator = (src / "mksignames.c").read_text()
        signal_include = "\n#include <signal.h>\n"
        if generator.count(signal_include) != 1:
            raise ValueError("upstream signal generator header changed")
        (src / "mksignames.c").write_text(generator.replace(signal_include, "\n#include <signal.h>\n#include \"native_signals.h\"\n", 1))
        for name in ("mkinit", "mksyntax", "mknodes", "mksignames"):
            run(["clang-20", "-std=gnu11", "-O2", "-DBSD=1", "-DSHELL=1", "-I", src,
                 src / (name + ".c"), "-o", src / name])
        run([src / "mknodes", src / "nodetypes", src / "nodes.c.pat"], src)
        run([src / "mksyntax"], src)
        run(["clang-20", f"--target={args.arch}-apple-macos26.5", "-E", "-x", "c", "-nostdinc",
             "-include", src / "config_defines.h", src / "builtins.def.in", "-o", src / "builtins.def"])
        run(["bash", src / "mkbuiltins", src / "builtins.def"], src)
        run([src / "mkinit", *sources], src)
        sources += ["builtins.c", "init.c", "nodes.c", "signames.c", "syntax.c"]
        with (src / "signames.c").open("w") as stream:
            subprocess.run([str(src / "mksignames")], cwd=src, stdout=stream, check=True)
        cpu = 0x100000c if args.arch == "arm64" else 0x1000007

        def compile_source(item):
            obj = src / (item.replace("/", "_") + ".o")
            run([*flags, "-c", src / item, "-o", obj])
            magic, actual_cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, actual_cpu, kind) != (0xfeedfacf, cpu, 1):
                raise ValueError("foreign object in native dash")
            return obj

        with ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_source, sources))
        output = src / "dash.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", output, *objects])
        members = run(["llvm-ar-20", "t", output]).splitlines()
        if members != [obj.name for obj in objects]:
            raise ValueError("dash archive inventory mismatch")
        defined, undefined = set(), set()
        for line in run(["llvm-nm-20", "-g", output]).splitlines():
            fields = line.split()
            if len(fields) >= 2 and not line.endswith(":"):
                (undefined if fields[-2] == "U" else defined).add(fields[-1])
        if "_main" not in defined:
            raise ValueError("native dash main entry missing")
        imports = sorted(undefined - defined)
        args.exports.write_text("\n".join(imports) + "\n")
        probe = src / "test_main.o"
        run([*flags, "-Werror", "-I", args.root.resolve() / "src/Kernel/Extensions/25.5.0/hfs/core",
             "-c", args.root.resolve() / "userland/apps/dash/test_main.c", "-o", probe])
        magic, actual_cpu, _, kind = struct.unpack_from("<4I", probe.read_bytes())
        if (magic, actual_cpu, kind) != (0xfeedfacf, cpu, 1):
            raise ValueError("shell consumer is not native Mach-O")
        probe_imports = {line.split()[-1] for line in run(["llvm-nm-20", "-g", probe]).splitlines()
                         if len(line.split()) >= 2 and line.split()[-2] == "U"}
        args.system_exports.write_text("\n".join(sorted(set(imports) | probe_imports)) + "\n")
        args.report.write_text(json.dumps({
            "architecture": args.arch, "provider": json.loads((source / "UPSTREAM.json").read_text()),
            "sources": sources, "objects": members, "imports": imports,
            "test_consumer_imports": sorted(probe_imports),
            "test_consumer_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
            "sdk_declared_features": declared_features,
            "selected_features": features, "darwin_signals": signals,
            "source_sha256": {name: hashlib.sha256((source / "src" / name).read_bytes()).hexdigest()
                              for name in sources if (source / "src" / name).exists()},
            "archive_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "validation": "Native Mach-O source compilation and required import inventory; link and guest execution are separate.",
        }, indent=2, sort_keys=True) + "\n")
        output.replace(args.output)
        probe.replace(args.probe_output)
        print(f"Native dash {args.arch}: {len(objects)} Mach-O objects, {len(imports)} required native imports")


if __name__ == "__main__":
    main()
