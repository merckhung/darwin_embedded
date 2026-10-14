#!/usr/bin/env python3
"""Build real OpenLibm math and floating-point environment code as Darwin Mach-O.

Bazel owns the build. The upstream Make.files are read as source inventories;
neither Make nor CMake is executed. Darwin ARM64 uses 64-bit long double, while
x86_64 uses the upstream ld80 implementation and optimized x86 assembly.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile


def source_inventory(source, architecture):
    values = {"ARCH": "aarch64" if architecture == "arm64" else "amd64",
              "OS": "Darwin", "LONG_DOUBLE_NOT_DOUBLE": "0" if architecture == "arm64" else "1"}

    def read_inventory(directory):
        text = (source / directory / "Make.files").read_text().replace("\\\n", " ")
        enabled, result = [True], []
        for raw in text.splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            condition = re.fullmatch(r"(ifeq|ifneq)\s*\(\$\((\w+)\),\s*([^)]*)\)", line)
            if condition:
                kind, name, value = condition.groups()
                if name not in values:
                    raise ValueError(f"unknown OpenLibm source condition: {line}")
                match = values[name] == value.strip()
                enabled.append(enabled[-1] and (match if kind == "ifeq" else not match))
            elif line == "endif":
                if len(enabled) == 1:
                    raise ValueError("unbalanced source inventory condition")
                enabled.pop()
            else:
                assignment = re.fullmatch(r"\$\(CUR_SRCS\)\s*(\+?=)\s*(.*)", line)
                if not assignment:
                    raise ValueError(f"unsupported OpenLibm source inventory syntax: {line}")
                if enabled[-1]:
                    op, names = assignment.groups()
                    if op == "=":
                        result.clear()
                    for name in names.split():
                        if not re.fullmatch(r"[A-Za-z0-9_]+\.(c|S)", name):
                            raise ValueError(f"invalid OpenLibm source filename: {name}")
                        result.append(f"{directory}/{name}")
        if len(enabled) != 1:
            raise ValueError("unterminated source inventory condition")
        return result

    generic = read_inventory("src")
    optimized = read_inventory(values["ARCH"])
    replacements = {Path(name).stem for name in optimized if name.endswith(".S")}
    result = [name for name in generic if Path(name).stem not in replacements]
    result += optimized + read_inventory("bsdsrc")
    if architecture == "x86_64":
        result += read_inventory("ld80")
    if len(result) != len(set(result)):
        raise ValueError("duplicate OpenLibm sources")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--probe-output", type=Path)
    args = parser.parse_args()
    source = args.root.resolve() / "src/Libraries/25.5.0/openlibm"
    provenance = json.loads((source / "UPSTREAM.json").read_text())
    names = source_inventory(source, args.arch)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".native-math-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("native SDK architecture does not match OpenLibm")
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-fPIC",
                 "-nostdinc", "-std=c99", "-O2", "-Wall", "-Werror",
                 "-Wno-ignored-attributes", "-Wno-deprecated-non-prototype",
                 # Upstream's strict lane intentionally permits lround's
                 # integer-boundary conversion through double/float.
                 "-Wno-implicit-int-float-conversion", "-Wno-implicit-const-int-float-conversion",
                 "-DASSEMBLER", "-D__BSD_VISIBLE=1"]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        else:
            flags += ["-march=penryn"]
        for include in (source, source / "include", source / "src",
                        source / ("aarch64" if args.arch == "arm64" else "amd64"),
                        source / ("ld128" if args.arch == "arm64" else "ld80")):
            flags += ["-I", str(include)]
        flags += ["-isystem", str(sdk / "usr/include"), "-isystem", resource + "/include"]
        cpu = 0x100000C if args.arch == "arm64" else 0x1000007

        def compile_source(name):
            output = work / (name.replace("/", "_") + ".o")
            extra = [] if name.endswith(".S") else [
                "-include", str(args.root.resolve() / "userland/native/math/target_config.h")]
            if name.endswith("/fenv.c"):
                # The header supplies inline bodies; this upstream translation
                # unit's extern-inline declarations must emit public definitions.
                extra += ["-D__fenv_static="]
            result = subprocess.run([*flags, *extra, "-c", str(source / name), "-o", str(output)],
                                    capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(f"{name}:\n{result.stderr}")
            magic, actual_cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
            if (magic, actual_cpu, kind) != (0xFEEDFACF, cpu, 1):
                raise RuntimeError(f"foreign object format in native math: {name}")
            return output

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_source, names))
        output = work / "libmath.a"
        subprocess.run(["llvm-ar-20", "--format=darwin", "rcsD", str(output), *map(str, objects)], check=True)
        members = subprocess.check_output(["llvm-ar-20", "t", str(output)], text=True).splitlines()
        if members != [obj.name for obj in objects]:
            raise ValueError("native math archive inventory mismatch")
        defined, undefined = set(), set()
        for line in subprocess.check_output(["llvm-nm-20", "-g", str(output)], text=True).splitlines():
            fields = line.split()
            if len(fields) >= 2:
                (undefined if fields[-2] == "U" else defined).add(fields[-1])
        required = {"_sin", "_cos", "_tan", "_sqrt", "_exp", "_log", "_pow", "_hypot",
                    "_floor", "_ceil", "_fma", "_nan", "_sinf", "_sqrtf", "_sinl", "_sqrtl",
                    "_powl", "_cexp", "_csqrt", "_feclearexcept", "_fegetexceptflag",
                    "_fesetexceptflag", "_feraiseexcept", "_fetestexcept", "_fegetround",
                    "_fesetround", "_fegetenv", "_feholdexcept", "_fesetenv", "_feupdateenv"}
        if missing := required - defined:
            raise ValueError(f"missing native math exports: {sorted(missing)}")
        report = {"architecture": args.arch, "provider": provenance,
                  "sources": names, "object_count": len(objects), "exports": sorted(defined),
                  "remaining_external_references": sorted(undefined - defined),
                  "archive_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                  "validation": "native Mach-O objects, source inventory and exports; guest execution is separate"}
        if args.probe_output:
            probe = work / "test_math.o"
            subprocess.run([*flags, "-c", str(args.root.resolve() / "userland/native/math/test_math.c"),
                            "-o", str(probe)], check=True)
            magic, actual_cpu, _, kind = struct.unpack_from("<4I", probe.read_bytes())
            if (magic, actual_cpu, kind) != (0xFEEDFACF, cpu, 1):
                raise RuntimeError("math probe is not a native target Mach-O object")
            report["probe_sha256"] = hashlib.sha256(probe.read_bytes()).hexdigest()
            args.probe_output.parent.mkdir(parents=True, exist_ok=True)
            probe.replace(args.probe_output)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        output.replace(args.output)
        print(f"OpenLibm {args.arch}: {len(objects)} native Mach-O objects, {len(defined)} exports, "
              f"{len(undefined - defined)} remaining external references")


if __name__ == "__main__":
    main()
