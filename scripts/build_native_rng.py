#!/usr/bin/env python3
"""Build the native XNU-backed RNG implementation inside a Bazel action."""
import argparse
import hashlib
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
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--probe-output", type=Path, required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "userland/native/rng"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-rng-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("native RNG SDK architecture mismatch")
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=c11", "-O2",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-stack-check",
                 "-nostdinc", "-Wall", "-Wextra", "-Werror", "-Wno-availability",
                 "-isystem", resource + "/include", "-isystem", str(sdk / "usr/include")]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        else:
            flags += ["-march=penryn"]
        cpu = 0x100000c if args.arch == "arm64" else 0x1000007
        objects = []
        for name in ("rng.c", "test_rng.c"):
            obj = work / (name + ".o")
            subprocess.run([*flags, "-c", str(source / name), "-o", str(obj)], check=True)
            magic, actual_cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, actual_cpu, kind) != (0xfeedfacf, cpu, 1):
                raise ValueError("foreign object in native RNG component")
            objects.append(obj)
        output = work / "rng.a"
        subprocess.run(["llvm-ar-20", "--format=darwin", "rcsD", str(output), str(objects[0])], check=True)
        if subprocess.check_output(["llvm-ar-20", "t", str(output)], text=True).splitlines() != ["rng.c.o"]:
            raise ValueError("native RNG archive inventory mismatch")
        nm = subprocess.check_output(["llvm-nm-20", "-g", str(output)], text=True)
        exports, imports = set(), set()
        for line in nm.splitlines():
            fields = line.split()
            if len(fields) >= 2 and not line.endswith(":"):
                (imports if fields[-2] == "U" else exports).add(fields[-1])
        if exports != {"_ccrng", "_ccrng_uniform"} or imports != {"_getentropy"}:
            raise ValueError(f"unexpected RNG symbol closure: {exports=}, {imports=}")
        args.report.write_text(json.dumps({
            "architecture": args.arch,
            "component": "Native ccrng API over XNU cryptographic RNG",
            "exports": sorted(exports), "imports": sorted(imports),
            "archive_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "source_sha256": hashlib.sha256((source / "rng.c").read_bytes()).hexdigest(),
            "validation": "native Mach-O archive and consumer compilation; guest execution is separate",
        }, indent=2) + "\n")
        output.replace(args.output)
        objects[1].replace(args.probe_output)
        print(f"Native RNG {args.arch}: real Darwin getentropy import and audited ccrng exports")


if __name__ == "__main__":
    main()
