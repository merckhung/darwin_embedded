#!/usr/bin/env python3
"""Link and install compiler TLS consumers against production native LibSystem."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from build_diagnostic_userland import PROGRAM_TYPES, verify_installed_files
from sign_diagnostic_macho import sign


def audit(path, arch, kind, tls=False):
    data = path.read_bytes()
    magic, cpu, _, actual_kind, _, _, flags, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, actual_kind) != (0xfeedfacf, 0x100000c if arch == "arm64" else 0x1000007, kind):
        raise ValueError("incorrect native compiler TLS image: " + str(path))
    if tls and not (flags & 0x800000):
        raise ValueError("compiler TLS image lacks MH_HAS_TLV_DESCRIPTORS")
    public, imports, weak = symbols(path)
    return {"sha256": hashlib.sha256(data).hexdigest(), "exports": sorted(public),
            "imports": sorted(imports), "weak_imports": sorted(weak), "flags": flags}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "system", "base-root", "dyld", "probe", "module", "zero-module", "lifecycle", "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-tls-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk, system = work / "sdk", work / "system"
        for archive_path, destination in ((args.sdk, sdk), (args.system, system)):
            with tarfile.open(archive_path) as archive:
                archive.extractall(destination, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("TLS guest SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-isystem", sdk / "usr/include",
                 "-isystem", sdk / "usr/local/include", "-isystem", resource + "/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        main_object = work / "main.o"
        run([*flags, "-c", root / "userland/native/tls/test_main.c", "-o", main_object])
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                "-undefined", "dynamic_lookup",
                "-L", system / "usr/lib/system"]
        facade = system / "usr/lib/libSystem.B.dylib"
        library = work / "libnative_tls_consumer.dylib"
        run([*link, "-dylib", "-install_name", "/usr/lib/" + library.name,
             "-o", library, args.module.resolve(), facade])
        dynamic = work / "libnative_tls_dynamic.dylib"
        run([*link, "-dylib", "-install_name", "/usr/lib/" + dynamic.name,
             "-o", dynamic, args.module.resolve(), facade])
        zero = work / "libnative_tls_zero.dylib"
        run([*link, "-dylib", "-install_name", "/usr/lib/" + zero.name,
             "-o", zero, args.zero_module.resolve(), facade])
        executable = work / "native-tls-test"
        run([*link, "-pie", "-e", "_main", "-o", executable,
             main_object, args.probe.resolve(), args.lifecycle.resolve(), library, zero, facade])
        for path in (library, dynamic, zero, executable):
            sign(path)
        # Public dyld APIs remain owned by /usr/lib/dyld and use flat imports.
        # Reject every import that the actual installed providers cannot close.
        providers = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        providers |= set().union(*(symbols(path)[0] for path in (system / "usr/lib").rglob("*.dylib")))
        providers |= symbols(library)[0] | symbols(zero)[0]
        for path in (library, dynamic, zero, executable):
            if missing := symbols(path)[1] - providers:
                raise ValueError("TLS guest import has no real provider: " + ", ".join(sorted(missing)))
        report = {"architecture": args.arch, "main": audit(executable, args.arch, 2, True),
                  "consumer_dylib": audit(library, args.arch, 6, True),
                  "dynamic_dylib": audit(dynamic, args.arch, 6, True),
                  "zero_dylib": audit(zero, args.arch, 6, True),
                  "system_tar_sha256": hashlib.sha256(args.system.read_bytes()).hexdigest(),
                  "probe_object_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                  "module_object_sha256": hashlib.sha256(args.module.read_bytes()).hexdigest(),
                  "zero_module_object_sha256": hashlib.sha256(args.zero_module.read_bytes()).hexdigest(),
                  "lifecycle_object_sha256": hashlib.sha256(args.lifecycle.read_bytes()).hexdigest(),
                  "runtime_checked": False,
                  "validation": "Signed Mach-O TLS consumers linked against production LibSystem; QEMU execution is separate."}
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*arguments):
            run([args.hfs_tool.resolve(), image, *arguments])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/usr/lib/dyld": args.dyld, "/bin/native-tls-test": executable,
                     "/usr/lib/" + library.name: library, "/usr/lib/" + dynamic.name: dynamic,
                     "/usr/lib/" + zero.name: zero}
        for path in sorted((system / "usr").rglob("*")):
            if path.is_file():
                installed["/" + str(path.relative_to(system))] = path
        directories = {str(Path(name).parent) for name in installed}
        for name in sorted(directories, key=lambda value: len(Path(value).parts)):
            if not (original / name.lstrip("/")).is_dir():
                edit("mkdir", name)
        types = dict(PROGRAM_TYPES)
        for name, path in installed.items():
            edit("add", path, name)
            edit("chmod", "755", name)
            expected[name] = path.read_bytes()
            kind = 7 if name == "/usr/lib/dyld" else 2 if name.startswith("/bin/") else 6
            audit(path, args.arch, kind)
            types[name] = kind
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        verify_installed_files(extracted, args.arch, expected, types)
        run(["fsck.hfsplus", "-n", image])
        report["root_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
        args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
        image.replace(args.output)
        print(f"Native compiler TLS {args.arch}: signed system, consumer DLL and test root ready")


if __name__ == "__main__":
    main()
