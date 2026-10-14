#!/usr/bin/env python3
"""Link and install the actual Mach-O dash shell and native account checks."""
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
    magic, cpu, _, actual_kind = struct.unpack_from("<4I", data)
    if (magic, cpu, actual_kind) != (0xfeedfacf, 0x100000c if arch == "arm64" else 0x1000007, kind):
        raise ValueError("incorrect native shell image: " + str(path))
    public, imports, weak = symbols(path)
    return {"sha256": hashlib.sha256(data).hexdigest(), "exports": sorted(public),
            "imports": sorted(imports), "weak_imports": sorted(weak)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "system", "base-root", "dyld", "dash", "probe", "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-shell-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        system = work / "system"
        with tarfile.open(args.system) as archive:
            archive.extractall(system, filter="data")
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                "-undefined", "dynamic_lookup", "-L", system / "usr/lib/system"]
        facade = system / "usr/lib/libSystem.B.dylib"
        dash = work / "dash"
        run([*link, "-pie", "-e", "_main", "-o", dash, args.dash.resolve(), facade])
        executable = work / "native-shell-test"
        run([*link, "-pie", "-e", "_main", "-o", executable, args.probe.resolve(), facade])
        for path in (dash, executable):
            sign(path)
        providers = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        providers |= set().union(*(symbols(path)[0] for path in (system / "usr/lib").rglob("*.dylib")))
        for path in (dash, executable):
            if missing := symbols(path)[1] - providers:
                raise ValueError("shell import has no actual provider: " + ", ".join(sorted(missing)))
        report = {"architecture": args.arch, "dash": audit(dash, args.arch, 2),
                  "account_consumer": audit(executable, args.arch, 2),
                  "system_tar_sha256": hashlib.sha256(args.system.read_bytes()).hexdigest(),
                  "dash_archive_sha256": hashlib.sha256(args.dash.read_bytes()).hexdigest(),
                  "consumer_object_sha256": hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                  "runtime_checked": False,
                  "validation": "Signed Mach-O dash and account consumer with real native system providers; QEMU execution is separate."}
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*arguments):
            run([args.hfs_tool.resolve(), image, *arguments])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/usr/lib/dyld": args.dyld, "/bin/dash": dash,
                     "/bin/native-shell-test": executable,
                     "/etc/native-shell-test.sh": root / "userland/apps/dash/test.sh"}
        for path in sorted((system / "usr").rglob("*")):
            if path.is_file():
                installed["/" + str(path.relative_to(system))] = path
        # Include malformed records ahead of real accounts to exercise parsing.
        master = work / "master.passwd"
        master.write_text("# Native Darwin file-backed account records\n"
                          "root:*:4294967296:0::0:0:Invalid UID:/wrong:/bin/dash\n"
                          "overflow:*:18446744073709551616:20::0:0:Invalid UID:/wrong:/bin/dash\n"
                          "badtime:*:502:20::9223372036854775808:0:Invalid time:/wrong:/bin/dash\n"
                          "root:*:0:0::0:0:System Administrator:/var/root:/bin/dash\n"
                          "desktop:*:501:20:staff:-1:0:Desktop User:/home/desktop:/bin/dash\n"
                          "longrecord:*:503:20::0:0:" + "L" * 600 + ":/home/desktop:/bin/dash\n")
        passwd = work / "passwd"
        passwd.write_text("# Native Darwin public account records\n"
                          "root:*:0:0:System Administrator:/var/root:/bin/dash\n"
                          "desktop:*:501:20:Desktop User:/home/desktop:/bin/dash\n")
        installed.update({"/etc/master.passwd": master, "/etc/passwd": passwd})
        directories = {str(Path(name).parent) for name in installed}
        directories |= {"/var", "/var/root", "/home", "/home/desktop"}
        for name in sorted(directories, key=lambda value: len(Path(value).parts)):
            if not (original / name.lstrip("/")).is_dir():
                edit("mkdir", name)
        types = dict(PROGRAM_TYPES)
        for name, path in installed.items():
            edit("add", path, name)
            kind = 7 if name == "/usr/lib/dyld" else 2 if name.startswith("/bin/") else 6 if name.endswith(".dylib") else None
            edit("chmod", "755" if kind else "600" if name == "/etc/master.passwd" else "644", name)
            expected[name] = path.read_bytes()
            if kind:
                audit(path, args.arch, kind)
                types[name] = kind
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        verify_installed_files(extracted, args.arch, expected, types)
        run(["fsck.hfsplus", "-n", image])
        report["installed_sha256"] = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(expected.items())}
        report["root_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
        args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
        image.replace(args.output)
        print(f"Native dash {args.arch}: signed shell, genuine file accounts and audited HFS root ready")


if __name__ == "__main__":
    main()
