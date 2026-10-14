#!/usr/bin/env python3
"""Cross-build Apple's native Darwin BSD/Mach entry points as Mach-O objects.

This is a runtime component, not a complete libSystem. Higher-level libc,
pthread, MIG interfaces and dyld are built separately. No guest ABI translation
or replacement kernel syscall dispatch is involved.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--arch", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    xnu = args.root.resolve() / "src/Kernel/xnu/12377.121.6"
    library = xnu / "libsyscall"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    cpu = {"x86_64": 0x01000007, "arm64": 0x0100000c}[args.arch]
    with tempfile.TemporaryDirectory(prefix=".native-libsyscall-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk, stubs, objects = work / "sdk", work / "stubs", work / "objects"
        stubs.mkdir()
        objects.mkdir()
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        # Use the matching XNU generator, syscall table and native platform map.
        generated = subprocess.run([
            "perl", str(library / "xcodescripts/create-syscalls.pl"),
            str(xnu / "bsd/kern/syscalls.master"), str(library / "custom"),
            str(library / "Platforms"), "MacOSX", str(stubs)],
            env=dict(os.environ, ARCHS=args.arch), text=True, capture_output=True)
        if generated.returncode:
            raise RuntimeError(generated.stdout + generated.stderr)
        sources = [Path(line) for line in (stubs / "stubs.list").read_text().splitlines()]
        sources += [library / "mach/mach_traps.s", library / "custom/errno.c"]
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-DPRIVATE=1",
                 "-isystem", resource + "/include", "-isystem", str(sdk / "usr/include"),
                 "-I", str(stubs), "-I", str(library / "os")]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]

        def compile_source(source):
            output = objects / (source.name + ".o")
            language = "assembler-with-cpp" if source.suffix == ".s" else "c"
            result = subprocess.run([*flags, "-x", language, "-c", str(source), "-o", str(output)],
                                    text=True, capture_output=True)
            if result.returncode:
                raise RuntimeError(f"native {args.arch} compile failed: {source}\n{result.stderr}")
            data = output.read_bytes()
            if len(data) < 32:
                raise RuntimeError(f"truncated native object: {source}")
            magic, object_cpu, _, kind = struct.unpack_from("<4I", data)
            if (magic, kind) != (0xfeedfacf, 1):
                raise RuntimeError(f"expected a Mach-O object: {source}")
            if object_cpu != cpu:
                raise RuntimeError(f"foreign architecture in native archive: {source}")
            return output

        with ThreadPoolExecutor(max_workers=args.jobs) as workers:
            built = list(workers.map(compile_source, sources))
        archive = work / "libsyscall_base.a"
        subprocess.run(["llvm-ar-20", "--format=darwin", "rcsD", str(archive),
                        *map(str, built)], check=True)
        members = subprocess.check_output(["llvm-ar-20", "t", str(archive)], text=True).splitlines()
        if sorted(members) != sorted(path.name for path in built):
            raise RuntimeError("native archive member inventory mismatch")
        symbols = subprocess.check_output(["llvm-nm-20", "--defined-only", str(archive)], text=True)
        for symbol in ("___error", "_cerror_nocancel", "__thread_set_tsd_base", "_mach_msg2_trap"):
            if not any(line.split()[-1:] == [symbol] for line in symbols.splitlines()):
                raise RuntimeError(f"native archive is missing {symbol}")
        print(f"Built native {args.arch} libsyscall base: {len(built)} verified Mach-O objects")
        archive.replace(args.output)


if __name__ == "__main__":
    main()
