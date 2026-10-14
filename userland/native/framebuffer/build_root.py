#!/usr/bin/env python3
"""Install a signed native framebuffer probe in a copy of the real minimal root."""
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
    parser.add_argument("--program", choices=("native-framebuffer-test", "native-input-test"), default="native-framebuffer-test")
    for name in ("probe", "system", "base-root", "dyld", "hfs-tool", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        system = work / "system"
        with tarfile.open(args.system) as archive:
            archive.extractall(system, filter="data")
        probe = work / args.program
        run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
             "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
             "-undefined", "dynamic_lookup", "-L", system / "usr/lib/system", "-o", probe,
             args.probe.resolve(), system / "usr/lib/libSystem.B.dylib"])
        sign(probe)
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)
        def edit(*command):
            return run([args.hfs_tool.resolve(), image, *command])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/bin/" + args.program: probe, "/usr/lib/dyld": args.dyld}
        for path in (system / "usr").rglob("*"):
            if path.is_file():
                installed["/" + str(path.relative_to(system))] = path
        for name, path in installed.items():
            edit("add", path, name)
            edit("chmod", "755", name)
            expected[name] = path.read_bytes()
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        providers = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        providers |= set().union(*(symbols(path)[0] for path in (extracted / "usr/lib").rglob("*.dylib")))
        types = {}
        for name, data in expected.items():
            if name.startswith(("/bin/", "/sbin/")) or name == "/usr/lib/dyld" or name.endswith(".dylib"):
                kind = 7 if name == "/usr/lib/dyld" else 6 if name.endswith(".dylib") else 2
                cpu = 0x100000c if args.arch == "arm64" else 0x1000007
                if len(data) < 32 or struct.unpack_from("<4I", data)[::3] != (0xfeedfacf, kind) or struct.unpack_from("<I", data, 4)[0] != cpu:
                    raise ValueError("wrong native framebuffer image: " + name)
                types[name] = kind
                if kind != 7 and (missing := symbols(extracted / name.lstrip("/"))[1] - providers):
                    raise ValueError("framebuffer import lacks real provider: " + name + ": " + ", ".join(sorted(missing)))
        verify_installed_files(extracted, args.arch, expected, types)
        run(["fsck.hfsplus", "-n", image])
        args.report.write_text(json.dumps({"architecture": args.arch,
            "base_root_sha256": sha(args.base_root), "system_tar_sha256": sha(args.system),
            "probe_sha256": sha(probe), "root_sha256": sha(image), "runtime_checked": False,
            "installed_sha256": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(expected.items())},
            "validation": "Complete HFS inventory, byte equality, target Mach-O CPU/type, signed images, actual import closure and fsck."}, indent=2) + "\n")
        image.replace(args.output)


if __name__ == "__main__":
    main()
