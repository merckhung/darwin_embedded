#!/usr/bin/env python3
"""Compile original libxml2 inventories as native Darwin Mach-O in Bazel."""
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
from build_pango import meson_list
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from build_clients import pack

VERSION = "2.15.4"
FEATURES = ("c14n", "catalog", "debug", "html", "iconv", "iso8859x", "modules", "output", "pattern", "push", "reader",
            "regexps", "relaxng", "sax1", "schemas", "schematron", "threads", "valid", "writer", "xinclude", "xpath", "xptr", "zlib")


def source_inventory(recipe):
    files = meson_list(recipe, "xml_src")
    block = re.search(r"^xml_opt_src\s*=\s*\[(.*?)^\]", recipe, re.M | re.S)
    if not block:
        raise ValueError("original libxml2 optional inventory missing")
    groups = {name: re.findall(r"'([^']+\.c)'", content)
              for name, content in re.findall(r"\[want_(\w+),\s*\[(.*?)\]\]", block[1], re.S)}
    for feature in FEATURES:
        files += groups.get(feature, [])
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dbus", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/xml/libxml2" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-libxml-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        merge_archive(args.dbus, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("libxml2 SDK architecture mismatch")
        include = install / "usr/include/libxml2/libxml"
        include.mkdir(parents=True)
        for header in (source / "include/libxml").glob("*.h"):
            shutil.copyfile(header, include / header.name)
        features = {"WITH_" + feature.upper(): 1 for feature in FEATURES}
        features.update(WITH_HTTP=0, WITH_ICU=0, WITH_LEGACY=0, WITH_THREAD_ALLOC=0)
        version_values = {**features, "VERSION": VERSION, "LIBXML_VERSION_NUMBER": 21504, "LIBXML_VERSION_EXTRA": "", "MODULE_EXTENSION": ".so"}
        configured = re.sub(r"@(\w+)@", lambda m: str(version_values[m[1]]), (source / "include/libxml/xmlversion.h.in").read_text())
        (include / "xmlversion.h").write_text(configured)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fno-common", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1",
                 "-D_FORTIFY_SOURCE=0", "-DIN_LIBXML", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-Wno-deprecated-declarations", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", install / "usr/include", "-I", include.parent, "-I", generated, "-I", source, "-I", source / "include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        values = {"XML_SYSCONFDIR": '"/etc"'}
        sdk_features = {}
        def probe(name, text, required=False, boolean=False):
            path = generated / "feature.c"
            path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            sdk_features[name] = result.returncode == 0
            if required and result.returncode:
                raise ValueError(name + ":\n" + result.stderr.decode())
            if sdk_features[name] or boolean:
                values[name] = int(sdk_features[name])
        for header in ("stdint.h", "inttypes.h", "stdio.h", "stdlib.h", "string.h", "strings.h", "sys/stat.h", "sys/types.h", "unistd.h", "dlfcn.h", "pthread.h"):
            probe("HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()), "#include <" + header + ">\n", required=True)
        for name, header, symbol in (("HAVE_DECL_GETENTROPY", "sys/random.h", "getentropy"),
                                     ("HAVE_DECL_GLOB", "glob.h", "glob"), ("HAVE_DECL_MMAP", "sys/mman.h", "mmap")):
            probe(name, f"#include <{header}>\nvoid *f=(void *)&{symbol};\n", boolean=True)
        probe("HAVE_DLOPEN", "#include <dlfcn.h>\nvoid *f=(void *)&dlopen;\n", required=True)
        probe("HAVE_FUNC_ATTRIBUTE_DESTRUCTOR", "__attribute__((destructor)) static void f(void){}\n", required=True)
        probe("Darwin_LP64", "#include <stdint.h>\n_Static_assert(__APPLE__ && __MACH__ && sizeof(void*)==8 && sizeof(long)==8 && sizeof(int)==4,\"Darwin LP64\");\n", required=True)
        (generated / "config.h").write_text("/* Native Darwin target declarations. */\n" + "".join(f"#define {name} {value}\n" for name, value in sorted(values.items())))
        recipe = (source / "meson.build").read_text()
        jobs = [(source / name, generated / f"xml_{index:03d}.o") for index, name in enumerate(source_inventory(recipe))]
        def compile_one(job):
            path, obj = job
            try:
                run([*flags, "-c", path, "-o", obj])
            except RuntimeError as error:
                return {"error": str(error)}
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native XML source object")
            return {"source": str(path.relative_to(root)), "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool:
            records = list(pool.map(compile_one, jobs))
        errors = [entry["error"] for entry in records if "error" in entry]
        if errors:
            raise ValueError("native XML compilation failed:\n" + "\n".join(errors))
        archive = install / "usr/lib/libxml2.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
        exported, imported, weak = symbols(archive)
        license_dir = install / "usr/share/licenses/libxml2"
        license_dir.mkdir(parents=True)
        shutil.copyfile(source / "Copyright", license_dir / "Copyright")
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            defined |= definitions(path)
            undefined |= symbols(path)[1]
        for path in (install / "usr/libexec").glob("*.o"):
            undefined |= symbols(path)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION,
            "libraries": {"libxml2": {"sources": records, "exports": sorted(exported), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak)}},
            "configuration": values, "sdk_features": sdk_features, "feature_configuration": features, "system_requests": requests,
            "generated_headers": {"usr/include/libxml2/libxml/xmlversion.h": digest(include / "xmlversion.h")},
            "bundle_sha256": digest(args.output), "runtime_checked": False,
            "disabled_optional_backends": ["removed HTTP compatibility APIs", "removed legacy compatibility APIs", "ICU", "Python bindings", "readline/history"]}, indent=2) + "\n")
        print(f"Native libxml2 {args.arch}: {len(records)} original Mach-O objects, {len(exported)} exports")


if __name__ == "__main__": main()
