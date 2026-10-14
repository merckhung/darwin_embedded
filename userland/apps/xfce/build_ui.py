#!/usr/bin/env python3
"""Compile the original Xfce X11 UI and keyboard libraries through Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

from build_base import inventory, run, symbols, digest, definitions, pack

def enabled_inventory(recipe, variable):
    """Read release inventories; expand references and the selected X11 additions."""
    text = recipe.read_text().replace("\\\n", " ")
    tokens = inventory(recipe, variable)
    for block in re.findall(r"^if ENABLE_X11\n(.*?)^endif", text, re.M | re.S):
        match = re.search(r"^" + re.escape(variable) + r"\s*\+=\s*(.*)$", block, re.M)
        if match:
            tokens += match[1].split()
    result = []
    for token in tokens:
        if token == "$(NULL)":
            continue
        match = re.fullmatch(r"\$\((\w+)\)", token)
        result += enabled_inventory(recipe, match[1]) if match else [token]
    return list(dict.fromkeys(result))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    source = root / "src/Applications/xfce/libxfce4ui/4.20.2"
    manifest = json.loads((source / "UPSTREAM.json").read_text())
    for name, expected in manifest["upstream_files_sha256"].items():
        if digest(source / name) != expected:
            raise ValueError("modified original Xfce UI source: " + name)
    with tempfile.TemporaryDirectory(prefix=".native-xfce-ui-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        for path, target in ((args.sdk, sdk), (args.dependencies, install)):
            with tarfile.open(path) as archive:
                archive.extractall(target, filter="data")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-isystem", resource + "/include", "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        for include in ("", "glib-2.0", "gio-unix-2.0", "gtk-3.0", "pango-1.0", "gdk-pixbuf-2.0", "atk-1.0", "harfbuzz", "freetype2", "cairo",
                        "xfce4", "xfce4/xfconf-0", "xfce4/libxfce4ui-2", "xfce4/libxfce4kbd-private-3"):
            flags += ["-I", install / "usr/include" / include]
        values = {"PACKAGE": '"libxfce4ui"', "PACKAGE_NAME": '"libxfce4ui"', "PACKAGE_VERSION": '"4.20.2"',
                  "PACKAGE_STRING": '"libxfce4ui 4.20.2"', "PACKAGE_BUGREPORT": '"https://gitlab.xfce.org/xfce/libxfce4ui"',
                  "GETTEXT_PACKAGE": '"libxfce4ui"', "ENABLE_NLS": 1, "ENABLE_X11": 1, "ENABLE_LIBSM": 1,
                  "STDC_HEADERS": 1, "DATADIR": '"/usr/share"', "PREFIX": '"/usr"', "LIBEXECDIR": '"/usr/libexec"',
                  "PACKAGE_LOCALE_DIR": '"/usr/share/locale"', "LIBXFCE4UI_VERSION_API": '"2"',
                  "MANUAL_WEBSITE": '"https://docs.xfce.org/help.php"'}
        probes = {}
        def probe(key, code):
            file = generated / "probe.c"; file.write_text(code)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(file), "-o", str(generated / "probe.o")], capture_output=True)
            probes[key] = result.returncode == 0
            if not result.returncode:
                values[key] = 1
        for header in ("crt_externs.h", "errno.h", "fcntl.h", "limits.h", "locale.h", "math.h", "memory.h", "signal.h", "stdarg.h", "stdlib.h", "string.h", "unistd.h"):
            probe("HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()), '#include <' + header + '>\n')
        probe("HAVE_DECL_ENVIRON", '#include <unistd.h>\nvoid *p=(void*)&environ;\n')
        probe("HAVE__NSGETENVIRON", '#include <crt_externs.h>\nvoid *p=(void*)&_NSGetEnviron;\n')
        probe("HAVE_BIND_TEXTDOMAIN_CODESET", '#include <libintl.h>\nvoid *p=(void*)&bind_textdomain_codeset;\n')
        (generated / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
        libraries = {}
        for name, subdirectory, source_variable, header_variable, api in (
            ("libxfce4ui-2", "libxfce4ui", "libxfce4ui_sources", "libxfce4ui_headers", "libxfce4ui-2"),
            ("libxfce4kbd-private-3", "libxfce4kbd-private", "libxfce4kbd_sources", "libxfce4kbd_headers", "libxfce4kbd-private-3")):
            recipe = source / subdirectory / "Makefile.am"
            include = install / "usr/include/xfce4" / api / subdirectory; include.mkdir(parents=True, exist_ok=True)
            for header in enabled_inventory(recipe, header_variable):
                shutil.copyfile(source / subdirectory / header, include / header)
            if subdirectory == "libxfce4ui":
                versions = {"LIBXFCE4UI_VERSION_" + key: value for key, value in zip(("MAJOR", "MINOR", "MICRO"), ("4", "20", "2"), strict=True)}
                (include / "libxfce4ui-config.h").write_text(re.sub(r"@(\w+)@", lambda m: versions[m[1]], (source / subdirectory / "libxfce4ui-config.h.in").read_text()))
                umbrella = (source / subdirectory / "libxfce4ui.h.in").read_text()
                umbrella = re.sub(r"^@(?:BEGIN|END)_(?:X11|WAYLAND)_HEADERS@\n", "", umbrella, flags=re.M)
                (include / "libxfce4ui.h").write_text(umbrella)
            files = [source / subdirectory / file for file in enabled_inventory(recipe, source_variable) if file.endswith(".c")]
            jobs = [(file, generated / f"{name}-{index:02d}.o") for index, file in enumerate(files)]
            local = ["-I", generated, "-I", source, "-I", source / subdirectory, "-I", include]
            if subdirectory == "libxfce4ui":
                local += ["-DLIBXFCE4UI_COMPILATION", '-DG_LOG_DOMAIN="libxfce4ui"']
            else:
                local += ['-DG_LOG_DOMAIN="libxfce4kbd-private"']
            def compile_one(job):
                file, obj = job; run([*flags, *local, "-c", file, "-o", obj])
                if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign Xfce UI object")
                return {"source": str(file.relative_to(root)), "source_sha256": digest(file), "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                records = list(pool.map(compile_one, jobs))
            archive = install / "usr/lib" / (name + ".a"); run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            public, imported, weak = symbols(archive)
            libraries[name] = {"sources": records, "exports": sorted(public), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak), "archive_sha256": digest(archive)}
        shortcuts = install / "etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-keyboard-shortcuts.xml"
        shortcuts.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source / "libxfce4kbd-private/xfce4-keyboard-shortcuts.xml", shortcuts)
        license = install / "usr/share/licenses/libxfce4ui/COPYING"; license.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(source / "COPYING", license)
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install / "usr/libexec/xorg"):
                continue
            defined |= definitions(path); undefined |= symbols(path)[1]
        args.imports.write_text("\n".join(sorted(undefined - defined - {"___dso_handle"})) + "\n")
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": libraries, "configuration": values, "sdk_probes": probes,
            "bundle_sha256": digest(args.output), "runtime_checked": False, "desktop_checked": False,
            "features": "Original GTK3 X11 UI, X11 keyboard grabs and real ICE/SM session client; optional startup-notification not yet built",
            "generated_sources": "Original released enum, marshal, resource and visibility sources; version and umbrella headers from original templates"}, indent=2) + "\n")
        print(f"Native Xfce UI {args.arch}: {sum(len(v['sources']) for v in libraries.values())} original Mach-O UI/keyboard objects")

if __name__ == "__main__":
    main()
