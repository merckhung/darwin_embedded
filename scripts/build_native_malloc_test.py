#!/usr/bin/env python3
"""Link real Darwin allocator components and install a signed guest test root."""
import argparse
import json
import re
from pathlib import Path
import shutil
import tarfile
import tempfile

from build_diagnostic_userland import PROGRAM_TYPES, verify_installed_files
from sign_diagnostic_macho import sign


from native_components import build_helpers, run, symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "base-root", "dyld", "libsyscall",
                 "libmach", "libplatform", "libpthread", "libpthread-dyld", "libc", "libmalloc", "libmath", "probe",
                 "libtlv", "tlv-probe", "librng", "rng-probe", "metadata-probe",
                 "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "userland/native/malloc"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-malloc-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        for path in (args.sdk, args.mach_headers):
            with tarfile.open(path) as archive:
                archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise SystemExit("allocator guest SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-DPRIVATE=1", "-isystem", str(sdk / "usr/include"),
                 "-isystem", str(sdk / "usr/local/include"), "-isystem", resource + "/include",
                 "-isystem", str(root / "tools/cctools/include"),
                 "-I", str(root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers")]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        libc_source = root / "src/Libraries/25.5.0/Libc"
        flags += ["-I", str(libc_source / "darwin"), "-I", str(work)]
        program_vars = re.search(r"struct ProgramVars\s*\{.*?\};",
                                 (libc_source / "sys/crt_externs.c").read_text(), re.DOTALL)
        if not program_vars:
            raise RuntimeError("published Libc ProgramVars declaration missing")
        (work / "native_programvars.h").write_text(program_vars.group() + "\n")
        objects = {}
        for name in ("bootstrap.c", "test_main.c", "test_libc.c"):
            objects[name] = work / (name + ".o")
            run([*flags, "-c", source / name, "-o", objects[name]])
        helpers, provenance = build_helpers(root, work, sdk, flags, args.arch, crypto_probe=True)
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign"]
        archives = {
            "kernel": [args.libmach.resolve(), args.libsyscall.resolve()],
            "platform": [args.libplatform.resolve()],
            "pthread": [args.libpthread.resolve()],
            "libc": [args.libc.resolve()],
            "malloc": [args.libmalloc.resolve()],
            "math": [args.libmath.resolve()],
            "blocks": [], "crypto": [], "tlv": [args.libtlv.resolve()],
            "rng": [args.librng.resolve()],
            "bootstrap": [args.libpthread_dyld.resolve(), args.libsyscall.resolve()],
        }
        extra = {"kernel": [], "platform": [], "pthread": [], "libc": [objects["test_libc.c"]], "malloc": [args.probe.resolve(), args.metadata_probe.resolve()], "math": [],
                 "bootstrap": [objects["bootstrap.c"]], "blocks": [], "crypto": [],
                 "tlv": [args.tlv_probe.resolve()], "rng": [args.rng_probe.resolve()]}
        for name, paths in helpers.items():
            extra[name] += paths
        selected = {
            "kernel": {"___libkernel_init", "___write_nocancel", "___sysctl"},
            "platform": {"_memcpy", "___libplatform_init"},
            "pthread": {"___pthread_init", "__pthread_exit_if_canceled",
                        "__pthread_clear_qos_tsd", "_pthread_current_stack_contains_np"},
            "libc": {"__libc_initializer"},
            "malloc": {"_native_malloc_check", "___malloc_init"},
            "math": set(), "blocks": set(), "crypto": set(), "tlv": set(), "rng": set(),
            "bootstrap": {"_native_malloc_bootstrap"},
        }
        providers = {}
        # Kernel's private memory/callback forwarders must never become public
        # providers for another component's imports.
        for name in ("platform", "pthread", "malloc", "libc", "math", "kernel", "blocks", "crypto", "tlv", "rng"):
            for archive in [*archives[name], *extra[name]]:
                public, _, _ = symbols(archive)
                for symbol in public:
                    providers.setdefault(symbol, name)
        libraries = {name: work / f"libnative_malloc_{name}.dylib" for name in archives}
        imports = {}
        for iteration in range(24):
            for name, output in libraries.items():
                exports = work / (name + ".exports")
                exports.write_text("\n".join(sorted(selected[name])) + "\n")
                roots = [option for symbol in sorted(selected[name]) for option in ("-u", symbol)]
                run([*link, "-dylib", "-undefined", "dynamic_lookup", "-install_name",
                     "/usr/lib/" + output.name, "-exported_symbols_list", exports, *roots,
                     "-o", output, *extra[name], *archives[name]])
            imports = {name: symbols(path)[1] for name, path in libraries.items()}
            # ld64 defines this real image-header symbol when linking MH_EXECUTE.
            imports["main"] = symbols(objects["test_main.c"])[1] - {"__mh_execute_header"}
            available = set().union(*(symbols(path)[0] for path in libraries.values()))
            available |= symbols(args.dyld)[0] | {"dyld_stub_binder"}
            missing = set().union(*imports.values()) - available
            if not missing:
                break
            unavailable = missing - providers.keys()
            added = False
            for symbol in missing - unavailable:
                provider = providers[symbol]
                if symbol not in selected[provider]:
                    selected[provider].add(symbol)
                    added = True
            if added:
                continue
            if unavailable:
                details = [line for name in archives for path in [*archives[name], *extra[name]]
                           for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines()
                           if any(symbol in line for symbol in unavailable)]
                raise RuntimeError("native allocator components need genuine implementations: " +
                                   ", ".join(sorted(unavailable)) + "\n" + "\n".join(details))
            raise RuntimeError("selected native exports are hidden or unavailable: " +
                               ", ".join(sorted(missing)))
        else:
            raise RuntimeError("native allocator component dependency closure did not converge")
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
            "helper_sources": provenance,
            "helper_configuration": {
                "platform_init": "published dynamic initializer; unused AppleFeatures include removed",
                "sha256": "published portable C implementation; assembly and unavailable CPU DIT disabled",
                "mach_errors": "complete source tables; exact published USB/FireWire numeric declarations extracted",
                "sleep": "published cancellable sleep/nanosleep variants",
                "timezone": "published NOTIFY_TZ-disabled feature branch; no notify daemon in test root",
            },
            "main_imports": sorted(symbols(executable)[1]),
            "libraries": {name: {"exports": sorted(symbols(path)[0]),
                                  "imports": sorted(symbols(path)[1]),
                                  "weak_imports": sorted(symbols(path)[2])}
                          for name, path in libraries.items()},
            "runtime_checked": False,
            "allocator_callbacks": "actual source-built malloc/free/realloc/type APIs and native exit",
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
        print(f"native malloc {args.arch}: closed real component imports, signed guest root ready")


if __name__ == "__main__":
    main()
