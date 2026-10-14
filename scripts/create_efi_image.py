#!/usr/bin/env python3
"""Create a GPT/FAT ESP containing the current EFI loader and kernel collection."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(*command):
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, required=True)
    parser.add_argument("--kernel-collection", type=Path, required=True)
    parser.add_argument("--root-image", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for source in (args.loader, args.kernel_collection):
        if not source.is_file():
            parser.error(f"missing input: {source}")
    if args.root_image and (not args.root_image.is_file() or args.root_image.stat().st_size not in (64 * 1024 * 1024, 128 * 1024 * 1024)):
        parser.error("native HFS root image must be 64 or 128 MiB")
    payload = sum(source.stat().st_size for source in (args.loader, args.kernel_collection))
    if args.root_image:
        payload += args.root_image.stat().st_size
    # Leave space for FAT metadata and boot configuration, rounding the ESP
    # up to 32 MiB units while retaining the existing small-image geometry.
    partition_mib = max(96, ((payload + 8 * 1024 * 1024 + 32 * 1024 * 1024 - 1) // (32 * 1024 * 1024)) * 32)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".efi-image-", dir=args.output.parent) as directory:
        work = Path(directory)
        image, partition = work / "boot.img", work / "esp.img"
        with image.open("wb") as stream:
            stream.truncate((partition_mib + 32) * 1024 * 1024)
        run("sgdisk", "--clear", f"--new=1:2048:+{partition_mib}M", "--typecode=1:EF00", str(image))
        with partition.open("wb") as stream:
            stream.truncate(partition_mib * 1024 * 1024)
        run("mformat", "-i", str(partition), "-F", "-v", "DARWIN", "::")
        run("mmd", "-i", str(partition), "::EFI", "::EFI/BOOT")
        for source, destination in ((args.loader, "::EFI/BOOT/BOOTX64.EFI"),
                                    (args.kernel_collection, "::EFI/BOOT/kernel")):
            run("mcopy", "-i", str(partition), str(source), destination)
        if args.root_image:
            run("mcopy", "-i", str(partition), str(args.root_image), "::ramdisk.img")
        boot_args = work / "boot-args.txt"
        boot_args.write_text("-v -s serial=3 debug=0x14c keepsyms=1 startup_debug=1\n")
        run("mcopy", "-i", str(partition), str(boot_args), "::EFI/BOOT/boot-args.txt")
        with image.open("r+b") as target, partition.open("rb") as source:
            target.seek(1024 * 1024)
            while block := source.read(1024 * 1024):
                target.write(block)
        image.replace(args.output)


if __name__ == "__main__":
    main()
