#!/usr/bin/env python3
"""Compile native Xorg scanout against the actual Darwin/IOKit and server APIs."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glib"))
from build_dependencies import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "iokit-headers", "core", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--backend", choices=("display", "input"), default="display")
    args = parser.parse_args(); root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xorg-display-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, libraries = work / "sdk", work / "libraries"
        for path, destination in ((args.sdk, sdk), (args.mach_headers, sdk), (args.iokit_headers, sdk), (args.core, libraries)):
            with tarfile.open(path) as archive: archive.extractall(destination, filter="data")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        source = root / "src/Applications/xserver/xorg-server/21.1.25"
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_DIX_CONFIG_H",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include", "-isystem", resource + "/include",
                 "-I", libraries / "usr/include", "-I", libraries / "usr/include/pixman-1", "-I", libraries / "usr/libexec/xorg",
                 "-I", root / "userland/native/framebuffer", "-I", source]
        if args.backend == "input": flags += ["-I", root / "userland/native/input"]
        paths = re.search(r"^inc\s*=\s*include_directories\((.*?)\)", (source / "meson.build").read_text(), re.M | re.S)
        for include in re.findall(r"'([^']+)'", paths[1]): flags += ["-I", source / include]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        path = root / "userland/apps/x11" / ("xorg_" + args.backend + ".c")
        run([*flags, "-c", path, "-o", args.output])
        magic, cpu, _, kind = struct.unpack_from("<4I", args.output.read_bytes())
        if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1): raise ValueError("non-native Xorg display object")
        exported, imported, weak = symbols(args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "source_sha256": digest(path), "object_sha256": digest(args.output),
            "exports": sorted(exported), "imports": sorted(imported), "weak_imports": sorted(weak), "runtime_checked": False,
            args.backend: "Actual NativeBootFramebuffer IOUserClient mapping, firmware masks, console ownership, software MI cursor and fixed RandR output" if args.backend == "display" else
                          "Actual NativeBootInput IOUserClient reads, native HID keycodes, original XKB and DIX keyboard/pointer queues",
            "input_backend_supplied": args.backend == "input", "server_program_linked": False,
            **({"native_keycodes_sha256": digest(root / "userland/apps/x11/data/keycodes/nativehid"),
                "native_rules_sha256": digest(root / "userland/apps/x11/data/rules/nativehid")}
               if args.backend == "input" else {})}, indent=2) + "\n")
        print(f"Native Xorg {args.backend} {args.arch}: Mach-O object; server linkage and runtime pending")


if __name__ == "__main__": main()
