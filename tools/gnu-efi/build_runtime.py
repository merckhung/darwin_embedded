#!/usr/bin/env python3
"""Build this repository's x86_64 GNU-EFI runtime inside a Bazel action.

The source list and flags follow GNU-EFI 3.0.18's lib/Makefile,
gnuefi/Makefile, and Make.defaults. No generated build system is invoked.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


LIB_SOURCES = (
    "boxdraw.c smbios.c console.c crc.c data.c debug.c dpath.c entry.c "
    "error.c event.c exit.c guid.c hand.c hw.c init.c lock.c misc.c pause.c "
    "print.c sread.c str.c cmdline.c runtime/rtlock.c runtime/efirtlib.c "
    "runtime/rtstr.c runtime/vm.c runtime/rtdata.c x86_64/initplat.c "
    "x86_64/math.c x86_64/setjmp.S x86_64/callwrap.c x86_64/efi_stub.S ctors.S"
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
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gnu-efi-", dir=output.parent) as directory:
        work = Path(directory)
        install = work / "install"
        lib = install / "usr/lib"
        lib.mkdir(parents=True)
        common = ["gcc", "-m64", "-std=c11", "-DCONFIG_x86_64",
                  "-DGNU_EFI_USE_MS_ABI", "-maccumulate-outgoing-args",
                  "-mno-red-zone", "-mno-avx", "-fPIE", "-g", "-O2",
                  "-Wall", "-Wextra", "-Wno-pointer-sign", "-Werror",
                  "-Wno-error=pragmas", "-funsigned-char", "-fshort-wchar",
                  "-fno-strict-aliasing", "-ffreestanding",
                  "-fno-stack-protector", "-fno-stack-check",
                  "-fno-merge-all-constants", "-fdebug-prefix-map=" + str(source) + "=gnu-efi",
                  "-Iinc", "-Iinc/x86_64", "-Iinc/protocol"]
        inputs = [Path("lib") / name for name in LIB_SOURCES]
        inputs += [Path("gnuefi/reloc_x86_64.c"), Path("gnuefi/crt0-efi-x86_64.S")]

        def compile_one(relative):
            obj = work / "objects" / relative.with_suffix(".o")
            obj.parent.mkdir(parents=True, exist_ok=True)
            run([*common, "-I" + str(relative.parts[0]), "-c", relative, "-o", obj], source)
            header = obj.read_bytes()[:20]
            if (header[:6] != b"\x7fELF\x02\x01" or
                    int.from_bytes(header[16:18], "little") != 1 or
                    int.from_bytes(header[18:20], "little") != 62):
                raise RuntimeError("GNU-EFI input is not an x86_64 relocatable object: " + str(relative))
            return obj

        with ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_one, inputs))
        run(["ar", "rcsD", lib / "libefi.a", *objects[:len(LIB_SOURCES)]], source)
        run(["ar", "rcsD", lib / "libgnuefi.a", objects[-2]], source)
        shutil.copyfile(objects[-1], lib / "crt0-efi-x86_64.o")
        shutil.copyfile(source / "gnuefi/elf_x86_64_efi.lds", lib / "elf_x86_64_efi.lds")
        for relative in ("", "protocol", "x86_64"):
            target = install / "usr/include/efi" / relative
            target.mkdir(parents=True, exist_ok=True)
            for header in sorted((source / "inc" / relative).glob("*.h")):
                shutil.copyfile(header, target / header.name)
        package = (source / "gnuefi/gnu-efi.pc.in").read_text()
        for key, value in {"PREFIX": "/usr", "EXEC_PREFIX": "/usr",
                           "INCLUDEDIR": "/usr/include", "LIBDIR": "/usr/lib",
                           "VERSION": "3.0.18"}.items():
            package = package.replace("@" + key + "@", value)
        (lib / "pkgconfig").mkdir()
        (lib / "pkgconfig/gnu-efi.pc").write_text(package)
        inventory = {str(path): hashlib.sha256((source / path).read_bytes()).hexdigest()
                     for path in inputs}
        (lib / "gnu-efi-build.json").write_text(json.dumps({
            "architecture": "x86_64", "version": "3.0.18",
            "compiler": run(["gcc", "--version"], source).splitlines()[0],
            "source_sha256": inventory,
        }, indent=2, sort_keys=True) + "\n")
        archive = work / "gnu-efi.tar"
        with tarfile.open(archive, "w") as stream:
            for path in sorted(install.rglob("*")):
                info = stream.gettarinfo(path, str(path.relative_to(install)))
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                info.mode = 0o755 if path.is_dir() else 0o644
                if path.is_file():
                    with path.open("rb") as contents:
                        stream.addfile(info, contents)
                else:
                    stream.addfile(info)
        archive.replace(output)
        print(f"GNU-EFI x86_64: compiled {len(objects)} genuine source objects with Bazel")


if __name__ == "__main__":
    main()
