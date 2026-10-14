#!/usr/bin/env python3
"""Install a signed Mach-O math/fenv probe using real source-built OpenLibm."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

from build_diagnostic_userland import PROGRAM_TYPES, verify_installed_files
from sign_diagnostic_macho import sign


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def symbols(path):
    public, undefined = set(), set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        if "external " not in line:
            continue
        match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
        if not match:
            raise RuntimeError("unrecognized native symbol listing: " + line)
        if "(undefined)" in line:
            undefined.add(match.group(1))
        elif "private external" not in line:
            public.add(match.group(1))
    return public, undefined


def audit_image(path, architecture, filetype):
    data = path.read_bytes()
    magic, cpu, _, actual_type, count, size, flags, _ = struct.unpack_from("<8I", data)
    expected_cpu = 0x100000C if architecture == "arm64" else 0x1000007
    if (magic, cpu, actual_type) != (0xFEEDFACF, expected_cpu, filetype):
        raise RuntimeError("unexpected native math image format: " + str(path))
    # The bounded native loader deliberately rejects weak coalescing. Only
    # the strong probe symbol is exported; private math aliases stay local.
    if flags & (0x8000 | 0x10000):
        raise RuntimeError("math test image requires unsupported weak coalescing")
    offset = 32
    for _ in range(count):
        command, command_size = struct.unpack_from("<2I", data, offset)
        if command_size < 8 or offset + command_size > 32 + size:
            raise RuntimeError("invalid native math load command")
        if command in (0x22, 0x80000022):
            if struct.unpack_from("<I", data, offset + 28)[0]:
                raise RuntimeError("math test image contains unsupported weak binds")
        offset += command_size
    if offset != 32 + size:
        raise RuntimeError("native math load command size mismatch")
    return {"sha256": hashlib.sha256(data).hexdigest(), "mach_header_flags": flags}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "base-root", "dyld", "libsyscall", "libmath", "probe",
                 "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-math-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Wall", "-Werror",
                 "-isystem", resource + "/include", "-I", root / "userland/native/dyld"]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        else:
            flags += ["-march=penryn"]
        main_object = work / "main.o"
        run([*flags, "-c", root / "userland/native/math/test_main.c", "-o", main_object])
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign"]
        library = work / "libnative_math.dylib"
        link_map = work / "math.map"
        run([*link, "-dylib", "-install_name", "/usr/lib/" + library.name,
             "-exported_symbol", "_native_math_probe", "-map", link_map, "-o", library,
             args.probe.resolve(), args.libmath.resolve()])
        public, imports = symbols(library)
        if public != {"_native_math_probe"} or imports:
            raise RuntimeError("math probe must be self-contained with one strong export")
        executable = work / "native-math-test"
        run([*link, "-pie", "-e", "_main", "-undefined", "dynamic_lookup", "-o", executable,
             main_object, library, args.libsyscall.resolve()])
        providers = public | symbols(args.dyld)[0] | {"dyld_stub_binder"}
        if missing := symbols(executable)[1] - providers:
            raise RuntimeError("missing actual native math executable imports: " + ", ".join(sorted(missing)))
        for path in (library, executable):
            sign(path)
        report = {
            "architecture": args.arch,
            "main": dict(audit_image(executable, args.arch, 2), imports=sorted(symbols(executable)[1])),
            "math": dict(audit_image(library, args.arch, 6), exports=sorted(public), imports=sorted(imports)),
            "linked_source_objects": [line for line in link_map.read_text().splitlines()
                                      if re.match(r"^\[\s*\d+\]", line)],
            "runtime_checked": False,
            "validation": "signed native images and closed imports; QEMU execution is a separate Bazel run",
        }
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*arguments):
            run([args.hfs_tool.resolve(), image, *arguments])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/usr/lib/dyld": args.dyld, "/bin/native-math-test": executable,
                     "/usr/lib/" + library.name: library}
        for name, path in installed.items():
            edit("add", path, name)
            edit("chmod", "755", name)
            expected[name] = path.read_bytes()
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        types = dict(PROGRAM_TYPES, **{"/bin/native-math-test": 2, "/usr/lib/" + library.name: 6})
        verify_installed_files(extracted, args.arch, expected, types)
        run(["fsck.hfsplus", "-n", image])
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
        image.replace(args.output)
        print(f"native math {args.arch}: closed actual math imports, signed guest root ready")


if __name__ == "__main__":
    main()
