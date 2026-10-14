#!/usr/bin/env python3
"""Compile original desktop X11 extension clients into native Darwin archives."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glib"))
from build_dependencies import definitions, digest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "fonts"))
from build_cairo import merge_archive
from build_clients import pack

RELEASES = {"libXi": "1.8.3", "libXrandr": "1.5.5", "libXcursor": "1.2.3", "libXfixes": "6.0.2",
            "libXdamage": "1.1.7", "libXcomposite": "0.4.7", "libXinerama": "1.1.6", "libXtst": "1.2.5",
            "libXres": "1.2.3", "libXScrnSaver": "1.2.5"}
LIBRARIES = {name: {"libXres": "libXRes", "libXScrnSaver": "libXss"}.get(name, name) for name in RELEASES}


def make_inventory(source, variable):
    assignments = dict(re.findall(r"^(\w+)\s*=\s*(.*)$", source.read_text().replace("\\\n", " "), re.M))
    def expand(name, active):
        if name in active or name not in assignments:
            raise ValueError("invalid original Make source inventory: " + name)
        return re.sub(r"\$\((\w+)\)", lambda match: expand(match[1], active | {name}), assignments[name])
    return [name for name in expand(variable, set()).split() if name.endswith(".c")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "pixbuf", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xextensions-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        merge_archive(args.pixbuf, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("extension SDK architecture mismatch")
        sources = {name: root / "src/Applications/desktop-x11" / name / release for name, release in RELEASES.items()}
        # Input and cursor headers depend on other extension headers: install
        # the entire original public header set before compiling any client.
        for name, source in sources.items():
            for header in (source / "include").rglob("*.h"):
                target = install / "usr/include" / header.relative_to(source / "include")
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() and target.read_bytes() != header.read_bytes():
                    raise ValueError("conflicting original extension header: " + str(target))
                shutil.copyfile(header, target)
        source = sources["libXcursor"]
        version_values = dict(zip(("XCURSOR_LIB_MAJOR", "XCURSOR_LIB_MINOR", "XCURSOR_LIB_REVISION"), map(int, RELEASES["libXcursor"].split("."))))
        template = (source / "include/X11/Xcursor/Xcursor.h.in").read_text()
        for name, value in version_values.items():
            if template.count("#undef " + name) != 1:
                raise ValueError("original Xcursor version declaration changed")
            template = template.replace("#undef " + name, f"#define {name} {value}")
        header = install / "usr/include/X11/Xcursor/Xcursor.h"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(template)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fno-common",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration",
                 "-Werror=incompatible-pointer-types", "-Wno-availability", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", install / "usr/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        features, configuration, records = {}, {}, {}
        for name in ("dlfcn.h", "inttypes.h", "stdint.h", "stdio.h", "stdlib.h", "strings.h", "string.h", "sys/stat.h", "sys/types.h", "unistd.h", "wchar.h"):
            feature = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", name.upper())
            path = generated / "feature.c"
            path.write_text("#include <" + name + ">\n")
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[feature] = result.returncode == 0
            if features[feature]:
                configuration[feature] = 1
        for name, source in sources.items():
            library = LIBRARIES[name]
            config_dir = generated / library
            config_dir.mkdir()
            values = {**configuration, "STDC_HEADERS": 1, "PACKAGE_VERSION": '"' + RELEASES[name] + '"'}
            major, minor, micro = map(int, RELEASES[name].split("."))
            values.update(PACKAGE_VERSION_MAJOR=major, PACKAGE_VERSION_MINOR=minor, PACKAGE_VERSION_PATCHLEVEL=micro)
            options = ["-I", config_dir, "-I", source / "include", "-I", source / "include/X11", "-I", source / "include/X11/extensions", "-I", source / "src"]
            if name == "libXcursor":
                values.update(HAVE_XFIXES=1, **version_values)
                options += ["-I", header.parent, '-DICONDIR="/usr/share/icons"',
                            '-DXCURSORPATH="~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps"']
            (config_dir / "config.h").write_text("/* Actual native Darwin target configuration. */\n" +
                "".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
            files = [source / "src" / path for path in make_inventory(source / "src/Makefile.am", library + "_la_SOURCES")]
            jobs = [(path, config_dir / f"{library}_{index:03d}.o") for index, path in enumerate(files)]
            def compile_one(job):
                path, obj = job
                run([*flags, *options, "-c", path, "-o", obj])
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("non-native extension source object")
                return {"source": str(path.relative_to(root)), "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                entries = list(pool.map(compile_one, jobs))
            archive = install / "usr/lib" / (library + ".a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            public, imports, weak = symbols(archive)
            records[library] = {"sources": entries, "configuration": values, "exports": sorted(public),
                                "imports": sorted(imports - definitions(archive)), "weak_imports": sorted(weak)}
            license_dir = install / "usr/share/licenses" / name
            license_dir.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / "COPYING", license_dir / "COPYING")
        defined, undefined = set(), set()
        for archive in (install / "usr/lib").glob("*.a"):
            defined |= definitions(archive)
            undefined |= symbols(archive)[1]
        for path in (install / "usr/libexec").glob("*.o"):
            undefined |= symbols(path)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "versions": RELEASES, "libraries": records, "sdk_features": features,
            "configured_headers": {"usr/include/X11/Xcursor/Xcursor.h": digest(header)}, "system_requests": requests,
            "bundle_sha256": digest(args.output), "runtime_checked": False}, indent=2) + "\n")
        print(f"Native desktop X11 {args.arch}: {sum(len(record['sources']) for record in records.values())} original objects, {sum(len(record['exports']) for record in records.values())} exports")


if __name__ == "__main__":
    main()
