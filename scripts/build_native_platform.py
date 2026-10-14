#!/usr/bin/env python3
"""Cross-build Apple's native libplatform strings, atomics and locks as Mach-O."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--mach-headers", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "src/Libraries/25.5.0/libplatform"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".native-platform-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        for headers in (args.sdk, args.mach_headers):
            with tarfile.open(headers) as archive:
                archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise SystemExit("native SDK architecture does not match platform target")
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Wall", "-Werror",
                 "-Wno-availability", "-Wno-void-pointer-to-int-cast", "-Wno-int-conversion",
                 "-DPRIVATE=1", "-DVARIANT_STATIC=1",
                 "-DVARIANT_NO_RESOLVERS=1", "-D_FORTIFY_SOURCE=0",
                 "-DOSATOMIC_USE_INLINED=0", "-DOSATOMIC_DEPRECATED=0",
                 "-DOSSPINLOCK_USE_INLINED=0", "-DOSSPINLOCK_DEPRECATED=0",
                 "-DCONFIG_MTE=0",
                 "-isystem", str(sdk / "usr/include"), "-isystem", resource + "/include",
                 "-isystem", str(sdk / "usr/local/include"),
                 "-I", str(source / "private"), "-I", str(source / "include"),
                 "-I", str(source / "internal"), "-I", str(source / "src/os/resolver")]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        sources = sorted((source / "src/string/generic").glob("*.c"))
        sources += [source / name for name in (
            "src/os/lock.c", "src/os/alloc_once.c", "src/os/semaphore.c",
            "src/os/security_config.c", "src/os/script_config.c",
            "src/simple/getenv.c", "src/simple/string_io.c", "src/simple/asl.c", "src/simple/log_simple.c")]
        sources.append(source / ("src/os/atomic.c" if args.arch == "arm64" else "src/atomics/x86_64/OSAtomic.s"))
        if args.arch == "x86_64":
            sources += [source / "src/atomics/init.c", source / "src/atomics/x86_64/pfz.s"]
        objects = []
        for item in sources:
            output = work / ("_".join(item.relative_to(source).parts) + ".o")
            compile_item = item
            if item.name == "security_config.c":
                # This published file includes a private AppleFeatures header
                # but uses no definitions from it. Compile its actual body.
                original = item.read_text()
                unused_include = "#include <AppleFeatures/AppleFeatures.h>\n"
                if original.count(unused_include) != 1:
                    raise SystemExit("unexpected security configuration source")
                compile_item = work / item.name
                compile_item.write_text(original.replace(unused_include, "", 1))
            language = ["-x", "assembler-with-cpp"] if item.suffix == ".s" else []
            subprocess.run([*flags, *language, "-c", str(compile_item), "-o", str(output)], check=True)
            magic, cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
            expected_cpu = 0x100000C if args.arch == "arm64" else 0x1000007
            if (magic, cpu, kind) != (0xFEEDFACF, expected_cpu, 1):
                raise SystemExit(f"wrong native object format: {output}")
            objects.append(output)
        output = work / "libplatform_base.a"
        subprocess.run(["llvm-ar-20", "--format=darwin", "rcsD", str(output), *map(str, objects)], check=True)
        names = subprocess.check_output(["llvm-ar-20", "t", str(output)], text=True).splitlines()
        if names != [obj.name for obj in objects]:
            raise SystemExit("native platform archive inventory mismatch")
        exports = set(line.split()[-1] for line in subprocess.check_output(
            ["llvm-nm-20", "--defined-only", "--extern-only", str(output)], text=True).splitlines() if line and not line.endswith(":"))
        required = {"_memcpy", "_memmove", "_memset", "_strlen", "_strcmp", "_strlcpy",
                    "_os_unfair_lock_lock", "_os_unfair_lock_unlock", "_os_unfair_lock_trylock",
                    "_os_security_config_get", "___os_security_config_init", "_os_script_config_storage",
                    "__os_once", "__simple_getenv", "_OSAtomicAdd32", "_OSAtomicCompareAndSwap64"}
        if missing := required - exports:
            raise SystemExit(f"missing native platform exports: {sorted(missing)}")
        output.replace(args.output)
        print(f"native libplatform {args.arch}: {len(objects)} Mach-O objects, {len(exports)} exports")


if __name__ == "__main__":
    main()
