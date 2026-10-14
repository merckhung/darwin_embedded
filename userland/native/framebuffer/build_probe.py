#!/usr/bin/env python3
"""Compile the real native IOUserClient probe and request its actual imports."""
import argparse
from pathlib import Path
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--source", type=Path, default=Path(__file__).with_name("probe.c"))
    for name in ("sdk", "mach-headers", "iokit-headers", "base-exports", "output", "exports"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as directory:
        sdk = Path(directory).resolve()
        for path in (args.sdk, args.mach_headers, args.iokit_headers):
            with tarfile.open(path) as archive:
                archive.extractall(sdk, filter="data")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-Werror",
                 "-Wno-availability", "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0",
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include",
                 "-isystem", resource + "/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        run([*flags, "-c", args.source, "-o", args.output])
        imports = symbols(args.output)[1]
        imports |= set(args.base_exports.read_text().split())
        args.exports.write_text("\n".join(sorted(imports)) + "\n")


if __name__ == "__main__":
    main()
