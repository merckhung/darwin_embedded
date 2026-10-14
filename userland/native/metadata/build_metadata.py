#!/usr/bin/env python3
"""Compile loader metadata guest checks against the genuine Darwin SDK."""
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
    for name in ("root", "sdk", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "userland/native/metadata/test_metadata.c"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-metadata-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        manifest = json.loads((sdk / "DarwinSDK.json").read_text())
        if manifest["architecture"] != args.arch:
            raise ValueError("native metadata SDK architecture mismatch")
        resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc",
                 "-Wall", "-Wextra", "-Werror", "-Wno-availability", "-D_FORTIFY_SOURCE=0",
                 "-isystem", str(sdk / "usr/include"), "-isystem", resource + "/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        output = work / "test_metadata.o"
        subprocess.run([*flags, "-c", str(source), "-o", str(output)], check=True)
        magic, cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
        if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
            raise ValueError("metadata probe is not a native Mach-O object")
        nm = subprocess.check_output(["llvm-nm-20", "-g", str(output)], text=True)
        imports = sorted(line.split()[-1] for line in nm.splitlines()
                         if len(line.split()) >= 2 and line.split()[-2] == "U")
        args.report.write_text(json.dumps({
            "architecture": args.arch, "sdk_dyld": manifest["projects"]["dyld"],
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "object_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "imports": imports,
            "validation": "Genuine SDK API/structure compilation and native Mach-O object; guest execution is separate.",
            "sdk_token_layout": "dyld1378 DyldAPIs.cpp packs dyld_build_version_t; deliberately version-local, not a future token ABI promise.",
        }, indent=2, sort_keys=True) + "\n")
        output.replace(args.output)
        print(f"Native metadata {args.arch}: genuine SDK guest probe compiled")


if __name__ == "__main__":
    main()
