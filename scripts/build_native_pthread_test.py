#!/usr/bin/env python3
"""Link real Darwin pthread components and install a signed guest test root."""
import argparse
import json
import re
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from build_diagnostic_userland import PROGRAM_TYPES, verify_installed_files
from sign_diagnostic_macho import sign


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def symbols(path):
    public, undefined, weak = set(), set(), set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        if "external " not in line:
            continue
        match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
        if not match:
            raise RuntimeError("unrecognized native symbol listing: " + line)
        name = match.group(1)
        if "(undefined)" in line:
            (weak if "weak external" in line else undefined).add(name)
        elif "private external" not in line:
            public.add(name)
    return public, undefined, weak


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "base-root", "dyld", "libsyscall",
                 "libmach", "libplatform", "libpthread", "libpthread-dyld", "probe",
                 "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "userland/native/pthread"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-thread-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        for path in (args.sdk, args.mach_headers):
            with tarfile.open(path) as archive:
                archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise SystemExit("pthread guest SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-DPRIVATE=1", "-isystem", str(sdk / "usr/include"),
                 "-isystem", str(sdk / "usr/local/include"), "-isystem", resource + "/include",
                 "-isystem", str(root / "tools/cctools/include"),
                 "-I", str(root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers")]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        objects = {}
        for name in ("bootstrap.c", "test_main.c"):
            objects[name] = work / (name + ".o")
            run([*flags, "-c", source / name, "-o", objects[name]])
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign"]
        archives = {
            "kernel": [args.libmach.resolve(), args.libsyscall.resolve()],
            "platform": [args.libplatform.resolve()],
            "pthread": [args.libpthread.resolve()],
            "bootstrap": [args.libpthread_dyld.resolve(), args.libsyscall.resolve()],
        }
        extra = {"kernel": [], "platform": [], "pthread": [args.probe.resolve()],
                 "bootstrap": [objects["bootstrap.c"]]}
        selected = {
            "kernel": {"___libkernel_init", "___write_nocancel", "___sysctl"},
            "platform": {"_memcpy"},
            "pthread": {"_native_pthread_probe", "___pthread_init", "__pthread_exit_if_canceled",
                        "__pthread_clear_qos_tsd", "_pthread_current_stack_contains_np"},
            "bootstrap": {"_native_pthread_bootstrap"},
        }
        providers = {}
        # Kernel's private memory/callback forwarders must never become public
        # providers for another component's imports.
        for name in ("platform", "pthread", "kernel"):
            for archive in archives[name]:
                public, _, _ = symbols(archive)
                for symbol in public:
                    providers.setdefault(symbol, name)
        libraries = {name: work / f"libnative_thread_{name}.dylib" for name in archives}
        imports = {}
        for iteration in range(16):
            for name, output in libraries.items():
                exports = work / (name + ".exports")
                exports.write_text("\n".join(sorted(selected[name])) + "\n")
                roots = [option for symbol in sorted(selected[name]) for option in ("-u", symbol)]
                run([*link, "-dylib", "-undefined", "dynamic_lookup", "-install_name",
                     "/usr/lib/" + output.name, "-exported_symbols_list", exports, *roots,
                     "-o", output, *extra[name], *archives[name]])
            imports = {name: symbols(path)[1] for name, path in libraries.items()}
            imports["main"] = symbols(objects["test_main.c"])[1]
            available = set().union(*(symbols(path)[0] for path in libraries.values()))
            available |= symbols(args.dyld)[0] | {"dyld_stub_binder"}
            missing = set().union(*imports.values()) - available
            if not missing:
                break
            unavailable = missing - providers.keys()
            if unavailable:
                details = [line for paths in archives.values() for path in paths
                           for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines()
                           if any(symbol in line for symbol in unavailable)]
                raise RuntimeError("native thread components need genuine implementations: " +
                                   ", ".join(sorted(unavailable)) + "\n" + "\n".join(details))
            added = False
            for symbol in missing:
                provider = providers[symbol]
                if symbol not in selected[provider]:
                    selected[provider].add(symbol)
                    added = True
            if not added:
                raise RuntimeError("selected native exports are hidden or unavailable: " +
                                   ", ".join(sorted(missing)))
        else:
            raise RuntimeError("native thread component dependency closure did not converge")
        executable = work / "native-loader-test"
        run([*link, "-pie", "-e", "_main", "-undefined", "dynamic_lookup",
             "-o", executable, objects["test_main.c"],
             *libraries.values()])
        if missing := symbols(executable)[1] - available:
            raise RuntimeError("unresolved final native executable imports: " + ", ".join(sorted(missing)))
        for path in (*libraries.values(), executable):
            sign(path)
        report = {
            "architecture": args.arch,
            "main_imports": sorted(symbols(executable)[1]),
            "libraries": {name: {"exports": sorted(symbols(path)[0]),
                                  "imports": sorted(symbols(path)[1]),
                                  "weak_imports": sorted(symbols(path)[2])}
                          for name, path in libraries.items()},
            "runtime_checked": False,
            "allocator_callbacks": "unavailable; upstream nullable function table entries",
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
        installed = {"/usr/lib/dyld": args.dyld, "/bin/native-loader-test": executable}
        installed.update({"/usr/lib/" + path.name: path for path in libraries.values()})
        for name, path in installed.items():
            edit("add", path, name)
            edit("chmod", "755", name)
            expected[name] = path.read_bytes()
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        types = dict(PROGRAM_TYPES, **{"/bin/native-loader-test": 2})
        types.update({"/usr/lib/" + path.name: 6 for path in libraries.values()})
        verify_installed_files(extracted, args.arch, expected, types)
        run(["fsck.hfsplus", "-n", image])
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
        image.replace(args.output)
        print(f"native pthread {args.arch}: closed real component imports, signed guest root ready")


if __name__ == "__main__":
    main()
