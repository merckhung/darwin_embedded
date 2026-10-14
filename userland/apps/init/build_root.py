#!/usr/bin/env python3
"""Install a real native PID 1 and shell into audited development/test HFS roots."""
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
from build_diagnostic_userland import verify_installed_files
from sign_diagnostic_macho import sign


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    for name in ("base-root", "system", "dyld", "init", "uname", "home", "home-script", "busybox", "busybox-report", "hfs-tool", "metadata-tool", "output", "test-output", "report", "test-report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-init-root-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        system = work / "system"
        with tarfile.open(args.system) as archive:
            archive.extractall(system, filter="data")
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
                "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
                "-undefined", "dynamic_lookup", "-L", system / "usr/lib/system"]
        programs = {}
        for name, source in (("init", args.init), ("uname", args.uname), ("native-home-test", args.home), ("busybox",args.busybox)):
            destination = work / name
            run([*link, "-o", destination, source.resolve(), system / "usr/lib/libSystem.B.dylib"])
            sign(destination)
            programs[name] = destination
        image = work / "test-root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*command):
            return run([args.hfs_tool.resolve(), image, *command])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        # This base contains actual dash plus account/script probes. Replace
        # every diagnostic executable and install the complete selected runtime.
        installed = {"/sbin/launchd": programs["init"], "/bin/uname": programs["uname"],
                     "/bin/busybox": programs["busybox"],
                     "/bin/sh": original / "bin/dash", "/usr/lib/dyld": args.dyld,
                     "/bin/native-home-test": programs["native-home-test"],
                     "/etc/native-shell-test.sh": args.home_script,
                     "/etc/native-shell-checks.sh": original / "etc/native-shell-test.sh"}
        for path in (system / "usr").rglob("*"):
            if path.is_file():
                installed["/" + str(path.relative_to(system))] = path
        about = work / "os-release"
        about.write_text('NAME="Darwin Embedded"\nDESCRIPTION="Native Mach-O dash with embedded PID 1"\n')
        installed["/etc/os-release"] = about
        for name, source in installed.items():
            edit("add", source, name)
            edit("chmod", "644" if name.startswith("/etc/") else "755", name)
            expected[name] = source.read_bytes()
        busybox_report=json.loads(args.busybox_report.read_text())
        if busybox_report['architecture']!=args.arch:raise ValueError('BusyBox architecture mismatch')
        aliases={'/bin/'+name:'/bin/busybox' for name in busybox_report['applets'] if '/bin/'+name not in expected}
        for name,target in aliases.items():
            edit('symlink',name,target)
            expected[name]=target.encode()
        # Writable temporary files are a real HFS operation, not a syscall shim.
        edit("chmod", "1777", "/tmp")
        edit("chmod", "700", "/var/root")
        run([args.metadata_tool.resolve(), image, "/home/desktop", "501", "20", "700"])
        providers = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        providers |= set().union(*(symbols(path)[0] for path in (system / "usr/lib").rglob("*.dylib")))

        def verify(output, report_path, test):
            expected_metadata = {
                "/home/desktop": {"uid": 501, "gid": 20, "mode": 0o40700},
                "/var/root": {"uid": 0, "gid": 0, "mode": 0o40700},
                "/tmp": {"uid": 0, "gid": 0, "mode": 0o41777},
                "/etc/master.passwd": {"uid": 0, "gid": 0, "mode": 0o100600},
            }
            actual_metadata = {name: json.loads(run([args.metadata_tool.resolve(), image, name]))
                               for name in expected_metadata}
            if actual_metadata != expected_metadata:
                raise ValueError("HFS ownership/mode mismatch: " + json.dumps(actual_metadata, sort_keys=True))
            for name,target in aliases.items():
                metadata=json.loads(run([args.metadata_tool.resolve(),image,name]))
                if metadata['mode']&0o170000!=0o120000 or expected[name]!=target.encode():raise ValueError('Invalid BusyBox applet symlink: '+name)
            extracted = work / ("verified-test" if test else "verified-root")
            extracted.mkdir()
            edit("extractall", "/", extracted)
            types, programs_report = {}, {}
            cpu = 0x100000c if args.arch == "arm64" else 0x1000007
            for name, data in expected.items():
                if name in aliases:continue
                if name.startswith(("/bin/", "/sbin/")) or name == "/usr/lib/dyld" or name.endswith(".dylib"):
                    kind = 7 if name == "/usr/lib/dyld" else 6 if name.endswith(".dylib") else 2
                    if len(data) < 32 or struct.unpack_from("<4I", data)[::3] != (0xfeedfacf, kind):
                        raise ValueError("expected Mach-O program: " + name)
                    if struct.unpack_from("<I", data, 4)[0] != cpu:
                        raise ValueError("wrong Mach-O CPU: " + name)
                    types[name] = kind
                    path = extracted / name.lstrip("/")
                    if kind == 2 and (missing := symbols(path)[1] - providers):
                        raise ValueError("program lacks actual provider: " + name + ": " + ", ".join(sorted(missing)))
                    programs_report[name] = {"sha256": hashlib.sha256(data).hexdigest(), "kind": kind}
            verify_installed_files(extracted, args.arch, expected, types)
            if expected["/bin/sh"] != expected["/bin/dash"]:
                raise ValueError("/bin/sh must contain the exact upstream dash executable")
            run(["fsck.hfsplus", "-n", image])
            report = {"architecture": args.arch, "test_fixtures": test,
                      "init": "embedded native PID 1 installed at XNU's /sbin/launchd path; no launchd service API emulation",
                      "system_tar_sha256": sha(args.system), "base_shell_root_sha256": sha(args.base_root),
                      "init_object_sha256": sha(args.init), "uname_object_sha256": sha(args.uname),
                      "home_probe_object_sha256": sha(args.home) if test else None,
                      "busybox_version":busybox_report['version'],"busybox_archive_sha256":sha(args.busybox),"busybox_applets":busybox_report['applets'],"busybox_aliases":aliases,
                      "hfs_metadata": actual_metadata,
                      "programs": programs_report,
                      "installed_sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(expected.items())},
                      "root_sha256": sha(image), "runtime_checked": False,
                      "validation": "Complete extracted HFS file inventory, byte equality, native Mach-O CPU/type and actual import providers, no ELF, fsck read-only check."}
            report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
            shutil.copyfile(image, output)

        verify(args.test_output, args.test_report, True)
        # The normal minimal root carries real accounts, without malformed or
        # synthetic fixture records or the test executables/scripts.
        for name in ("/bin/native-shell-test", "/etc/native-shell-test.sh", "/bin/native-home-test", "/etc/native-shell-checks.sh"):
            del expected[name]
        master = work / "master.passwd"
        master.write_text("root:*:0:0::0:0:System Administrator:/var/root:/bin/sh\n"
                          "desktop:*:501:20:staff:0:0:Desktop User:/home/desktop:/bin/sh\n")
        passwd = work / "passwd"
        passwd.write_text("root:*:0:0:System Administrator:/var/root:/bin/sh\n"
                          "desktop:*:501:20:Desktop User:/home/desktop:/bin/sh\n")
        expected["/etc/group"] = b"wheel:*:0:root\nstaff:*:20:desktop\n"
        for name, source in (("/etc/master.passwd", master), ("/etc/passwd", passwd)):
            expected[name] = source.read_bytes()
        # Construct the clean inventory in a fresh volume. The imported host
        # HFS editor cannot reliably remove these catalog keys in-place.
        image = work / "root.img"
        with image.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024)
        run(["mkfs.hfsplus", "-v", "DarwinEmbedded", image])
        directories = {"/" + str(path.relative_to(original))
                       for path in original.rglob("*") if path.is_dir()}
        for name in sorted(directories, key=lambda name: (len(Path(name).parts), name)):
            edit("mkdir", name)
        staging = work / "staging"
        for name, data in sorted(expected.items()):
            if name in aliases:
                edit('symlink',name,aliases[name])
                continue
            source = staging / name.lstrip("/")
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(data)
            edit("add", source, name)
            binary = name.startswith(("/bin/", "/sbin/")) or name == "/usr/lib/dyld" or name.endswith(".dylib")
            edit("chmod", "755" if binary else "600" if name == "/etc/master.passwd" else "644", name)
        edit("chmod", "1777", "/tmp")
        edit("chmod", "700", "/var/root")
        run([args.metadata_tool.resolve(), image, "/home/desktop", "501", "20", "700"])
        verify(args.output, args.report, False)
        print(f"Native embedded init {args.arch}: clean and fixture roots audited; real dash installed as /bin/sh")


if __name__ == "__main__":
    main()
