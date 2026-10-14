#!/usr/bin/env python3
"""Cross-build Apple's native Darwin allocator from the pinned libmalloc sources."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def archive_symbols(path):
    defined, undefined = set(), set()
    for line in run(["llvm-nm-20", "-g", path]).splitlines():
        fields = line.split()
        if len(fields) >= 2:
            (undefined if fields[-2] == "U" else defined).add(fields[-1])
    return defined, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--mach-headers", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--probe-output", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--audit-archive", type=Path)
    parser.add_argument("--dependency", type=Path, action="append", default=[])
    args = parser.parse_args()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    if args.audit_archive:
        defined, undefined = archive_symbols(args.audit_archive)
        for dependency in args.dependency:
            more_defined, _ = archive_symbols(dependency)
            defined.update(more_defined)
        remaining = sorted(undefined - defined)
        args.report.write_text(json.dumps({
            "archive": args.audit_archive.name,
            "dependencies": [path.name for path in args.dependency],
            "remaining_external_references": remaining,
            "scope": "allocator references after available native archives; link and runtime checks remain separate",
        }, sort_keys=True, indent=2) + "\n")
        print(f"{args.audit_archive.name}: {len(remaining)} remaining native references")
        return
    if not all((args.sdk, args.mach_headers, args.output)):
        parser.error("archive build requires --sdk, --mach-headers and --output")
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-malloc-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        for source in (args.sdk, args.mach_headers):
            with tarfile.open(source) as archive:
                archive.extractall(sdk, filter="data")
        for manifest in ("DarwinSDK.json", "DarwinMachInterfaces.json"):
            if json.loads((sdk / manifest).read_text())["architecture"] != args.arch:
                raise SystemExit(f"{manifest} architecture does not match allocator target")
        source = work / "libmalloc"
        shutil.copytree(root / "src/Libraries/25.5.0/libmalloc", source)
        run(["patch", "--batch", "--fuzz=0", "-d", source, "-p1", "-i",
             root / "userland/native/malloc/build-config.patch"])
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        # The published resolver selects direct function addresses. Building
        # both NanoV2 halves produces one complete, generic native variant.
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fblocks", "-nostdinc", "-O2",
                 "-Wall", "-Werror", "-Wno-availability", "-Wno-deprecated-declarations",
                 "-Wno-format-invalid-specifier", "-Wno-format-extra-args",
                 "-Wno-self-assign",
                 "-Wno-unused-but-set-variable", "-Wno-unused-variable", "-Wno-unused-function", "-Wno-missing-braces",
                 "-Wno-gnu-folding-constant",
                 "-Wno-nullability-completeness", "-DPRIVATE=1", "-D_FORTIFY_SOURCE=0",
                 "-DNDEBUG=1", "-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1",
                 "-DTARGET_OS_BRIDGE=0",
                 "-DOSATOMIC_USE_INLINED=1", "-DOS_UNFAIR_LOCK_INLINE=1", "-DCONFIG_MTE=0",
                 "-DOS_VARIANT_RESOLVED=1", "-DOS_VARIANT_NOTRESOLVED=1",
                 "-DCONFIG_FEATUREFLAGS_SIMPLE=0", "-DNATIVE_DISABLE_DTRACE=1",
                 "-isystem", str(sdk / "usr/include"), "-isystem", str(sdk / "usr/local/include"),
                 "-isystem", resource + "/include",
                 "-isystem", str(root / "src/Kernel/xnu/12377.121.6/EXTERNAL_HEADERS"),
                 "-I", str(source / "include"), "-I", str(source / "private"),
                 "-I", str(source / "resolver"), "-I", str(source / "src")]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        excluded = {"malloc_exclaves.c", "malloc_type_stubs.c", "xzone_introspect_stub.c"}
        sources = [path for path in sorted((source / "src").rglob("*.c")) if path.name not in excluded]
        expected_cpu = 0x100000C if args.arch == "arm64" else 0x1000007
        def compile_source(item):
            output = work / ("_".join(item.relative_to(source).parts) + ".o")
            run([*flags, "-c", item, "-o", output])
            magic, cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
            if (magic, cpu, kind) != (0xFEEDFACF, expected_cpu, 1):
                raise SystemExit(f"wrong native allocator object format: {output}")
            return output
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            results = [executor.submit(compile_source, item) for item in sources]
            objects, failures = [], []
            for item, result in zip(sources, results):
                try:
                    objects.append(result.result())
                except RuntimeError as error:
                    failures.append(f"{item.relative_to(source)}:\n{error}")
            if failures:
                raise RuntimeError("\n".join(failures))
        archive = work / "libmalloc_base.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *objects])
        if run(["llvm-ar-20", "t", archive]).splitlines() != [item.name for item in objects]:
            raise SystemExit("native allocator archive inventory mismatch")
        exports = {line.split()[-1] for line in run([
            "llvm-nm-20", "--defined-only", "--extern-only", archive]).splitlines()
            if line and not line.endswith(":")}
        required = {"_malloc", "_free", "_calloc", "_realloc", "_posix_memalign",
                    "___malloc_init", "_malloc_zone_malloc", "_malloc_size",
                    "_nanov2_configure", "_nanov2_init", "_nanov2_create_zone", "_nanov2_forked_zone"}
        if missing := required - exports:
            raise SystemExit(f"missing native allocator exports: {sorted(missing)}")
        if args.probe_output:
            probe = work / "test_malloc.o"
            probe_flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                           "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Werror",
                           "-D_FORTIFY_SOURCE=0", "-Wno-availability", "-isystem", str(sdk / "usr/include"),
                           "-isystem", resource + "/include"]
            if args.arch == "arm64":
                probe_flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
            run([*probe_flags, "-c", root / "userland/native/malloc/test_malloc.c", "-o", probe])
            magic, cpu, _, kind = struct.unpack_from("<4I", probe.read_bytes())
            if (magic, cpu, kind) != (0xFEEDFACF, expected_cpu, 1):
                raise SystemExit("wrong native allocator consumer object format")
            args.probe_output.parent.mkdir(parents=True, exist_ok=True)
            probe.replace(args.probe_output)
        args.report.write_text(json.dumps({
            "architecture": args.arch,
            "component": "libmalloc",
            "source_version": "812.100.31",
            "build_config_patch_sha256": hashlib.sha256(
                (root / "userland/native/malloc/build-config.patch").read_bytes()).hexdigest(),
            "featureflags": False, "dtrace_probes": False, "memory_tagging": False,
            "objects": [obj.name for obj in objects],
            "exports": sorted(exports),
            "source_sha256": {str(item.relative_to(source)): hashlib.sha256(item.read_bytes()).hexdigest()
                              for item in sources},
            "validation": "Mach-O objects, public consumer compile, and exports; guest execution remains separate",
        }, sort_keys=True, indent=2) + "\n")
        archive.replace(args.output)
        print(f"native libmalloc {args.arch}: {len(objects)} Mach-O objects, {len(exports)} exports")


if __name__ == "__main__":
    main()
