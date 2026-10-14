#!/usr/bin/env python3
"""Build file-backed account lookup with native Darwin headers and imports."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "userland/native/accounts/accounts.c"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-accounts-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("account SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=c11", "-O2",
                 "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc",
                 "-D_DARWIN_C_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-availability",
                 "-isystem", resource + "/include", "-isystem", sdk / "usr/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        obj = work / "accounts.o"
        run([*flags, "-c", source, "-o", obj])
        magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
        if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
            raise ValueError("account object is not native Mach-O")
        archive = work / "accounts.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, obj])
        if run(["llvm-ar-20", "t", archive]).splitlines() != ["accounts.o"]:
            raise ValueError("account archive inventory mismatch")
        public, imports, weak = symbols(archive)
        if public != {"_getpwnam", "_getpwuid", "_getpwnam_r", "_getpwuid_r", "_getgrgid", "_getgrnam", "_getgrgid_r", "_getgrnam_r", "_getgrent", "_setgrent", "_endgrent", "_getgrouplist", "_getgroupcount", "_initgroups"} or weak:
            raise ValueError("unexpected account API exports")
        args.report.write_text(json.dumps({
            "architecture": args.arch, "backend": "Darwin /etc/master.passwd or /etc/passwd; reentrant /etc/group lookup",
            "exports": sorted(public), "imports": sorted(imports),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "validation": "native Mach-O archive; file parsing and thread behavior require guest execution",
        }, indent=2) + "\n")
        archive.replace(args.output)
        print(f"Native accounts {args.arch}: real file-backed name and UID lookup")


if __name__ == "__main__":
    main()
