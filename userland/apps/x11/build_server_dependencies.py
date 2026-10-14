#!/usr/bin/env python3
"""Build original X server font and keyboard libraries through native Bazel."""
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gtk"))
from build_gtk import meson_list
from build_accessibility import merge_archive, run, symbols, definitions, digest, pack

RELEASES = {"libfontenc": "1.1.9", "libXfont2": "2.0.9", "libxkbfile": "1.2.0"}
FEATURES = {"XFONT_FONTFILE", "XFONT_FREETYPE", "XFONT_BITMAP", "XFONT_BDFFORMAT", "XFONT_PCFFORMAT", "XFONT_BUILTINS", "XFONT_PCF_OR_BUILTIN"}


def font_sources(source):
    text = (source / "Makefile.am").read_text().replace("\\\n", " ")
    active, files = [True], []
    for line in text.splitlines():
        if line.startswith("if "): active.append(active[-1] and line.split()[1] in FEATURES)
        elif line.startswith("endif"): active.pop()
        elif active[-1] and re.match(r"^libXfont2_la_SOURCES\s*\+?=", line):
            files += [name for name in line.split("=", 1)[1].split() if name.endswith(".c")]
    return files


def inventories(base):
    font = base / "libXfont2" / RELEASES["libXfont2"]
    enc = (base / "libfontenc" / RELEASES["libfontenc"] / "src/Makefile.am").read_text().replace("\\\n", " ")
    enc = re.search(r"^libfontenc_la_SOURCES\s*=\s*(.*)$", enc, re.M)[1]
    xkb = (base / "libxkbfile" / RELEASES["libxkbfile"] / "src/meson.build").read_text()
    return {"libfontenc": ["src/" + n for n in enc.split() if n.endswith(".c")], "libXfont2": font_sources(font),
            "libxkbfile": ["src/" + n for n in meson_list(xkb, "libxkbfile_sources") if n.endswith(".c")]}


def replacement_sources(name, features):
    if name == "libfontenc": return [] if features["HAVE_REALLOCARRAY"] else ["src/reallocarray.c"]
    if name == "libXfont2":
        return ["src/util/" + function + ".c" for function in ("reallocarray", "realpath", "strlcat", "strlcpy")
                if not features["HAVE_" + function.upper()]]
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "gtk", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); base = root / "src/Applications/xserver"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xserver-deps-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"; generated.mkdir()
        with tarfile.open(args.sdk) as archive: archive.extractall(sdk, filter="data")
        merge_archive(args.gtk, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch: raise ValueError("X server dependency SDK mismatch")
        for name, version in RELEASES.items():
            source = base / name / version
            for header in (source / "include").rglob("*.h"):
                path = install / "usr/include" / header.relative_to(source / "include"); path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(header, path)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1",
                 "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include", "-I", install / "usr/include", "-I", install / "usr/include/freetype2"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        libraries, features, configurations = {}, {}, {}
        inventory = inventories(base)
        for name, version in RELEASES.items():
            source = base / name / version; config_dir = generated / name; config_dir.mkdir()
            values = {"PACKAGE": '"' + name + '"', "PACKAGE_NAME": '"' + name + '"', "PACKAGE_VERSION": '"' + version + '"',
                      "PACKAGE_STRING": '"' + name + ' ' + version + '"', "VERSION": '"' + version + '"', "STDC_HEADERS": 1}
            if name == "libXfont2":
                values.update({feature: 1 for feature in FEATURES if feature != "XFONT_PCF_OR_BUILTIN"})
                values.update(X_GZIP_FONT_COMPRESSION=1)
            tested = {}
            def probe(key, text, required=False):
                path = config_dir / "feature.c"; path.write_text(text)
                result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(config_dir / "feature.o")], capture_output=True)
                tested[key] = result.returncode == 0
                if required and result.returncode: raise ValueError(key + ":\n" + result.stderr.decode())
                if not result.returncode: values[key] = 1
            for header in ("dlfcn.h", "inttypes.h", "stdint.h", "stdio.h", "stdlib.h", "strings.h", "string.h", "sys/stat.h", "sys/types.h", "unistd.h", "wchar.h", "err.h", "float.h", "poll.h", "sys/poll.h"):
                probe("HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()), "#include <" + header + ">\n")
            headers = '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <strings.h>\n#include <unistd.h>\n#include <poll.h>\n'
            for function in ("reallocarray", "reallocf", "strndup", "getc_unlocked", "strlcpy", "strlcat", "strcasecmp", "readlink", "realpath", "poll"):
                key = "HAVE_UNLOCKED_STDIO" if function == "getc_unlocked" else "HAVE_" + function.upper()
                probe(key, headers + "void *p=(void*)&" + function + ";\n")
            (config_dir / "config.h").write_text("/* Native Darwin target declarations. */\n" + "".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
            local = ["-I", config_dir, "-I", source, "-I", source / "include", "-I", source / "include/X11/fonts", "-I", source / "include/X11/extensions"]
            if name == "libfontenc": local += ['-DFONT_ENCODINGS_DIRECTORY="/usr/share/fonts/encodings/encodings.dir"']
            if name == "libXfont2": local += ["-fvisibility=hidden"]
            replacements = replacement_sources(name, tested)
            jobs = [(source / path, config_dir / f"{name}_{index:03d}.o") for index, path in enumerate(inventory[name] + replacements)]
            def compile_one(job):
                path, obj = job
                try: run([*flags, *local, "-c", path, "-o", obj])
                except RuntimeError as error: return {"error": str(error)}
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1): raise ValueError("non-native server dependency object")
                return {"source": str(path.relative_to(root)), "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool: records = list(pool.map(compile_one, jobs))
            errors = [record["error"] for record in records if "error" in record]
            if errors: raise ValueError("native server dependency compilation failed:\n" + "\n".join(errors))
            archive = install / "usr/lib" / (name + ".a"); run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            exported, imported, weak = symbols(archive)
            libraries[name] = {"sources": records, "exports": sorted(exported), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak),
                               "original_replacement_sources": replacements}
            features[name] = tested; configurations[name] = values
            licenses = install / "usr/share/licenses" / name; licenses.mkdir(parents=True)
            shutil.copyfile(source / "COPYING", licenses / "COPYING")
        defined, undefined = set(), set()
        for path in install.rglob("*.a"): defined |= definitions(path); undefined |= symbols(path)[1]
        for path in (install / "usr/libexec").glob("*.o"): undefined |= symbols(path)[1]
        requests = sorted(undefined - defined - {"___dso_handle"}); args.imports.write_text("\n".join(requests) + "\n")
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": libraries, "configuration": configurations, "sdk_features": features,
            "system_requests": requests, "bundle_sha256": digest(args.output), "runtime_checked": False,
            "disabled_optional_font_backends": ["bzip2 compression", "obsolete SNF format", "remote X Font Services protocol (upstream default disabled)"]}, indent=2) + "\n")
        print(f"Native X server dependencies {args.arch}: {sum(len(record['sources']) for record in libraries.values())} Mach-O objects")


if __name__ == "__main__": main()
