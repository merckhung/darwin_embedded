#!/usr/bin/env python3
"""Compile original Exo, Garcon and Xfce windowing libraries through Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

from build_base import run, symbols, digest, definitions, pack
from build_ui import enabled_inventory

RELEASES = {"libdisplay-info": "0.4.0", "exo": "4.20.0", "garcon": "4.20.0", "libxfce4windowing": "4.20.7"}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xfce-desktop-libs-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, install = work / "sdk", work / "install"
        os.environ["TMPDIR"] = str(work); os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        for path, target in ((args.sdk, sdk), (args.dependencies, install)):
            with tarfile.open(path) as archive:
                archive.extractall(target, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("desktop library SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability", "-isystem", resource + "/include",
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        for include in ("", "glib-2.0", "gio-unix-2.0", "gtk-3.0", "pango-1.0", "gdk-pixbuf-2.0", "atk-1.0", "cairo", "harfbuzz", "freetype2",
                        "startup-notification-1.0", "libwnck-3.0", "xfce4", "xfce4/xfconf-0", "xfce4/libxfce4ui-2", "exo-2", "garcon-1", "garcon-gtk3-1"):
            flags += ["-I", install / "usr/include" / include]
        libraries, configurations, generated_records = {}, {}, {}
        for project, version in RELEASES.items():
            source = root / "src/Applications" / ("xfce-prerequisites" if project == "libdisplay-info" else "xfce") / project / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            for name, expected in manifest["upstream_files_sha256"].items():
                if digest(source / name) != expected:
                    raise ValueError("modified original desktop library source: " + name)
            generated = work / project; generated.mkdir()
            values = {"PACKAGE": json.dumps(project), "PACKAGE_NAME": json.dumps(project), "PACKAGE_VERSION": json.dumps(version),
                      "PACKAGE_STRING": json.dumps(project + " " + version), "VERSION": json.dumps(version), "VERSION_FULL": json.dumps(version),
                      "GETTEXT_PACKAGE": json.dumps(project), "ENABLE_NLS": 1, "STDC_HEADERS": 1,
                      "PACKAGE_LOCALE_DIR": '"/usr/share/locale"', "LOCALEDIR": '"/usr/share/locale"', "DATADIR": '"/usr/share"',
                      "PREFIX": '"/usr"', "SYSCONFDIR": '"/etc"'}
            tests = {}
            for header in ("errno.h", "fcntl.h", "limits.h", "locale.h", "math.h", "memory.h", "mmintrin.h", "signal.h", "stdarg.h", "stdio.h", "stdlib.h", "string.h", "sys/stat.h", "sys/types.h", "sys/wait.h", "time.h", "unistd.h"):
                file = generated / "probe.c"; file.write_text('#include <' + header + '>\n')
                result = subprocess.run([*map(str, flags), "-Werror", "-c", str(file), "-o", str(generated / "probe.o")], capture_output=True)
                key = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()); tests[key] = result.returncode == 0
                if not result.returncode:
                    values[key] = 1
            if project == "libdisplay-info":
                hardware = root / "src/Applications/xfce-prerequisites/hwdata/0.412"
                hardware_manifest = json.loads((hardware / "UPSTREAM.json").read_text())
                for file, expected in hardware_manifest["upstream_files_sha256"].items():
                    if digest(hardware / file) != expected:
                        raise ValueError("modified original hardware database: " + file)
                run(["python3", source / "tool/gen-search-table.py", hardware / "pnp.ids", generated / "pnp-id-table.c", "pnp_id_table"])
                selections = [("libdisplay-info", ".", "", "", "", "")]
            elif project == "exo":
                values.update(LIBEXO_VERSION_API='"2"')
                selections = [("libexo-2", "exo", "libexo_2_la_SOURCES", "libexo_2_include_HEADERS", "exo-2", "EXO_COMPILATION")]
            elif project == "garcon":
                values.update(GARCON_VERSION_API='"1"', SYSCONFIGDIR='"/etc/xdg"')
                selections = [("libgarcon-1", "garcon", "libgarcon_sources", "libgarcon_headers", "garcon-1", "GARCON_COMPILATION"),
                              ("libgarcon-gtk3-1", "garcon-gtk", "libgarcon_gtk_sources", "libgarcon_gtk_headers", "garcon-gtk3-1", "GARCON_COMPILATION")]
            else:
                values.update(ENABLE_X11=1)
                selections = [("libxfce4windowing-0", "libxfce4windowing", "libxfce4windowing_0_la_SOURCES", "libxfce4windowing_0_headers", "xfce4", "LIBXFCE4WINDOWING_COMPILATION"),
                              ("libxfce4windowingui-0", "libxfce4windowingui", "libxfce4windowingui_0_la_SOURCES", "libxfce4windowingui_0_headers", "xfce4", "LIBXFCE4WINDOWINGUI_COMPILATION")]
            (generated / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
            for name, subdir, source_var, header_var, api, compilation in selections:
                localdir = generated / subdir; localdir.mkdir(exist_ok=True)
                recipe = source / subdir / "Makefile.am"
                substitutions = {"LIBEXO_VERSION_" + part: number for part, number in zip(("MAJOR", "MINOR", "MICRO"), version.split("."), strict=True)}
                for prefix in ("GARCON_VERSION_", "LIBXFCE4WINDOWING_VERSION_", "LIBXFCE4WINDOWINGUI_VERSION_"):
                    substitutions.update({prefix + part: number for part, number in zip(("MAJOR", "MINOR", "MICRO"), version.split("."), strict=True)})
                substitutions.update(LIBXFCE4WINDOWING_HAS_X11="#define LIBXFCE4WINDOWING_HAS_X11", LIBXFCE4WINDOWING_HAS_WAYLAND="/* #undef LIBXFCE4WINDOWING_HAS_WAYLAND */")
                for template in (source / subdir).glob("*-config.h.in"):
                    (localdir / template.name.removesuffix(".in")).write_text(re.sub(r"@(\w+)@", lambda m: substitutions[m[1]], template.read_text()))
                if project == "libdisplay-info":
                    text = (source / "meson.build").read_text()
                    match = re.search(r"di_lib = library\(\s*'display-info',\s*\[(.*?)\]", text, re.S)
                    if not match or match[1].count("pnp_id_table") != 1:
                        raise ValueError("original display-info inventory changed")
                    paths = [source / file for file in re.findall(r"'([^']+\.c)'", match[1])] + [generated / "pnp-id-table.c"]
                else:
                    paths = [source / subdir / file for file in enabled_inventory(recipe, source_var) if file.endswith(".c")]
                if project == "libxfce4windowing":
                    for kind, suffix in (("header", ".h"), ("source", ".c")):
                        run(["python3", source / "xdt-gen-visibility", "--kind=" + kind, source / subdir / (subdir + ".symbols"), localdir / (subdir + "-visibility" + suffix)])
                    # Visibility sources are included by original implementations.
                    if subdir == "libxfce4windowing":
                        original = root / "src/Applications/glib/glib/2.90.1/gobject/glib-genmarshal.in"
                        generator = generated / "glib-genmarshal"; generator.write_text(original.read_text().replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
                        for kind, suffix in (("header", ".h"), ("body", ".c")):
                            output = localdir / ("xfw-marshal" + suffix)
                            output.write_text(run(["python3", generator, "--prefix=xfw_marshal", "--internal", "--" + kind, source / subdir / "xfw-marshal.list"]))
                        paths += [localdir / "xfw-marshal.c"]
                include = install / "usr/include" / api / ("libdisplay-info" if project == "libdisplay-info" else subdir); include.mkdir(parents=True, exist_ok=True)
                headers = [str(file.relative_to(source)) for file in (source / "include/libdisplay-info").glob("*.h")] if project == "libdisplay-info" else enabled_inventory(recipe, header_var)
                for header in headers:
                    file = localdir / header if (localdir / header).is_file() else source / subdir / header
                    shutil.copyfile(file, include / Path(header).name)
                local = ["-I", generated, "-I", localdir, "-I", source, "-I", source / subdir, "-DWNCK_I_KNOW_THIS_IS_UNSTABLE"]
                local += ["-I", source / "include"] if project == "libdisplay-info" else ["-D" + compilation]
                jobs = [(file, localdir / (f"{index:02d}-" + file.stem + ".o")) for index, file in enumerate(paths)]
                def compile_one(job):
                    file, obj = job
                    try:
                        run([*flags, *local, "-c", file, "-o", obj])
                    except RuntimeError as error:
                        return {"error": str(error)}
                    if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                        raise ValueError("foreign desktop library object")
                    relative = str(file.relative_to(generated)) if file.is_relative_to(generated) else str(file.relative_to(root))
                    if file.is_relative_to(generated):
                        saved = install / "build-metadata/generated" / project / relative; saved.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(file, saved)
                        generated_records[project + "/" + relative] = digest(file)
                    return {"source": relative, "generated": file.is_relative_to(generated), "source_sha256": digest(file), "member": obj.name, "object_sha256": digest(obj)}
                with ThreadPoolExecutor(max_workers=8) as pool:
                    records = list(pool.map(compile_one, jobs))
                errors = [record["error"] for record in records if "error" in record]
                if errors:
                    raise ValueError("native desktop library compilation failed:\n" + "\n".join(errors))
                archive = install / "usr/lib" / (name + ".a"); run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
                public, imported, weak = symbols(archive)
                libraries[name] = {"sources": records, "exports": sorted(public), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak), "archive_sha256": digest(archive)}
            configurations[project] = {"values": values, "probes": tests}
            license = install / "usr/share/licenses" / project; license.mkdir(parents=True, exist_ok=True)
            for file in [*source.glob("COPYING*"), *source.glob("LICENSE*")]:
                shutil.copyfile(file, license / file.name)
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install / "usr/libexec/xorg"):
                continue
            defined |= definitions(path); undefined |= symbols(path)[1]
        args.imports.write_text("\n".join(sorted(undefined - defined - {"___dso_handle"})) + "\n"); pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": libraries, "configuration": configurations,
            "generated_sources": generated_records, "bundle_sha256": digest(args.output), "runtime_checked": False, "desktop_checked": False,
            "scope": "Complete original Exo, Garcon/GTK and Xfce X11 windowing/UI source inventories; Bazel orchestrates native compilation and original generators."}, indent=2) + "\n")
        print(f"Native Xfce desktop libraries {args.arch}: {sum(len(v['sources']) for v in libraries.values())} Mach-O objects, {len(libraries)} libraries")

if __name__ == "__main__":
    main()
