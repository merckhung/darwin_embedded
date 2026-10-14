#!/usr/bin/env python3
"""Build Apple's full native pthread sources for the Darwin QEMU CPU targets."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile


SOURCES = (
    "pthread.c", "pthread_atfork.c", "pthread_cancelable.c", "pthread_cond.c",
    "pthread_cwd.c", "pthread_dependency.c", "pthread_mutex.c", "pthread_rwlock.c",
    "pthread_tsd.c", "qos.c", "pthread_asm.s", "variants/pthread_cancelable_cancel.c",
    "resolver/resolver.c",
)
# Apple's libpthread_dyld target omits cancellation variants and assembly entry
# points, and keeps the real main-thread structure and early TSD initializer.
DYLD_SOURCES = tuple(name for name in SOURCES
                     if name not in ("pthread_asm.s", "variants/pthread_cancelable_cancel.c"))


def audit_closure(archive, dependencies, report):
    """Inventory real remaining references, without synthesizing definitions."""
    def symbols(path):
        defined, undefined = set(), set()
        output = subprocess.check_output(["llvm-nm-20", "-g", str(path)], text=True)
        for line in output.splitlines():
            fields = line.split()
            if len(fields) >= 2:
                (undefined if fields[-2] == "U" else defined).add(fields[-1])
        return defined, undefined

    defined, undefined = symbols(archive)
    for dependency in dependencies:
        more_defined, _ = symbols(dependency)
        defined.update(more_defined)
    unresolved = sorted(undefined - defined)
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps({
        "archive": archive.name,
        "dependencies": [path.name for path in dependencies],
        "remaining_external_references": unresolved,
        "scope": "pthread references after available native archives; link and runtime validation remain separate",
    }, sort_keys=True, indent=2) + "\n")
    print(f"{archive.name}: {len(unresolved)} remaining references after native dependencies")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--variant", choices=("runtime", "dyld"), default="runtime")
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--mach-headers", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--probe-output", type=Path)
    parser.add_argument("--audit-archive", type=Path)
    parser.add_argument("--dependency", type=Path, action="append", default=[])
    args = parser.parse_args()
    if args.audit_archive:
        audit_closure(args.audit_archive, args.dependency, args.report)
        return
    if not all((args.sdk, args.mach_headers, args.output)):
        parser.error("archive build requires --sdk, --mach-headers and --output")
    root = args.root.resolve()
    libs = root / "src/Libraries/25.5.0"
    source = libs / "libpthread"
    xnu = root / "src/Kernel/xnu/12377.121.6"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".native-pthread-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        for path in (args.sdk, args.mach_headers):
            with tarfile.open(path) as archive:
                archive.extractall(sdk, filter="data")
        for name in ("DarwinSDK.json", "DarwinMachInterfaces.json"):
            if json.loads((sdk / name).read_text())["architecture"] != args.arch:
                raise SystemExit(f"{name} architecture does not match pthread target")
        includes = (
            source, source / "src", source / "src/resolver", source / "include",
            source / "private", libs / "dyld/include", sdk / "usr/include",
            sdk / "usr/local/include",
            sdk / "System/Library/Frameworks/System.framework/Versions/B/PrivateHeaders",
            libs / "libplatform/private", libs / "libmalloc/private",
            xnu / "bsd", xnu / "libkern", xnu / "libsyscall/wrappers",
            xnu / "libsyscall/wrappers/spawn", xnu / "libsyscall/mach",
            root / "tools/cctools/include", libs / "Libc", libs / "libdispatch",
        )
        flags = [
            "clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
            "-fno-builtin", "-fno-stack-protector", "-fno-stack-check", "-nostdinc",
            "-O2", "-fblocks", "-D__LIBC__", "-D__POSIX_LIB__", "-D__DARWIN_UNIX03=1",
            "-D__DARWIN_64_BIT_INO_T=1", "-D__DARWIN_NON_CANCELABLE=1",
            "-D__DARWIN_VERS_1050=1", "-D_FORTIFY_SOURCE=0",
            "-D__PTHREAD_BUILDING_PTHREAD__=1", "-D__PTHREAD_EXPOSE_INTERNALS__",
            "-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1", "-DPRIVATE=1",
            "-DTARGET_OS_BRIDGE=0", "-DTARGET_OS_EXCLAVECORE=0", "-DTARGET_OS_EXCLAVEKIT=0",
            "-D__API_AVAILABLE_PLATFORM_xros(x)=visionos,introduced=x",
            "-Wno-int-conversion", "-Wno-availability", "-Wno-macro-redefined",
            "-Wno-nullability-completeness",
        ]
        if args.variant == "dyld":
            flags += ["-DVARIANT_STATIC=1", "-DVARIANT_DYLD=1"]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics", "-DNATIVE_CPU_CORTEX_A72=1"]
        else:
            flags += ["-march=penryn", "-DNATIVE_CPU_PENRYN=1"]
        for include in includes:
            flags += ["-I", str(include)]
        flags += ["-isystem", resource + "/include"]
        cpu = 0x100000C if args.arch == "arm64" else 0x1000007

        def compile_source(name):
            item = source / "src" / name
            output = work / (name.replace("/", "_") + ".o")
            extra = (["-x", "assembler-with-cpp"] if item.suffix == ".s" else
                     ["-include", str(root / "userland/native/pthread/target_config.h")])
            result = subprocess.run([*flags, *extra, "-c", str(item), "-o", str(output)],
                                    text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(f"{name}:\n{result.stderr}")
            magic, actual_cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
            if (magic, actual_cpu, kind) != (0xFEEDFACF, cpu, 1):
                raise RuntimeError(f"wrong Mach-O object format: {name}")
            return output

        sources = DYLD_SOURCES if args.variant == "dyld" else SOURCES
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_source, sources))
        output = work / "libpthread.a"
        subprocess.run(["llvm-ar-20", "--format=darwin", "rcsD", str(output),
                        *map(str, objects)], check=True)
        inventory = subprocess.check_output(["llvm-ar-20", "t", str(output)], text=True).splitlines()
        if inventory != [item.name for item in objects]:
            raise SystemExit("pthread archive inventory mismatch")
        symbols = subprocess.check_output(["llvm-nm-20", "--defined-only", "--extern-only",
                                           str(output)], text=True)
        exports = {line.split()[-1] for line in symbols.splitlines() if line and not line.endswith(":")}
        required = {"_pthread_create", "_pthread_join", "_pthread_exit", "_pthread_cancel",
                    "_pthread_mutex_lock", "_pthread_cond_wait", "_pthread_rwlock_rdlock",
                    "_pthread_key_create", "_pthread_setspecific", "___pthread_init",
                    "_pthread_jit_write_protect_supported_np", "_pthread_set_qos_class_self_np"}
        if args.variant == "dyld":
            required = {"__pthread_set_self", "__main_thread", "_pthread_mutex_lock",
                        "_pthread_self", "_pthread_jit_write_protect_supported_np"}
        if missing := required - exports:
            raise SystemExit(f"missing native pthread exports: {sorted(missing)}")
        report = {
            "architecture": args.arch,
            "component": "libpthread",
            "variant": args.variant,
            "source_version": "539.100.4",
            "cpu": "cortex-a72" if args.arch == "arm64" else "penryn",
            "per_thread_jit_write_protection": False,
            "objects": inventory,
            "exports": sorted(exports),
            "sources_sha256": {name: hashlib.sha256((source / "src" / name).read_bytes()).hexdigest()
                               for name in sources},
            "validation": "Mach-O archive build and export checks; guest runtime validation is separate",
        }
        report_path = work / "pthread.json"
        report_path.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
        if args.probe_output:
            # Compile the consumer with public headers and cancelable POSIX APIs,
            # independently of libpthread's private/noncancelable source flags.
            probe = work / "test_pthread.o"
            probe_flags = [
                "clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2",
                "-D_FORTIFY_SOURCE=0", "-Werror", "-Wno-availability",
                "-I", str(sdk / "usr/include"), "-isystem", resource + "/include",
            ]
            if args.arch == "arm64":
                probe_flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
            else:
                probe_flags += ["-march=penryn"]
            subprocess.run([*probe_flags, "-c", str(root / "userland/native/pthread/test_pthread.c"),
                            "-o", str(probe)], check=True)
            magic, actual_cpu, _, kind = struct.unpack_from("<4I", probe.read_bytes())
            if (magic, actual_cpu, kind) != (0xFEEDFACF, cpu, 1):
                raise SystemExit("wrong pthread probe Mach-O object format")
            args.probe_output.parent.mkdir(parents=True, exist_ok=True)
            probe.replace(args.probe_output)
        output.replace(args.output)
        report_path.replace(args.report)
        print(f"native libpthread {args.arch} {args.variant}: {len(objects)} Mach-O objects, {len(exports)} exports")


if __name__ == "__main__":
    main()
