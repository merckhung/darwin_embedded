#!/usr/bin/env python3
"""Build real Apple IOKit connections and XNU MIG clients, without CF substitutes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

FUNCTIONS = ("IOObjectRelease", "IOIteratorNext", "IOServiceOpen", "IOServiceClose",
             "IOConnectMapMemory64", "IOConnectUnmapMemory64", "IOConnectCallMethod",
             "IOConnectCallStructMethod")


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def digest(data):
    return hashlib.sha256(data).hexdigest()


def function(source, name):
    match = re.search(r"^(?:kern_return_t|io_object_t)\s+" + re.escape(name) + r"\s*\([^{};]*\{", source, re.M)
    if not match:
        raise ValueError("upstream function signature not found: " + name)
    depth = 1
    end = match.end()
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise ValueError("unterminated upstream function: " + name)
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "migcom", "output", "headers", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "userland/native/iokit/upstream"
    upstream = (source / "IOKitLib.c").read_text()
    public = (source / "IOKitLib.h").read_text()
    manifest = json.loads((source / "UPSTREAM.json").read_text())
    for name, expected in manifest["selected_files_sha256"].items():
        if digest((source / name).read_bytes()) != expected:
            raise ValueError("changed upstream IOKit source: " + name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-iokit-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk, installed = work / "sdk", work / "headers/usr/include"
        installed.mkdir(parents=True)
        for package in (args.sdk, args.mach_headers):
            with tarfile.open(package) as archive:
                archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("IOKit SDK architecture mismatch")
        xnu = root / "src/Kernel/xnu/12377.121.6"
        includes = ["-I" + str(path) for path in (sdk / "usr/include", xnu / "iokit", xnu / "osfmk")]
        environment = dict(os.environ, MIGCC="clang-20", MIGCOM=str(args.migcom.resolve()))
        generated = work / "iokitUser.c"
        header = installed / "NativeIOKitMIG.h"
        run(["bash", root / "tools/mig/mig.sh", "-target", f"{args.arch}-apple-macos26.5",
             "-arch", args.arch, "-novouchers", "-DKOBJECT_SERVER", "-DIOKIT=1", "-DPRIVATE=1",
             "-nostdinc", *includes, "-server", "/dev/null", "-header", header,
             "-user", generated, xnu / "osfmk/device/device.defs"], env=environment)
        license_text = upstream[:upstream.index("*/") + 2]
        declarations, bodies, selected = [], [], {}
        for name in FUNCTIONS:
            pattern = r"^(?:kern_return_t|io_object_t)\s+" + name + r"\s*\([^;]*;"
            declaration = re.search(pattern, public, re.M)
            if not declaration:
                raise ValueError("genuine IOKit public declaration missing: " + name)
            declarations.append(declaration.group())
            body = function(upstream, name)
            bodies.append(body)
            selected[name] = digest(body.encode())
        api = installed / "NativeIOKit.h"
        api.write_text(license_text + "\n/* Exact selected Apple declarations; not a full IOKit SDK. */\n"
            "#ifndef NATIVE_IOKIT_H\n#define NATIVE_IOKIT_H\n"
            "#include <AvailabilityMacros.h>\n"
            "#include <IOKit/IOTypes.h>\n#include <device/device_types.h>\n#include <mach/mach.h>\n"
            "__BEGIN_DECLS\n" + "\n\n".join(declarations) + "\n__END_DECLS\n#endif\n")
        helper = re.search(r"^#define reinterpret_cast_mach_vm_address_t\(p\) \\\n[^\n]+", upstream, re.M)
        if helper is None:
            raise ValueError("genuine IOKit address conversion macro missing")
        implementation = work / "NativeIOKit.c"
        implementation.write_text(license_text + "\n#include <NativeIOKit.h>\n#include <NativeIOKitMIG.h>\n" +
                                  helper.group() + "\n\n" + "\n\n".join(bodies) + "\n")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-DIOKIT=1",
                 "-DPRIVATE=1", "-D_DARWIN_C_SOURCE=1", "-Werror=implicit-function-declaration",
                 "-isystem", resource + "/include", "-I", installed, *includes]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        objects = []
        for file in (generated, implementation):
            obj = file.with_suffix(".o")
            run([*flags, "-c", file, "-o", obj])
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("IOKit object is not target Mach-O")
            objects.append(obj)
        run(["llvm-ar-20", "--format=darwin", "rcsD", args.output, *objects])
        # The corresponding real XNU types accompany the generated declarations.
        for name in ("IOTypes.h", "IOReturn.h", "IOMapTypes.h"):
            destination = installed / "IOKit" / name
            destination.parent.mkdir(exist_ok=True)
            shutil.copyfile(xnu / "iokit/IOKit" / name, destination)
        with tarfile.open(args.headers, "w") as archive:
            for file in sorted((work / "headers").rglob("*")):
                if file.is_file():
                    info = archive.gettarinfo(file, str(file.relative_to(work / "headers")))
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ""
                    with file.open("rb") as stream:
                        archive.addfile(info, stream)
        args.report.write_text(json.dumps({"architecture": args.arch, "source": manifest,
            "selected_function_sha256": selected, "generated_client_sha256": digest(generated.read_bytes()),
            "kernel_interface": "xnu-12377.121.6/osfmk/device/device.defs",
            "kernel_interface_sha256": digest((xnu / "osfmk/device/device.defs").read_bytes()),
            "archive_sha256": digest(args.output.read_bytes()), "runtime_checked": False,
            "scope": "Eight unchanged Apple IOKit functions and matching XNU MIG clients. No CoreFoundation matching/serialization or dispatch-backed IOMasterPort implementation is advertised."}, indent=2) + "\n")
        print(f"Native IOKit {args.arch}: {len(FUNCTIONS)} actual Apple APIs and genuine XNU MIG client")


if __name__ == "__main__":
    main()
