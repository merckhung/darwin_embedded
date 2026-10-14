#!/usr/bin/env python3
"""Cross-build native Darwin libc from Apple's source inventory through Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile


def openstep(text):
    """Read the checked-in OpenStep project without an Xcode dependency."""
    pattern = re.compile(r'\s+|/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}()=;,]|[^\s{}()=;,]+', re.S)
    tokens = [m[0] for m in pattern.finditer(text)
              if not m[0].isspace() and not m[0].startswith(("/*", "//"))]
    cursor = 0
    def consume(expected=None):
        nonlocal cursor
        if cursor == len(tokens):
            raise ValueError("truncated Apple source inventory")
        token = tokens[cursor]
        cursor += 1
        if expected is not None and token != expected:
            raise ValueError(f"expected {expected}, got {token}")
        return json.loads(token) if token.startswith('"') else token
    def value():
        token = tokens[cursor]
        if token == "{":
            consume("{"); result = {}
            while tokens[cursor] != "}":
                key = consume(); consume("="); result[key] = value(); consume(";")
            consume("}"); return result
        if token == "(":
            consume("("); result = []
            while tokens[cursor] != ")":
                result.append(value())
                if tokens[cursor] == ",": consume(",")
            consume(")"); return result
        return consume()
    result = value()
    if cursor != len(tokens): raise ValueError("extra Apple inventory tokens")
    return result


def source_inventory(source):
    project = openstep((source / "Libc.xcodeproj/project.pbxproj").read_text())
    objects = project["objects"]
    paths = {}
    def visit(identifier, parent):
        item = objects[identifier]
        tree, relative = item.get("sourceTree", "<group>"), item.get("path", "")
        if tree == "SOURCE_ROOT": parent = source
        if tree == "<absolute>": parent = Path("/")
        path = parent / relative
        if item["isa"] in ("PBXGroup", "PBXVariantGroup"):
            for child in item.get("children", []): visit(child, path)
        elif item["isa"] == "PBXFileReference": paths[identifier] = path
    visit(objects[project["rootObject"]]["mainGroup"], source)
    result = {}
    for item in objects.values():
        if item["isa"] != "PBXNativeTarget" or item["name"] not in ("Base", "FreeBSD", "NetBSD", "TRE", "FortifySource"):
            continue
        for phase in item["buildPhases"]:
            if objects[phase]["isa"] != "PBXSourcesBuildPhase": continue
            for build in objects[phase]["files"]:
                reference = objects[build]["fileRef"]
                path = paths.get(reference)
                if path is None: raise ValueError(f"missing source path: {reference}")
                if path.name in ("kvm.c", "nlist.c", "OSMemoryNotification.c", "forceLibcToBuild.c"):
                    continue  # Apple's macOS/64-bit exclusions and generated archive trigger.
                if path.suffix not in (".c", ".S", ".s", ".m"): continue
                if not path.is_file(): raise ValueError(f"missing Apple libc source: {path}")
                result[str(path.relative_to(source))] = item["name"]
    return dict(sorted(result.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--mach-headers", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--inventory-only", action="store_true")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Libraries/25.5.0/Libc"
    inventory = source_inventory(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.inventory.write_text(json.dumps(inventory, indent=2) + "\n")
    if args.inventory_only:
        args.output.write_text(f"{len(inventory)} actual Apple libc sources\n")
        return
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".native-libc-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk = work / "sdk"
        for path in (args.sdk, args.mach_headers):
            with tarfile.open(path) as archive: archive.extractall(sdk, filter="data")
        for name in ("DarwinSDK.json", "DarwinMachInterfaces.json"):
            if json.loads((sdk / name).read_text())["architecture"] != args.arch:
                raise ValueError(f"native libc {name} architecture mismatch")
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-stack-check",
                 "-fblocks", "-nostdinc", "-O2", "-D__LIBC__=1", "-DPRIVATE=1",
                 "-D__DARWIN_UNIX03=1", "-D__DARWIN_64_BIT_INO_T=1",
                 "-D__DARWIN_NON_CANCELABLE=1", "-D__DARWIN_VERS_1050=1", "-D_FORTIFY_SOURCE=0",
                 "-D__DARWIN_ONLY_UNIX_CONFORMANCE=1", "-DXNU_PLATFORM_MacOSX=1",
                 "-DTARGET_OS_EXCLAVEKIT=0", "-DTARGET_OS_EXCLAVECORE=0", "-D__DBINTERFACE_PRIVATE=1",
                 "-DTARGET_OS_BRIDGE=0",
                 "-DDYLD_MACOSX_VERSION_15_4=0x000f0400",
                 "-Wno-nullability-completeness", "-Wno-availability"]
        if args.arch == "arm64": flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        for folder in (source, source / "include", source / "gen", source / "darwin",
                       source / "locale", source / "locale/FreeBSD", source / "stdtime/FreeBSD",
                       source / "stdio", source / "stdio/FreeBSD", source / "gen/FreeBSD",
                       sdk / "usr/local/include", sdk / "usr/include",
                       root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers",
                       root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers/libproc",
                       root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers/spawn",
                       root / "src/Libraries/25.5.0/libdispatch",
                       root / "src/Libraries/25.5.0/libdispatch/src/BlocksRuntime",
                       sdk / "System/Library/Frameworks/System.framework/Versions/B/PrivateHeaders"):
            flags += ["-I", str(folder)]
        flags += ["-isystem", resource + "/include"]
        expected_cpu = 0x100000c if args.arch == "arm64" else 0x1000007
        def compile_source(index_item):
            index, (relative, target) = index_item
            path = source / relative; output = work / f"{index:04d}_{path.name}.o"
            extra = []
            if target in ("FreeBSD", "NetBSD"):
                prefix = "fbsd" if target == "FreeBSD" else "nbsd"
                extra += ["-include", str(source / f"{prefix}compat/_{prefix}_compat_.h"),
                          "-I", str(source / (prefix + "compat"))]
            if target == "FreeBSD":
                extra += ["-I", str(source / "gdtoa"), "-I", str(source / "gdtoa/FreeBSD")]
            if target == "TRE":
                extra += ["-DHAVE_CONFIG_H", "-I", str(source / "regex/TRE"),
                          "-I", str(source / "regex/FreeBSD")]
            if relative in ("gen/FreeBSD/opendir.c", "gen/FreeBSD/readdir.c", "gen/FreeBSD/closedir.c",
                            "gen/FreeBSD/telldir.c", "gen/FreeBSD/rewinddir.c", "gen/FreeBSD/seekdir.c"):
                extra += ["-include", str(source / "gen/__dirent.h")]
            if path.suffix == ".s": extra += ["-x", "assembler-with-cpp"]
            if relative == "sys/settimeofday.c":
                # Select Apple's notification-free branch in this runtime,
                # which has no notifyd. The real kernel clock operation and
                # its error/errno handling remain the original implementation.
                text=path.read_text()
                needle="#if !TARGET_OS_DRIVERKIT"
                if text.count(needle)!=1:raise ValueError("Apple clock notification guard changed")
                path=work/("ported_"+path.name)
                path.write_text(text.replace(needle,"#if 0 /* Embedded runtime has no notifyd. */",1))
            if relative.startswith("uuid/"):
                extra += ["-include", str(source / "uuid/uuid-config.h")]
            if relative in ("gdtoa/FreeBSD/_hdtoa.c", "gdtoa/FreeBSD/_ldtoa.c"):
                # OpenLibm's classifier returns the standard C99 categories;
                # Apple's extra x87 category is not part of this provider.
                text = path.read_text()
                needle = "\tcase FP_SUPERNORMAL:"
                if text.count(needle) != 1: raise ValueError("Apple dtoa classifier source changed")
                text = text.replace(needle, "#ifdef FP_SUPERNORMAL\n" + needle + "\n#endif")
                path = work / ("ported_" + path.name)
                path.write_text(text)
            if relative in ("gen/FreeBSD/arc4random.c", "stdio/FreeBSD/vfprintf.c"):
                # Select Apple's existing failure/trap branch until the real
                # experimental libtrace formatter is available. Entropy errors
                # and format faults still terminate; no success is fabricated.
                text = path.read_text()
                needle = "#define OS_CRASH_ENABLE_EXPERIMENTAL_LIBTRACE 1"
                if text.count(needle) != 1: raise ValueError("Apple crash configuration changed")
                text = text.replace(needle, "/* Native build uses the existing non-libtrace crash path. */")
                if relative == "gen/FreeBSD/arc4random.c":
                    # The native provider propagates kernel RNG failures. The
                    # public arc4random APIs have no error result, so abort
                    # rather than consume uninitialized or cleared output.
                    replacements = {
                        "\tccrng_generate(rng, buf_size, buf);":
                            "\tif (ccrng_generate(rng, buf_size, buf))\n"
                            "\t\tos_crash(\"arc4random: native RNG generation failed\");",
                        "\tccrng_uniform(rng, upper_bound, &rand);":
                            "\tif (ccrng_uniform(rng, upper_bound, &rand))\n"
                            "\t\tos_crash(\"arc4random: native uniform generation failed\");",
                        "arc4random_uniform(uint32_t upper_bound)\n{\n\tuint64_t rand;":
                            "arc4random_uniform(uint32_t upper_bound)\n{\n"
                            "\tif (upper_bound < 2) return 0;\n\tuint64_t rand;",
                    }
                    for before, after in replacements.items():
                        if text.count(before) != 1:
                            raise ValueError("Apple arc4random provider source changed")
                        text = text.replace(before, after)
                path = work / ("ported_" + path.name)
                path.write_text(text)
            command = [flags[0], *extra, *flags[1:], "-c", str(path), "-o", str(output)]
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode: return relative, result.stdout, None
            if struct.unpack_from("<4I", output.read_bytes())[::3] != (0xfeedfacf, 1) or \
                    struct.unpack_from("<I", output.read_bytes(), 4)[0] != expected_cpu:
                raise ValueError(f"foreign object in native libc: {relative}")
            return relative, None, output
        with ThreadPoolExecutor(max_workers=args.jobs) as workers:
            results = list(workers.map(compile_source, enumerate(inventory.items())))
        failures = {relative: error for relative, error, _ in results if error}
        if failures:
            categories = {}
            for error in failures.values():
                key = re.search(r"fatal error: (.+)|error: (.+)", error)
                key = key[0] if key else error[:120]
                categories[key] = categories.get(key, 0) + 1
            print(json.dumps(dict(sorted(categories.items(), key=lambda x: -x[1])), indent=2))
            for relative, error in list(failures.items())[:5]: print(relative + "\n" + error)
            raise SystemExit(f"native libc {args.arch}: {len(results)-len(failures)}/{len(results)} sources compile")
        archive = work / "libc_base.a"
        subprocess.run(["llvm-ar-20", "--format=darwin", "rcsD", str(archive),
                        *map(str, (obj for _, _, obj in results))], check=True)
        members = subprocess.check_output(["llvm-ar-20", "t", str(archive)], text=True).splitlines()
        if members != [obj.name for _, _, obj in results]:
            raise ValueError("native libc archive inventory mismatch")
        symbols = subprocess.check_output(["llvm-nm-20", "--defined-only", "--extern-only",
                                           str(archive)], text=True)
        exports = {line.split()[-1] for line in symbols.splitlines() if line and not line.endswith(":")}
        required = {"_printf", "_fopen", "_exit", "_getenv", "_setenv", "_qsort", "_strtol",
                    "_abort", "_arc4random", "_regcomp", "_mbrtowc", "___libc_init"}
        if missing := required - exports:
            raise ValueError(f"native libc missing exports: {sorted(missing)}")
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({
            "architecture": args.arch,
            "component": "Libc",
            "upstream": json.loads((source / "UPSTREAM.json").read_text()),
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "objects": members,
            "exports": sorted(exports),
            "sources_sha256": {relative: hashlib.sha256((source / relative).read_bytes()).hexdigest()
                               for relative in inventory},
            "portability_changes": [
                "Guard Apple's FP_SUPERNORMAL cases for the OpenLibm C99 classifier.",
                "Select existing nonexperimental crash paths in arc4random and vfprintf.",
                "Guard arc4random_uniform bounds below two and abort on native RNG provider errors.",
                "Select Apple's notification-free settimeofday branch; preserve the genuine kernel clock operation and errno handling without notifyd.",
            ],
            "validation": "Mach-O archive, architecture, member inventory and exports; full runtime linkage and guest execution remain separate",
        }, indent=2, sort_keys=True) + "\n")
        archive.replace(args.output)
        print(f"native libc {args.arch}: {len(results)} verified Mach-O objects, {len(exports)} exports")


if __name__ == "__main__":
    main()
