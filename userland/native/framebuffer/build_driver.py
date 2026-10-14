#!/usr/bin/env python3
"""Bazel driver compilation against the matching source-generated XNU SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import tarfile
import tempfile


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "ld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--component", choices=("framebuffer", "input"), default="framebuffer")
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "userland/native" / args.component
    name = "NativeBootFramebuffer" if args.component == "framebuffer" else "NativeBootInput"
    xnu = root / "src/Kernel/xnu/12377.121.6"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-framebuffer-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        bundle = work / (name + ".kext/Contents")
        (bundle / "MacOS").mkdir(parents=True)
        info = plistlib.loads((source / "Info.plist").read_bytes())
        (bundle / "Info.plist").write_bytes(plistlib.dumps(info))
        metadata = work / "kmod_info.c"
        metadata.write_text('#include <mach/kmod.h>\n'
            'static kern_return_t module_start(kmod_info_t *m, void *p) { return KERN_SUCCESS; }\n'
            'static kern_return_t module_stop(kmod_info_t *m, void *p) { return KERN_FAILURE; }\n'
            'kmod_start_func_t *_realmain = module_start;\n'
            'extern kern_return_t _start(kmod_info_t *, void *);\n'
            f'KMOD_EXPLICIT_DECL(org.darwinembedded.driver.{name}, "1.0.0", _start, module_stop)\n')
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = [f"--target={args.arch}-apple-macos26.5", "-mkernel", "-ffreestanding", "-O2",
                 "-fno-stack-protector", "-fno-builtin", "-fblocks", "-fno-common", "-nostdinc",
                 "-DKERNEL=1", "-DKERNEL_PRIVATE=1", "-DPRIVATE=1", "-D__LITTLE_ENDIAN__",
                 "-D__PUREDARWIN__=1", "-isystem", resource + "/include", "-I", sdk / "usr/include",
                 "-I", source, "-I", root, "-include", xnu / "pd_apple_builtins.h"]
        flags += (["-mcpu=cortex-a72", "-mno-outline-atomics", "-DARM64_BOARD_CONFIG_BCM2711"]
                  if args.arch == "arm64" else ["-mno-red-zone"])
        objects = []
        backend = [source / "NativeDWC2.cpp"] if args.component == "input" and args.arch == "arm64" else []
        for number, file in enumerate([source / (name + ".cpp"), *backend, metadata, xnu / "libkern/kmod/c_start.c"]):
            obj = work / f"driver-{number}.o"
            options = (["-std=gnu++17", "-fapple-kext", "-fno-exceptions", "-fno-rtti", "-fno-use-cxa-atexit",
                        "-mllvm", "-disable-atexit-based-global-dtor-lowering"]
                       if file.suffix == ".cpp" else ["-std=gnu17"])
            run(["clang++-20" if file.suffix == ".cpp" else "clang-20", *flags, *options,
                 "-c", file, "-o", obj])
            objects.append(obj)
        executable = bundle / ("MacOS/" + name)
        run([args.ld.resolve(), "-arch", args.arch, "-kext", "-undefined", "dynamic_lookup",
             "-o", executable, *objects])
        with tarfile.open(args.output, "w") as archive:
            for file in sorted(bundle.parent.rglob("*")):
                if file.is_file():
                    entry = archive.gettarinfo(file, str(file.relative_to(work)))
                    entry.uid = entry.gid = entry.mtime = 0
                    entry.uname = entry.gname = ""
                    with file.open("rb") as stream:
                        archive.addfile(entry, stream)
        args.report.write_text(json.dumps({"architecture": args.arch,
            "sources": {file: hashlib.sha256((source / file).read_bytes()).hexdigest()
                        for file in [name + ".cpp", name + ".h", "geometry.h" if args.component == "framebuffer" else "ps2_decode.h", "Info.plist",
                                     *(["NativeDWC2.cpp", "NativeDWC2.h", "hid_decode.h"] if args.component == "input" and args.arch == "arm64" else [])]},
            **({"boot_metadata_header_sha256": hashlib.sha256((root / "bootloader/video/metadata.h").read_bytes()).hexdigest()}
               if args.component == "framebuffer" else {}),
            "sdk_sha256": hashlib.sha256(args.sdk.read_bytes()).hexdigest(),
            "image_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "archive_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
            "runtime_checked": False}, indent=2) + "\n")


if __name__ == "__main__":
    main()
