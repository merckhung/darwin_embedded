#!/usr/bin/env python3
"""Compile a real graphics consumer or install its signed native init root."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile

from build_libraries import digest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from build_diagnostic_userland import verify_installed_files
from sign_diagnostic_macho import sign


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "libraries", "output", "report", "exports", "probe", "runtime",
                 "system", "base-root", "dyld", "hfs-tool"):
        parser.add_argument("--" + name, type=Path, required=name in ("root", "sdk", "output", "report"))
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-graphics-test-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("graphics consumer SDK architecture mismatch")
        if args.exports:
            libraries = work / "libraries"
            with tarfile.open(args.libraries) as archive:
                archive.extractall(libraries, filter="data")
            source = root / "userland/apps/graphics/test_graphics.c"
            resource = run(["clang-20", "-print-resource-dir"]).strip()
            flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                     "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc",
                     "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-Wall", "-Wextra", "-Werror", "-Wno-availability",
                     "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                     "-I", libraries / "usr/include", "-I", libraries / "usr/include/freetype2",
                     "-I", libraries / "usr/include/pixman-1"]
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
            run([*flags, "-c", source, "-o", args.output])
            magic, cpu, _, kind = struct.unpack_from("<4I", args.output.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("graphics consumer is not the native Mach-O object")
            imports = symbols(args.output)[1]
            definitions = set().union(*(symbols(path)[0] for path in (libraries / "usr/lib").glob("*.a")))
            requests = imports - definitions
            args.exports.write_text("\n".join(sorted(requests)) + "\n")
            args.report.write_text(json.dumps({"architecture": args.arch, "imports": sorted(imports),
                "native_system_imports": sorted(requests), "source_sha256": digest(source),
                "object_sha256": digest(args.output), "libraries_sha256": digest(args.libraries)}, indent=2) + "\n")
            return
        tree = work / "runtime"
        # Replace the build-only selected runtime with the actual union needed
        # by PID 1, dash and this consumer. Their install names/ABI agree.
        for bundle in (args.runtime, args.system):
            with tarfile.open(bundle) as archive:
                archive.extractall(tree, filter="data")
        executable = work / "native-graphics-test"
        libraries = [path for path in sorted((tree / "usr/lib").glob("*.dylib")) if path.name != "libSystem.B.dylib"]
        run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
             "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
             "-undefined", "dynamic_lookup",
             "-o", executable, args.probe.resolve(), *libraries, "-L", tree / "usr/lib/system",
             tree / "usr/lib/libSystem.B.dylib"])
        sign(executable)
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)

        def edit(*arguments):
            return run([args.hfs_tool.resolve(), image, *arguments])

        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        fixture = root / "src/Applications/graphics/freetype/2.15.0/tests/data/varc-static-gvar.ttf"
        installed = {"/usr/lib/dyld": args.dyld, "/bin/native-graphics-test": executable,
                     "/usr/share/fonts/native-varc-static-gvar.ttf": fixture}
        installed.update({"/" + str(path.relative_to(tree)): path for path in sorted((tree / "usr").rglob("*")) if path.is_file()})
        directories = set()
        for name in installed:
            directories.update(str(parent) for parent in Path(name).parents if parent != Path("/"))
        for name in sorted(directories, key=lambda name: (len(Path(name).parts), name)):
            if not (original / name.lstrip("/")).is_dir():
                edit("mkdir", name)
        for name, source in installed.items():
            edit("add", source, name)
            edit("chmod", "644" if name.endswith(".ttf") else "755", name)
            expected[name] = source.read_bytes()
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        types, images = {}, {}
        for name, data in expected.items():
            if name.startswith(("/bin/", "/sbin/")) or name == "/usr/lib/dyld" or name.endswith(".dylib"):
                types[name] = 7 if name == "/usr/lib/dyld" else 6 if name.endswith(".dylib") else 2
                images[name] = image_metadata(data)
        verify_installed_files(extracted, args.arch, expected, types)
        loader_exports = images["/usr/lib/dyld"]["exports"] | {"dyld_stub_binder"}
        bindings = {name: verify_imports(name, images, loader_exports, flat_namespace_root="/bin/native-graphics-test")
                    for name, kind in types.items() if kind != 7}
        run(["fsck.hfsplus", "-n", image])
        args.report.write_text(json.dumps({"architecture": args.arch, "root_sha256": digest(image),
            "base_init_root_sha256": digest(args.base_root), "executable_sha256": digest(executable),
            "probe_sha256": digest(args.probe), "runtime_sha256": digest(args.runtime), "system_sha256": digest(args.system),
            "dyld_sha256": digest(args.dyld), "font_fixture": {"source": str(fixture.relative_to(root)), "sha256": digest(fixture)},
            "installed_sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(expected.items())},
            "import_resolutions": bindings, "runtime_checked": False,
            "validation": "Actual native init/dash root; every installed byte and Mach-O CPU/type audited; per-image strong ordinal closure; no ELF; fsck read-only PASS."}, indent=2) + "\n")
        image.replace(args.output)
        print(f"Native graphics {args.arch}: complete signed consumer under actual init/dash, HFS inventory and bindings audited")


if __name__ == "__main__":
    main()
