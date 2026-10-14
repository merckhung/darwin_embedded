#!/usr/bin/env python3
"""Bazel action: link signed native Mach-O modules and an actual lifecycle probe."""
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


def audit(path, arch, kind):
    data = path.read_bytes()
    magic, cpu, _, actual_kind, _, _, flags, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, actual_kind) != (0xfeedfacf, 0x100000c if arch == "arm64" else 0x1000007, kind):
        raise ValueError("incorrect native module image: " + str(path))
    public, imports, weak = symbols(path)
    return {"sha256": hashlib.sha256(data).hexdigest(), "exports": sorted(public),
            "imports": sorted(imports), "weak_imports": sorted(weak), "flags": flags}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "system", "base-root", "dyld", "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-modules-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk, system = work / "sdk", work / "system"
        for archive_path, destination in ((args.sdk, sdk), (args.system, system)):
            with tarfile.open(archive_path) as archive:
                archive.extractall(destination, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("module guest SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-funwind-tables", "-fPIC", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-isystem", sdk / "usr/include",
                 "-isystem", sdk / "usr/local/include", "-isystem", resource + "/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        objects = {}
        for name in ("test_main", "module", "helper", "broken", "bundle", "slow", "forking", "notifier", "weak_a", "weak_b", "weak_strong"):
            objects[name] = work / (name + ".o")
            run([*flags, "-c", root / "userland/native/modules" / (name + ".c"), "-o", objects[name]])
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                "-undefined", "dynamic_lookup", "-L", system / "usr/lib/system"]
        facade = system / "usr/lib/libSystem.B.dylib"
        weak_libraries=[]
        for name in ("weak_a", "weak_b", "weak_strong"):
            target=work/("lib"+name+".dylib")
            run([*link,"-dylib","-install_name","/usr/lib/native-modules/"+target.name,"-o",target,objects[name],facade])
            weak_libraries.append(target)
        helper, library, broken = [work / name for name in ("libhelper.dylib", "libmodule.dylib", "libbroken.dylib")]
        bundle, executable = work / "native.bundle", work / "native-modules-test"
        slow = work / "libslow.dylib"
        forking = work / "libforking.dylib"
        notifier = work / "libnotifier.dylib"
        run([*link, "-dylib", "-install_name", "@rpath/libhelper.dylib", "-o", helper, objects["helper"], facade])
        run([*link, "-dylib", "-install_name", "@rpath/libmodule.dylib", "-rpath", "@loader_path/helpers",
             "-o", library, objects["module"], helper, facade])
        run([*link, "-dylib", "-install_name", "/usr/lib/native-modules/libbroken.dylib",
             "-o", broken, objects["broken"], facade])
        run([*link, "-bundle", "-o", bundle, objects["bundle"], facade])
        run([*link, "-dylib", "-install_name", "/usr/lib/native-modules/libslow.dylib",
             "-o", slow, objects["slow"], facade])
        run([*link, "-dylib", "-install_name", "/usr/lib/native-modules/libforking.dylib",
             "-o", forking, objects["forking"], facade])
        run([*link, "-dylib", "-install_name", "/usr/lib/native-modules/libnotifier.dylib",
             "-o", notifier, objects["notifier"], facade])
        run([*link, "-pie", "-e", "_main", "-export_dynamic", "-rpath", "@executable_path/../usr/lib/native-modules",
             "-o", executable, objects["test_main"], facade])
        for path in (*weak_libraries, library, helper, broken, bundle, slow, forking, notifier, executable):
            sign(path)
        providers = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        providers |= set().union(*(symbols(path)[0] for path in (system / "usr/lib").rglob("*.dylib")))
        providers |= set().union(*(symbols(path)[0] for path in (*weak_libraries, library, helper, bundle, slow, forking, notifier, executable)))
        for path in (*weak_libraries, library, helper, broken, bundle, slow, forking, notifier, executable):
            missing = symbols(path)[1] - providers
            expected = {"_native_intentionally_missing_import"} if path == broken else set()
            if missing != expected:
                raise ValueError(f"unexpected native module imports in {path.name}: {sorted(missing)}")
        if "_native_absolute_zero" not in symbols(library)[0]:
            raise ValueError("absolute zero export fixture was removed by the linker")
        report = {"architecture": args.arch, "main": audit(executable, args.arch, 2),
                  "module": audit(library, args.arch, 6), "helper": audit(helper, args.arch, 6),
                  "broken": audit(broken, args.arch, 6), "bundle": audit(bundle, args.arch, 8),
                  "slow": audit(slow, args.arch, 6),
                  "forking": audit(forking, args.arch, 6),
                  "notifier": audit(notifier, args.arch, 6),
                  "dyld_sha256": hashlib.sha256(args.dyld.read_bytes()).hexdigest(),
                  "system_tar_sha256": hashlib.sha256(args.system.read_bytes()).hexdigest(),
                  "runtime_checked": False,
                  "validation": "Signed native Mach-O module root; QEMU lifecycle execution is separate."}
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*arguments):
            run([args.hfs_tool.resolve(), image, *arguments])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/usr/lib/dyld": (args.dyld, 7), "/bin/native-modules-test": (executable, 2),
                     "/usr/lib/native-modules/libmodule.dylib": (library, 6),
                     "/usr/lib/native-modules/helpers/libhelper.dylib": (helper, 6),
                     "/usr/lib/native-modules/libbroken.dylib": (broken, 6),
                     "/usr/lib/native-modules/libslow.dylib": (slow, 6),
                     "/usr/lib/native-modules/libforking.dylib": (forking, 6),
                     "/usr/lib/native-modules/libnotifier.dylib": (notifier, 6),
                     "/usr/lib/native-modules/native.bundle": (bundle, 8)}
        for path in weak_libraries:installed["/usr/lib/native-modules/"+path.name]=(path,6)
        report['weak_modules']={path.name:audit(path,args.arch,6) for path in weak_libraries}
        for path in sorted((system / "usr").rglob("*")):
            if path.is_file():
                installed["/" + str(path.relative_to(system))] = (path, 6)
        directories = {str(parent) for name in installed for parent in Path(name).parents if str(parent) != "/"}
        for name in sorted(directories, key=lambda value: len(Path(value).parts)):
            if not (original / name.lstrip("/")).is_dir():
                edit("mkdir", name)
        types = dict(PROGRAM_TYPES)
        for name, (path, kind) in installed.items():
            edit("add", path, name)
            edit("chmod", "755", name)
            expected[name] = path.read_bytes()
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
        print(f"Native modules {args.arch}: signed dylibs, bundle and test root ready")


if __name__ == "__main__":
    main()
