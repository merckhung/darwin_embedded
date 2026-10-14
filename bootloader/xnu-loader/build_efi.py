#!/usr/bin/env python3
"""Compile and link the x86_64 EFI loader directly in its Bazel action."""
import argparse
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile


SOURCES = (
    "main.c app.c boot.c logo.c panic.c console.c devtree.c fdt_board.c "
    "devtree-fdt.c fileio.c jump.S lowmem.c macho.c serial.c"
).split()


def run(command, cwd):
    result = subprocess.run(list(map(str, command)), cwd=cwd, text=True,
                            capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" +
                           result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="xnu-efi-", dir=output.parent) as directory:
        work = Path(directory)
        # GCC's intermediate files belong to this Bazel action, not the host's
        # potentially small or shared /tmp filesystem.
        os.environ["TMPDIR"] = str(work)
        runtime = work / "runtime"
        with tarfile.open(args.runtime) as archive:
            archive.extractall(runtime, filter="data")
        include = runtime / "usr/include"
        lib = runtime / "usr/lib"
        common = ["gcc", "-std=gnu11", "-ffreestanding", "-fno-stack-protector",
                  "-fno-stack-check", "-funsigned-char", "-fshort-wchar", "-fpie",
                  "-Wall", "-Wextra", "-Wno-pointer-sign", "-ggdb", "-m64",
                  "-maccumulate-outgoing-args", "-mno-avx", "-mno-red-zone",
                  "-DEFI_FUNCTION_WRAPPER", "-DCONFIG_x86_64", "-DCONFIG_LOADER_x86_64",
                  "-fdebug-prefix-map=" + str(source) + "=xnu-loader",
                  "-fdebug-prefix-map=" + str(work) + "=xnu-loader-build",
                  "-Iinclude"]
        for path in (include, include / "efi", include / "efi/x86_64",
                     include / "protocol", include / "efi/protocol"):
            common += ["-I", path]

        def compile_one(name):
            obj = work / (name + ".o")
            run([*common, "-c", Path("src") / name, "-o", obj], source)
            return obj

        with ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_one, SOURCES))
        libgcc = run(["gcc", "-m64", "-print-libgcc-file-name"], source).strip()
        linked = work / "xnu-loader.so"
        run(["ld", "-nostdlib", "--warn-common", "--no-undefined", "--fatal-warnings",
             "--no-warn-rwx-segments", "-znocombreloc", "-shared", "-Bsymbolic",
             "-L", lib, lib / "crt0-efi-x86_64.o", *objects, "-o", linked,
             "-lgnuefi", "-lefi", "-T", lib / "elf_x86_64_efi.lds", libgcc], source)
        firmware = work / "BOOTX64.EFI"
        sections = (".text", ".sdata", ".data", ".rodata", ".dynamic", ".dynsym",
                    ".rel", ".rela", ".reloc")
        command = ["objcopy", "--input-target=elf64-x86-64"]
        for section in sections:
            command += ["-j", section]
        run([*command, "--output-target=efi-app-x86_64", linked, firmware], source)
        data = firmware.read_bytes()
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        if (data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0" or
                struct.unpack_from("<H", data, pe + 4)[0] != 0x8664 or
                struct.unpack_from("<H", data, pe + 24)[0] != 0x20b or
                struct.unpack_from("<H", data, pe + 24 + 68)[0] != 10):
            raise RuntimeError("Output is not an x86_64 PE32+ EFI application")
        firmware.replace(output)
        print(f"XNU EFI loader: compiled {len(objects)} sources and checked x86_64 PE32+ output")


if __name__ == "__main__":
    main()
