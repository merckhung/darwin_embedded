#!/usr/bin/env python3
"""Compile missing original Darwin runtime sources for the native GLib port."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--gio", action="store_true")
    parser.add_argument("--libc-output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-glib-support-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("GLib support SDK architecture mismatch")
        source = args.root.resolve() / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers/unix03/fchmod.c"
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-Werror=implicit-function-declaration", "-Wno-availability", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", source.parents[1]]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        objects, records = [], []
        libc = args.root.resolve() / "src/Libraries/25.5.0/Libc"
        paths = [source.with_name(name + ".c") for name in ("fchmod", "chmod")]
        compiled = {}
        if args.gio:
            paths = [libc / "compat-43/FreeBSD/creat.c", source.parents[1] / "utimensat.c",
                     args.root.resolve() / "src/Applications/resolver/openbsd/fb109c562f6c1fced0a18e5ac2791d6d41a8b2f6/lib/libc/gen/fstab.c"]
            original = paths[-1]
            text = original.read_text()
            before = 'if ((_fs_fp = fopen(_PATH_FSTAB, "re")))\n\t\treturn(1);'
            if text.count(before) != 1:
                raise ValueError("upstream fstab open boundary changed")
            text = text.replace(before, '''if ((_fs_fp = fopen(_PATH_FSTAB, "r"))) {
        if (fcntl(fileno(_fs_fp), F_SETFD, FD_CLOEXEC) == 0) return 1;
        int error = errno;
        fclose(_fs_fp);
        _fs_fp = NULL;
        errno = error;
    }''')
            text = '#include <fcntl.h>\n#define DEF_WEAK(x)\nlong long strtonum(const char *, long long, long long, const char **);\n' + text
            generated = work / "fstab.c"
            generated.write_text(text)
            compiled[original] = generated
        for original in paths:
            obj = work / (original.stem + ".o")
            options = ["-I", libc / "fbsdcompat", "-include", libc / "fbsdcompat/_fbsd_compat_.h"] if original.is_relative_to(libc) else ["-DPRIVATE=1"]
            run([*flags, *options, "-c", compiled.get(original, original), "-o", obj])
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native GLib support object")
            objects.append(obj)
            records.append({"source": str(original.relative_to(args.root.resolve())),
                            "source_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                            "compiled_sha256": hashlib.sha256(compiled.get(original, original).read_bytes()).hexdigest(),
                            "object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest()})
        run(["llvm-ar-20", "--format=darwin", "rcsD", args.output, *objects])
        stdio_record = None
        if args.gio:
            if not args.libc_output:
                raise ValueError("GIO support needs an archive linked inside Libc")
            original = libc / "stdio/FreeBSD/fopen.c"
            obj = work / "fopen_darwin.o"
            options = ["-D__LIBC__=1", "-DBUILDING_VARIANT=1", "-DVARIANT_DARWINEXTSN=1",
                       "-D__DARWIN_NON_CANCELABLE=1", "-D__DARWIN_UNIX03=1", "-fblocks", "-DPRIVATE=1",
                       "-D_FORTIFY_SOURCE=0", "-isystem", sdk / "usr/local/include",
                       "-Wno-nullability-completeness", "-Wno-deprecated-declarations",
                       "-include", libc / "fbsdcompat/_fbsd_compat_.h"]
            for directory in ("fbsdcompat", "include", "darwin", "gen/FreeBSD", "stdio", "stdio/FreeBSD",
                              "locale", "locale/FreeBSD", "stdtime/FreeBSD"):
                options += ["-I", libc / directory]
            run([*flags, *options, "-c", original, "-o", obj])
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native Darwin stdio variant")
            run(["llvm-ar-20", "--format=darwin", "rcsD", args.libc_output, obj])
            stdio_record = {"source": str(original.relative_to(args.root.resolve())),
                            "source_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                            "object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest(),
                            "archive_sha256": hashlib.sha256(args.libc_output.read_bytes()).hexdigest()}
        public, imports, weak = symbols(args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "objects": records,
            "archive_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(), "exports": sorted(public),
            "imports": sorted(imports - public), "weak_imports": sorted(weak), "stdio": stdio_record,
            "port_adaptations": ["native FD_CLOEXEC replaces unsupported fopen e mode"] if args.gio else []}, indent=2) + "\n")


if __name__ == "__main__":
    main()
