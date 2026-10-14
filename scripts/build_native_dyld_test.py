#!/usr/bin/env python3
"""Build a Mach-O dependency/constructor/errno test root for native QEMU checks."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import tarfile

from build_diagnostic_userland import PROGRAM_TYPES, verify_installed_files
from sign_diagnostic_macho import sign


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--base-root", type=Path, required=True)
    parser.add_argument("--dyld", type=Path, required=True)
    parser.add_argument("--libsyscall", type=Path, required=True)
    parser.add_argument("--hfs-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--mach-headers", type=Path)
    parser.add_argument("--libplatform", type=Path)
    parser.add_argument("--libmach", type=Path)
    args = parser.parse_args()
    runtime = (args.sdk, args.mach_headers, args.libplatform, args.libmach)
    if any(runtime) and not all(runtime):
        parser.error("runtime tests require --sdk, --mach-headers, --libplatform and --libmach together")
    source = args.root.resolve() / "userland/native/dyld"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix=".native-dyld-test-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-nostdinc", "-O2", "-isystem", resource + "/include",
                 "-I", str(source)]
        if args.arch == "arm64": flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        if all(runtime):
            for archive_path in (args.sdk, args.mach_headers):
                with tarfile.open(archive_path) as archive:
                    archive.extractall(work / "sdk", filter="data")
            flags += ["-DNATIVE_RUNTIME_TEST=1", "-Wno-availability",
                      "-D_FORTIFY_SOURCE=0",
                      "-isystem", str(work / "sdk/usr/include"),
                      "-isystem", str(work / "sdk/usr/local/include")]
        objects = {}
        for name in ("test_leaf.c", "test_chain.c", "test_main.c", "memory.c"):
            output = work / (name + ".o")
            subprocess.run([*flags, "-c", str(source / name), "-o", str(output)], check=True)
            objects[name] = str(output)
        runtime_objects = {}
        if all(runtime):
            for component in ("platform", "mach"):
                output = work / (component + ".o")
                source_file = args.root.resolve() / f"userland/native/{component}/test_{component}.c"
                subprocess.run([*flags, "-c", str(source_file), "-o", str(output)], check=True)
                runtime_objects[component] = str(output)
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
                "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign"]
        runtime_libraries = {}
        for component, obj in runtime_objects.items():
            output = work / ("libnative_" + component + ".dylib")
            guest_path = "/usr/lib/" + output.name
            export = "_native_mach_probe" if component == "mach" else "_native_platform_check"
            archive = args.libplatform if component == "platform" else args.libmach
            subprocess.run([*link, "-dylib", "-install_name", guest_path, "-exported_symbol", export,
                            "-o", str(output), obj, str(archive.resolve()),
                            str(args.libsyscall.resolve())], check=True)
            sign(output)
            runtime_libraries[guest_path] = output
        leaf, chain, main = work / "libnative_leaf.dylib", work / "libnative_chain.dylib", work / "native-loader-test"
        subprocess.run([*link, "-dylib", "-install_name", "/usr/lib/libnative_leaf.dylib",
                        "-exported_symbol", "_native_double", "-o", str(leaf), objects["test_leaf.c"]], check=True)
        subprocess.run([*link, "-dylib", "-install_name", "/usr/lib/libnative_chain.dylib",
                        "-exported_symbol", "_native_value", "-exported_symbol", "_native_environment",
                        "-o", str(chain), objects["test_chain.c"], str(leaf), str(args.libsyscall.resolve())], check=True)
        subprocess.run([*link, "-pie", "-e", "_main", "-undefined", "dynamic_lookup", "-o", str(main),
                        objects["test_main.c"], objects["memory.c"], str(chain),
                        *map(str, runtime_libraries.values()),
                        str(args.libsyscall.resolve())], check=True)
        for binary in (leaf, chain, main): sign(binary)
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*command):
            subprocess.run([str(args.hfs_tool.resolve()), str(image), *command], check=True)
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", str(original))
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/usr/lib/dyld": args.dyld, "/usr/lib/libnative_leaf.dylib": leaf,
                     "/usr/lib/libnative_chain.dylib": chain, "/bin/native-loader-test": main}
        installed.update(runtime_libraries)
        for name, binary in installed.items():
            edit("add", str(binary), name)
            edit("chmod", "755", name)
            expected[name] = binary.read_bytes()
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", str(extracted))
        program_types = dict(PROGRAM_TYPES, **{"/usr/lib/libnative_leaf.dylib": 6,
                                              "/usr/lib/libnative_chain.dylib": 6,
                                              "/bin/native-loader-test": 2})
        program_types.update({name: 6 for name in runtime_libraries})
        verify_installed_files(extracted, args.arch, expected, program_types)
        subprocess.run(["fsck.hfsplus", "-n", str(image)], check=True)
        image.replace(args.output)


if __name__ == "__main__":
    main()
