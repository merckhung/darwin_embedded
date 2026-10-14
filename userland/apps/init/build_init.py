#!/usr/bin/env python3
"""Build the embedded PID 1 and uname with genuine Darwin SDK declarations."""
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
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    for name in ("root", "sdk", "init-output", "uname-output", "home-output", "support-output", "exports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.init_output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-init-", dir=args.init_output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("init SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc",
                 "-Wall", "-Wextra", "-Werror", "-Wno-availability", "-D_FORTIFY_SOURCE=0",
                 "-D_DARWIN_C_SOURCE=1", "-D__DARWIN_64_BIT_INO_T=1",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", root / "src/Kernel/Extensions/25.5.0/hfs/core"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        imports, objects = set(), {}
        for name, destination in (("init", args.init_output), ("uname", args.uname_output), ("test_home", args.home_output)):
            source = root / "userland/apps/init" / (name + ".c")
            output = work / (name + ".o")
            run([*flags, "-c", source, "-o", output])
            magic, cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native init object")
            public, undefined, weak = symbols(output)
            if "_main" not in public or weak:
                raise ValueError("init entry point or import inventory mismatch")
            imports |= undefined
            objects[name] = {"source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                             "object_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                             "imports": sorted(undefined)}
            output.replace(destination)
        unlink_source = root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers/unlink.c"
        unlink_object = work / "unlink.o"
        run([*flags, "-c", unlink_source, "-o", unlink_object])
        timestamp_source=unlink_source.with_name('utimensat.c')
        timestamp_object=work/'utimensat.o'
        run([*flags,'-DPRIVATE=1','-I',unlink_source.parent,'-c',timestamp_source,'-o',timestamp_object])
        run(["llvm-ar-20", "--format=darwin", "rcsD", args.support_output, unlink_object,timestamp_object])
        args.exports.write_text("\n".join(sorted(imports)) + "\n")
        args.report.write_text(json.dumps({"architecture": args.arch, "objects": objects,
            "native_support": {"source": str(unlink_source.relative_to(root)),
                               "source_sha256": hashlib.sha256(unlink_source.read_bytes()).hexdigest(),
                               "archive_sha256": hashlib.sha256(args.support_output.read_bytes()).hexdigest()},
            "timestamp_support":{"source":str(timestamp_source.relative_to(root)),"source_sha256":hashlib.sha256(timestamp_source.read_bytes()).hexdigest()},
            "hfs_mount_header": "src/Kernel/Extensions/25.5.0/hfs/core/hfs_mount.h",
            "hfs_mount_header_sha256": hashlib.sha256((root / "src/Kernel/Extensions/25.5.0/hfs/core/hfs_mount.h").read_bytes()).hexdigest(),
            "runtime_checked": False}, indent=2, sort_keys=True) + "\n")
        print(f"Native embedded init {args.arch}: 3 Mach-O objects, {len(imports)} actual API imports")


if __name__ == "__main__":
    main()
