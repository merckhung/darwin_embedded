#!/usr/bin/env python3
"""Build the native process-lifecycle consumer probe through Bazel."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile

p = argparse.ArgumentParser(description=__doc__)
for name in ("root", "sdk", "output", "exports"):
    p.add_argument("--" + name, type=Path, required=True)
p.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix=".native-lifecycle-", dir=a.output.parent) as directory:
    sdk = Path(directory).resolve()
    with tarfile.open(a.sdk) as archive:
        archive.extractall(sdk, filter="data")
    if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != a.arch:
        raise ValueError("native lifecycle SDK architecture mismatch")
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    flags = ["clang-20", f"--target={a.arch}-apple-macos26.5", "-ffreestanding", "-fno-builtin",
             "-fno-stack-protector", "-nostdinc", "-O2", "-Werror", "-Wno-availability",
             "-isystem", str(sdk / "usr/include"), "-isystem", resource + "/include"]
    if a.arch == "arm64": flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
    subprocess.run([*flags, "-c", str(a.root / "userland/native/system/test_lifecycle.c"),
                    "-o", str(a.output)], check=True)
    magic, cpu, _, kind = struct.unpack_from("<4I", a.output.read_bytes())
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if a.arch == "arm64" else 0x1000007, 1):
        raise ValueError("native lifecycle probe is not a target Mach-O object")
    undefined = subprocess.check_output(["llvm-nm-20", "--undefined-only", "--just-symbol-name", a.output], text=True)
    a.exports.write_text("\n".join(sorted(set(undefined.splitlines()))) + "\n")
