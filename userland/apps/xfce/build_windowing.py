#!/usr/bin/env python3
"""Build original XCB utilities, startup notification and Wnck through Bazel."""
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

from build_base import inventory, run, symbols, digest, definitions, pack
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gtk"))
from build_gtk import meson_list

RELEASES = {"xcb-util": "0.4.1", "startup-notification": "0.12", "libwnck": "43.3"}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xfce-windowing-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, install = work / "sdk", work / "install"
        os.environ["TMPDIR"] = str(work); os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        for path, target in ((args.sdk, sdk), (args.dependencies, install)):
            with tarfile.open(path) as archive:
                archive.extractall(target, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("windowing SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1",
                 "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-isystem", resource + "/include", "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        for include in ("", "glib-2.0", "gtk-3.0", "pango-1.0", "gdk-pixbuf-2.0", "atk-1.0", "cairo", "harfbuzz", "freetype2",
                        "startup-notification-1.0", "libwnck-3.0"):
            flags += ["-I", install / "usr/include" / include]
        libraries, configurations = {}, {}
        for project, version in RELEASES.items():
            source = root / "src/Applications/xfce-prerequisites" / project / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            for name, expected in manifest["upstream_files_sha256"].items():
                if digest(source / name) != expected:
                    raise ValueError("modified original windowing source: " + name)
            generated = work / project; generated.mkdir()
            values = {"PACKAGE": json.dumps(project), "PACKAGE_NAME": json.dumps(project), "PACKAGE_STRING": json.dumps(project + " " + version),
                      "PACKAGE_VERSION": json.dumps(version), "VERSION": json.dumps(version), "STDC_HEADERS": 1}
            tests = {}
            def probe(key, code):
                file = generated / "probe.c"; file.write_text(code)
                result = subprocess.run([*map(str, flags), "-Werror", "-c", str(file), "-o", str(generated / "probe.o")], capture_output=True)
                tests[key] = result.returncode == 0
                if not result.returncode:
                    values[key] = 1
            options = []; paths = []; install_headers = {}
            if project == "xcb-util":
                probe("HAVE_VASPRINTF", '#include <stdio.h>\nvoid *p=(void*)&vasprintf;\n')
                paths = [source / "src" / name for name in inventory(source / "src/Makefile.am", "libxcb_util_la_SOURCES")]
                install_headers = {"xcb/" + name: source / "src" / name for name in inventory(source / "src/Makefile.am", "xcbinclude_HEADERS")}
                name = "libxcb-util"
            elif project == "startup-notification":
                values.update(SANE_MALLOC_PROTOS=1, REALLOC_0_WORKS=1)
                probe("HAVE_EXECINFO_H", '#include <execinfo.h>\n')
                if tests["HAVE_EXECINFO_H"]:
                    probe("HAVE_BACKTRACE", '#include <execinfo.h>\nvoid *p=(void*)&backtrace;\n')
                paths = [source / "libsn" / name for name in inventory(source / "libsn/Makefile.am", "libstartup_notification_1_la_SOURCES") if name.endswith(".c")]
                install_headers = {"startup-notification-1.0/libsn/" + name: source / "libsn" / name for name in inventory(source / "libsn/Makefile.am", "libsninclude_HEADERS")}
                options = ["-DSN_API_NOT_YET_FROZEN=1"]; name = "libstartup-notification-1"
            else:
                recipe = (source / "libwnck/meson.build").read_text(); headers = meson_list(recipe, "headers")
                values.update(GETTEXT_PACKAGE='"libwnck-3.0"', HAVE_CAIRO_XLIB_XRENDER=1, HAVE_GETTEXT=1, HAVE_XRES=1, HAVE_LIBSTARTUP_NOTIFICATION_1_0=1, HAVE_STARTUP_NOTIFICATION=1)
                versions = {"WNCK_MAJOR_VERSION": "43", "WNCK_MINOR_VERSION": "3", "WNCK_MICRO_VERSION": "0"}
                header = generated / "version.h"; header.write_text(re.sub(r"@(\w+)@", lambda m: versions[m[1]], (source / "libwnck/version.h.in").read_text()))
                install_headers = {"libwnck-3.0/libwnck/" + name: source / "libwnck" / name for name in headers}
                install_headers["libwnck-3.0/libwnck/version.h"] = header
                glib = root / "src/Applications/glib/glib/2.90.1/gobject/glib-mkenums.in"
                generator = generated / "glib-mkenums"; generator.write_text(glib.read_text().replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
                c_template = '#include <libwnck/libwnck.h>\n#include "wnck-enum-types.h"\n'
                c_head = '''GType
@enum_name@_get_type (void)
{
  static gsize type_id;
  if (g_once_init_enter (&type_id)) {
    static const G@Type@Value values[] = {
'''
                c_value = '      { @VALUENAME@, "@VALUENAME@", "@valuenick@" },\n'
                c_tail = '''      { 0, NULL, NULL }
    };
    GType registered = g_@type@_register_static ("@EnumName@", values);
    g_once_init_leave (&type_id, registered);
  }
  return type_id;
}
'''
                for suffix in ("c", "h"):
                    command = ["python3", generator, "--fhead", c_template if suffix == "c" else '#ifndef __WNCK_ENUM_TYPES_H__\n#define __WNCK_ENUM_TYPES_H__\n#include <glib-object.h>\nG_BEGIN_DECLS\n']
                    if suffix == "c":
                        command += ["--vhead", c_head, "--vprod", c_value, "--vtail", c_tail]
                    else:
                        command += ["--vhead", '#define @ENUMPREFIX@_TYPE_@ENUMSHORT@ (@enum_name@_get_type())\nGType @enum_name@_get_type (void);\n', "--ftail", 'G_END_DECLS\n#endif\n']
                    with (generated / ("wnck-enum-types." + suffix)).open("w") as output:
                        subprocess.run([*map(str, command), *[str(source / "libwnck" / file) for file in headers]], stdout=output, check=True)
                install_headers["libwnck-3.0/libwnck/wnck-enum-types.h"] = generated / "wnck-enum-types.h"
                resources = generated / "wnck-resources.c"
                run(["glib-compile-resources", source / "libwnck/wnck.gresource.xml", "--sourcedir", source / "libwnck", "--generate-source", "--c-name", "wnck", "--target", resources])
                paths = [source / "libwnck" / file for group in ("sources", "a11y_sources") for file in meson_list(recipe, group) if file.endswith(".c")]
                paths += [generated / "wnck-enum-types.c", resources]
                options = ["-DWNCK_COMPILATION", "-DWNCK_I_KNOW_THIS_IS_UNSTABLE", "-DSN_API_NOT_YET_FROZEN=1", '-DWNCK_LOCALEDIR="/usr/share/locale"', '-DG_LOG_DOMAIN="Wnck"']
                name = "libwnck-3"
            for relative, file in install_headers.items():
                destination = install / "usr/include" / relative; destination.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(file, destination)
            (generated / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
            local = ["-I", generated, "-I", source, "-I", source / "src", "-I", source / "libsn", "-I", source / "libwnck"]
            jobs = [(file, generated / f"{index:02d}-{file.stem}.o") for index, file in enumerate(paths)]
            def compile_one(job):
                file, obj = job; run([*flags, *local, *options, "-c", file, "-o", obj])
                if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign native windowing object")
                if file.is_relative_to(generated):
                    label = "generated/" + project + "/" + file.name
                    retained = install / "build-metadata" / label; retained.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(file, retained)
                else:
                    label = str(file.relative_to(root))
                return {"source": label,
                        "source_sha256": digest(file), "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                records = list(pool.map(compile_one, jobs))
            archive = install / "usr/lib" / (name + ".a"); run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            public, imported, weak = symbols(archive)
            libraries[name] = {"sources": records, "exports": sorted(public), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak), "archive_sha256": digest(archive)}
            configurations[project] = {"values": values, "probes": tests}
            license = install / "usr/share/licenses" / project; license.mkdir(parents=True, exist_ok=True)
            for file in source.glob("COPYING*"):
                shutil.copyfile(file, license / file.name)
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install / "usr/libexec/xorg"):
                continue
            defined |= definitions(path); undefined |= symbols(path)[1]
        args.imports.write_text("\n".join(sorted(undefined - defined - {"___dso_handle"})) + "\n"); pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": libraries, "configuration": configurations,
            "bundle_sha256": digest(args.output), "runtime_checked": False, "desktop_checked": False,
            "generation": "Original release source inventories, pinned original GLib enum generator and genuine GResource compiler; no application build-system execution"}, indent=2) + "\n")
        print(f"Native Xfce windowing {args.arch}: {sum(len(v['sources']) for v in libraries.values())} Mach-O objects; real XCB utilities, startup notification and Wnck")

if __name__ == "__main__":
    main()
