#!/usr/bin/env python3
"""Boot a private root with an altered code page and require XNU to reject it."""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--root-image", type=Path, required=True)
    parser.add_argument("--hfs-tool", type=Path, default=ROOT / "bazel-bin/tools/libdmg-hfsplus/hfsplus")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--native-dylib", action="store_true",
                        help="alter the native loader test's leaf dylib and require its page rejection")
    args = parser.parse_args()
    output = ROOT / ".cache/qemu"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="signing-", dir=output) as directory:
        work = Path(directory)
        image, binary = work / "root.img", work / "altered-macho"
        guest_path = "/usr/lib/libnative_leaf.dylib" if args.native_dylib else "/sbin/launchd"
        shutil.copyfile(args.root_image, image)
        hfs = str(args.hfs_tool.resolve())
        subprocess.run([hfs, str(image), "extract", guest_path, str(binary)], check=True)
        data = bytearray(binary.read_bytes())
        if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xfeedfacf:
            raise RuntimeError("root executable is not a 64-bit Mach-O")
        cursor, entry = 32, None
        for _ in range(struct.unpack_from("<I", data, 16)[0]):
            command, size = struct.unpack_from("<II", data, cursor)
            if command == 0x80000028:
                entry = struct.unpack_from("<Q", data, cursor + 8)[0]
            elif args.native_dylib and command == 0x19:
                sections = struct.unpack_from("<I", data, cursor + 64)[0]
                for section in range(sections):
                    start = cursor + 72 + section * 80
                    if data[start:start + 16].rstrip(b"\0") == b"__text":
                        entry = struct.unpack_from("<I", data, start + 48)[0]
            if size < 8 or cursor + size > len(data):
                raise RuntimeError("invalid load command")
            cursor += size
        minimum = 32 if args.native_dylib else 4096
        if entry is None or not minimum <= entry < len(data):
            raise RuntimeError("expected entry in a signed code page after the header")
        data[entry] ^= 1
        binary.write_bytes(data)  # Deliberately retain the original signature.
        subprocess.run([hfs, str(image), "add", str(binary), guest_path], check=True)
        result = subprocess.run([sys.executable, str(ROOT / "scripts/verify_qemu_boot.py"),
                                 "--arch", args.arch, "--root-image", str(image),
                                 "--timeout", str(args.timeout),
                                 *(["--native-dyld-probe"] if args.native_dylib else [])],
                                capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        match = re.search(r"^Serial log: (.+)$", result.stdout, re.M)
        if not match or result.returncode == 0:
            raise RuntimeError("altered executable was not rejected")
        log = Path(match[1]).read_text(errors="replace")
        if args.native_dylib:
            rejected = re.search(r'CODE SIGNING: process [0-9]+\[native-loader[^]]*\]: '
                                 r'rejecting invalid page[^\n]*libnative_leaf\.dylib', log)
            if not rejected or "Verified interactive Darwin" not in result.stdout or "NATIVE_DYLD_OK:" in log:
                raise RuntimeError("test failed without evidence of native dylib hash rejection")
            print(f"Verified altered native dylib code-page rejection on {args.arch}")
            return
        if ('CODE SIGNING: process 1[launchd]: rejecting invalid page' not in log or
                'CS_KILLED initproc failed to start' not in log or
                'exit reason namespace 3 subcode 0x2' not in log):
            raise RuntimeError("boot failed without evidence of code-page hash rejection")
        print(f"Verified altered code-page rejection on {args.arch}")


if __name__ == "__main__":
    main()
