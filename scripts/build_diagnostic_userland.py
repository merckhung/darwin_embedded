#!/usr/bin/env python3
"""Build a freestanding Darwin diagnostic userland and a 64 MiB HFS+ root."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from sign_diagnostic_macho import sign


PROGRAM_TYPES = {"/sbin/launchd": 2, "/bin/sh": 2, "/bin/uname": 2, "/usr/lib/dyld": 7}
CPU_TYPES = {"x86_64": 0x01000007, "arm64": 0x0100000c}


def verify_installed_files(directory: Path, arch: str, expected: dict, program_types=PROGRAM_TYPES):
    """Check the entire extracted minimal root, including each native program."""
    actual = {"/" + str(path.relative_to(directory)): path
              for path in directory.rglob("*") if path.is_file()}
    if actual.keys() != expected.keys():
        raise ValueError(f"unexpected root contents: missing={sorted(expected.keys() - actual.keys())}, "
                         f"extra={sorted(actual.keys() - expected.keys())}")
    for name, path in actual.items():
        data = path.read_bytes()
        if data.startswith(b"\x7fELF"):
            raise ValueError(f"ELF is forbidden in the Darwin root: {name}")
        if name in program_types:
            if len(data) < 32:
                raise ValueError(f"truncated Mach-O: {name}")
            magic, cpu, _, kind, *_ = struct.unpack_from("<8I", data)
            if (magic, cpu, kind) != (0xfeedfacf, CPU_TYPES[arch], program_types[name]):
                raise ValueError(f"expected native {arch} Mach-O at {name}")
        if data != expected[name]:
            raise ValueError(f"installed root file does not match its source: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--ld", type=Path, required=True)
    parser.add_argument("--hfs-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "userland/diagnostic"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".darwin-root-", dir=args.output.parent) as directory:
        work = Path(directory)
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-nostdinc", "-isystem", resource + "/include", "-O2"]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        objects = []
        for name in ("diagnostic.c", "start.S", "dyld.c", "dyld_start.S"):
            output = work / (name + ".o")
            subprocess.run([*flags, "-c", str(source / name), "-o", str(output)], check=True)
            objects.append(str(output))
        binary = work / "diagnostic"
        # ld64.lld supports freestanding dynamic executables without libSystem.
        # Apple's ld64 requires libSystem even when no symbols are imported.
        subprocess.run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-pie", "-no_fixup_chains", "-e", "_start",
                        "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                        "-o", str(binary), *objects[:2]], check=True)
        dyld = work / "dyld"
        subprocess.run([str(args.ld.resolve()), "-arch", args.arch, "-dylinker", "-e", "_dyld_start",
                        "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                        "-o", str(dyld), *objects[2:]], check=True)
        # The Linux ld64 libcodedirectory shim leaves an empty signature command.
        # Supply genuine SHA-256 code-page signatures and require invalid-page kills.
        sign(binary)
        sign(dyld)
        image = work / "root.img"
        with image.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024)
        subprocess.run(["mkfs.hfsplus", "-v", "DarwinEmbedded", str(image)], check=True)
        def edit(*command):
            subprocess.run([str(args.hfs_tool.resolve()), str(image), *command], check=True)
        for folder in ("/sbin", "/bin", "/dev", "/etc", "/tmp", "/usr", "/usr/lib"):
            edit("mkdir", folder)
        installed = {}
        for destination in ("/sbin/launchd", "/bin/sh", "/bin/uname"):
            edit("add", str(binary), destination)
            edit("chmod", "755", destination)
            installed[destination] = binary.read_bytes()
        edit("add", str(dyld), "/usr/lib/dyld")
        edit("chmod", "755", "/usr/lib/dyld")
        installed["/usr/lib/dyld"] = dyld.read_bytes()
        about = work / "os-release"
        about.write_text('NAME="Darwin Embedded diagnostic userland"\n'
                         'DESCRIPTION="Freestanding shell for boot, filesystem and BSD syscall verification"\n')
        edit("add", str(about), "/etc/os-release")
        installed["/etc/os-release"] = about.read_bytes()
        # Check every installed file and each program's native architecture.
        subprocess.run(["fsck.hfsplus", "-n", str(image)], check=True)
        extracted = work / "root-check"
        extracted.mkdir()
        edit("extractall", "/", str(extracted))
        verify_installed_files(extracted, args.arch, installed)
        print(f"Verified {args.arch} root: four native Mach-O programs, no ELF, exact file inventory")
        image.replace(args.output)


if __name__ == "__main__":
    main()
