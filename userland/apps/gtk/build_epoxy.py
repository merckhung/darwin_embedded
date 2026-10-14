#!/usr/bin/env python3
"""Generate and compile original native GL/GLX dispatch without a fake GL provider."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import tarfile
import tempfile

from build_accessibility import merge_archive, run, symbols, definitions, digest, pack

VERSION = "1.5.10"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "accessibility", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/widgets/libepoxy" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-epoxy-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive: archive.extractall(sdk, filter="data")
        merge_archive(args.accessibility, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("libepoxy SDK architecture mismatch")
        include = install / "usr/include/epoxy"
        include.mkdir(parents=True)
        for header in ("common.h", "gl.h", "glx.h"):
            shutil.copyfile(source / "include/epoxy" / header, include / header)
        for api in ("gl", "glx"):
            run(["python3", source / "src/gen_dispatch.py", "--source", "--no-header", "--outputdir=" + str(generated), source / "registry" / (api + ".xml")])
            run(["python3", source / "src/gen_dispatch.py", "--header", "--no-source", "--outputdir=" + str(include), source / "registry" / (api + ".xml")])
        values = {"ENABLE_GLX": 1, "ENABLE_EGL": 0, "ENABLE_X11": 1, "HAVE_KHRPLATFORM_H": 0,
                  "EPOXY_PUBLIC": '__attribute__((visibility("default"))) extern', "PACKAGE_NAME": '"libepoxy"',
                  "PACKAGE_VERSION": '"' + VERSION + '"', "PACKAGE_STRING": '"libepoxy-' + VERSION + '"',
                  "PACKAGE_DATADIR": '"/usr/share"', "PACKAGE_LIBDIR": '"/usr/lib"', "PACKAGE_LOCALEDIR": '"/usr/share/locale"', "PACKAGE_LIBEXECDIR": '"/usr/libexec"'}
        (generated / "config.h").write_text("/* Native Darwin; optional GLX dispatch for GTK's X11 backend. */\n" +
            "".join(f"#define {name} {value}\n" for name, value in sorted(values.items())))
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fvisibility=hidden",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-Wno-int-conversion", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", generated, "-I", install / "usr/include", "-I", source / "src"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        files = [source / "src/dispatch_common.c", generated / "gl_generated_dispatch.c", source / "src/dispatch_glx.c", generated / "glx_generated_dispatch.c"]
        records, objects = [], []
        for index, path in enumerate(files):
            obj = generated / f"epoxy_{index:03d}.o"
            run([*flags, "-c", path, "-o", obj])
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native libepoxy object")
            objects.append(obj)
            records.append({"source": "generated/" + path.name if path.is_relative_to(generated) else str(path.relative_to(root)),
                            "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)})
        archive = install / "usr/lib/libepoxy.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *objects])
        exported, imported, weak = symbols(archive)
        licenses = install / "usr/share/licenses/libepoxy"
        licenses.mkdir(parents=True)
        shutil.copyfile(source / "COPYING", licenses / "COPYING")
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            defined |= definitions(path)
            undefined |= symbols(path)[1]
        for path in (install / "usr/libexec").glob("*.o"): undefined |= symbols(path)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION,
            "libraries": {"libepoxy": {"sources": records, "exports": sorted(exported), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak)}},
            "configuration": values, "system_requests": requests, "bundle_sha256": digest(args.output), "runtime_checked": False,
            "opengl_provider_supplied": False, "glx_dispatch_enabled": True, "egl_dispatch_enabled": False,
            "upstream_compiler_options": ["-Wno-int-conversion"],
            "generated_headers": {str(path.relative_to(install)): digest(path) for path in include.glob("*_generated.h")}}, indent=2) + "\n")
        print(f"Native libepoxy {args.arch}: four native GL/GLX dispatch objects, {len(exported)} exports; no GL provider supplied")


if __name__ == "__main__": main()
