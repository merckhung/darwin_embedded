#!/usr/bin/env python3
"""Build the signed native Mach-O dynamic linker against Apple's libsyscall."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

from sign_diagnostic_macho import sign

LOADER_APIS = (
    "_dyld_find_unwind_sections", "_dyld_register_func_for_remove_image",
    "_dyld_register_for_bulk_image_loads", "getsectiondata", "_dyld_get_image_header",
    "_dyld_get_image_slide", "dyld_get_active_platform", "dyld_get_program_sdk_version_token",
    "dyld_get_program_min_os_version",
    "dyld_program_sdk_at_least", "dyld_sdk_at_least", "NSVersionOfLinkTimeLibrary",
    "_dyld_is_memory_immutable", "dyld_process_is_restricted", "_dyld_get_image_uuid",
    "_dyld_images_for_addresses", "_dyld_stack_range", "_dyld_get_shared_cache_range",
    "_dyld_get_shared_cache_uuid", "dyld_image_header_containing_address", "dladdr",
    "dlopen", "dlsym", "dlclose", "dlerror", "_dyld_dlopen_atfork_prepare",
    "_dyld_dlopen_atfork_parent", "_dyld_dlopen_atfork_child",
    "NXArgc", "NXArgv", "environ", "__progname",
)


def generate_metadata_headers(root, destination):
    """Use Apple's generator, then retain its exact macOS version-set column.

    This loader accepts macOS images. Other platform identities must never be
    silently reinterpreted as macOS availability.
    """
    root, destination = Path(root), Path(destination)
    availability = root / "src/Libraries/25.5.0/AvailabilityVersions"
    generated = destination / "VersionMap.h"
    subprocess.run(["python3", str(availability / "availability"), "--av_version", "157.2",
                    "--preprocess", str(availability / "templates/VersionMap.h"), str(generated)], check=True)
    text = generated.read_text()
    rows = []
    for row in re.findall(r"\{\s*(\.set\s*=[^{}]+)\}", text):
        fields = dict(re.findall(r"\.(\w+)\s*=\s*(0x[0-9a-fA-F]+)", row))
        if not {"set", "macos"} <= fields.keys():
            raise ValueError("Apple version map lacks the required macOS column")
        rows.append((int(fields["set"], 16), int(fields["macos"], 16)))
    expected = re.search(r"std::array<VersionSetEntry,\s*(\d+)>", text)
    if not expected or len(rows) != int(expected.group(1)) or not rows:
        raise ValueError("Apple version map row count does not match its declaration")
    if any(a[0] >= b[0] or a[1] > b[1] for a, b in zip(rows, rows[1:])):
        raise ValueError("Apple macOS version map is not ordered")
    result = ["/* Generated from Apple's AvailabilityVersions/VersionMap.h. */",
              "struct native_version_set { uint32_t set, macos; };",
              "static const struct native_version_set native_version_sets[] = {"]
    result += [f"    {{ 0x{version_set:08x}, 0x{macos:08x} }}," for version_set, macos in rows]
    result += ["};", ""]
    (destination / "native_version_map.h").write_text("\n".join(result))
    # Internal constants come directly from matching XNU headers. No generated
    # SDK declarations or substitute security implementation is introduced.
    constants = []
    for relative, names in (
        ("bsd/sys/codesign.h", ("CS_OPS_STATUS",)),
        ("osfmk/kern/cs_blobs.h", ("CS_RESTRICT", "CS_REQUIRE_LV", "CS_RUNTIME")),
    ):
        source = (root / "src/Kernel/xnu/12377.121.6" / relative).read_text()
        for name in names:
            found = re.search(r"^#define\s+" + name + r"\s+(0x[0-9a-fA-F]+|[0-9]+)\b", source, re.M)
            if not found:
                raise ValueError("missing native code-signing definition: " + name)
            constants.append(f"#define NATIVE_{name} {found.group(1)}")
    (destination / "native_codesign_constants.h").write_text(
        "/* Exact numeric definitions from matching XNU codesign/cs_blobs headers. */\n" +
        "\n".join(constants) + "\n")
    fcntl = (root / "src/Kernel/xnu/12377.121.6/bsd/sys/fcntl.h").read_text()
    dlfcn = (root / "src/Libraries/25.5.0/dyld/include/dlfcn.h").read_text()
    with (destination / "native_codesign_constants.h").open("a") as output:
        for source, names in ((fcntl, ("F_GETPATH",)),
                              (dlfcn, ("RTLD_LAZY", "RTLD_NOW", "RTLD_LOCAL", "RTLD_GLOBAL",
                                       "RTLD_NOLOAD", "RTLD_NODELETE", "RTLD_FIRST"))):
            for name in names:
                found = re.search(r"^#define\s+" + name + r"\s+(0x[0-9a-fA-F]+|[0-9]+)\b", source, re.M)
                if not found:
                    raise ValueError("missing native loader definition: " + name)
                output.write(f"#define NATIVE_{name} {found.group(1)}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--libsyscall", type=Path, required=True)
    parser.add_argument("--ld", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.root.resolve() / "userland/native/dyld"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".native-dyld-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        generate_metadata_headers(args.root.resolve(), work)
        with tarfile.open(args.sdk) as archive:
            archive.extractall(work / "sdk", filter="data")
        availability = args.root.resolve() / "src/Libraries/25.5.0/AvailabilityVersions"
        tool = work / "sdk/usr/local/libexec/availability.pl"
        tool.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["python3", str(availability / "availability"), "--av_version", "157.2",
                        "--preprocess", str(availability / "availability"), str(tool)], check=True)
        tool.chmod(0o755)
        subprocess.run(["bash", str(args.root.resolve() / "src/Kernel/xnu/12377.121.6/bsd/sys/make_symbol_aliasing.sh"),
                        str(work / "sdk"), str(work / "sdk/usr/include/sys/_symbol_aliasing.h")],
                       env=dict(os.environ, DRIVERKITROOT=""), check=True)
        subprocess.run(["bash", str(args.root.resolve() / "src/Kernel/xnu/12377.121.6/bsd/sys/make_posix_availability.sh"),
                        str(work / "sdk/usr/include/sys/_posix_availability.h")], check=True)
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Wall", "-Werror",
                 "-I", str(work), "-isystem", resource + "/include", "-isystem", str(work / "sdk/usr/include")]
        flags += ["-I", str(args.root.resolve() / "src/Kernel/xnu/12377.121.6/libsyscall")]
        libraries = args.root.resolve() / "src/Libraries/25.5.0"
        shutil.copyfile(libraries / "libplatform/include/setjmp.h", work / "setjmp.h")
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        objects = []
        for name in ("loader.c", "start.S", "memory.c"):
            output = work / (name + ".o")
            subprocess.run([*flags, "-c", str(source / name), "-o", str(output)], check=True)
            objects.append(str(output))
        # The genuine non-signal recovery routines keep failed dlopen operations
        # recoverable. ARM's file also defines signal-saving entry points; select
        # its complete initial _setjmp/_longjmp section without those unrelated
        # routines. All assembly bodies, pointer munging and ABI offsets remain
        # exactly the pinned libplatform source.
        jump_source = libraries / "libplatform/src/setjmp" / args.arch / (
            "setjmp.s" if args.arch == "arm64" else "_setjmp.s")
        jump_text = jump_source.read_text()
        if args.arch == "arm64":
            marker = "\n.macro FUNC_PROLOGUE\n"
            if jump_text.count(marker) != 1:
                raise ValueError("Apple non-signal setjmp source boundary changed")
            jump_text = jump_text.split(marker)[0]
        jump = work / "native_setjmp.S"
        jump.write_text(jump_text)
        architecture = work / "architecture"
        shutil.copytree(libraries / "architecture", architecture)
        output = work / "native_setjmp.o"
        subprocess.run([*flags, "-I", str(args.root.resolve() / "src/Kernel/xnu/12377.121.6/libsyscall"),
                        "-c", str(jump), "-o", str(output)], check=True)
        objects.append(str(output))
        output = work / "dyld"
        subprocess.run([str(args.ld.resolve()), "-arch", args.arch, "-dylinker", "-dead_strip",
                        "-exported_symbol", "_native_dyld_start",
                        *[option for name in LOADER_APIS for option in ("-exported_symbol", "_" + name)],
                        "-e", "_native_dyld_start", "-platform_version", "macos", "26.5", "26.5",
                        "-adhoc_codesign", "-o", str(output), *objects, str(args.libsyscall.resolve())], check=True)
        sign(output)
        output.replace(args.output)


if __name__ == "__main__":
    main()
