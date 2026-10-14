#!/usr/bin/env python3
"""Build original ATK, AT-SPI, bridge and services as native Mach-O in Bazel."""
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

VERSION = "2.62.1"
# This release declares the old column-index API but has no implementation.
# Retain the original header; do not fabricate an exported success wrapper.
HEADER_ONLY_DECLARATIONS = {"libatspi": ["atspi_table_cell_get_column_index"]}


def source_inventory(source):
    def inventory(directory, variable):
        recipe = (source / directory / "meson.build").read_text()
        return [directory + "/" + name for name in meson_list(recipe, variable) if name.endswith(".c")]
    return {
        "libatk-1.0": inventory("atk", "atk_sources") + ["generated/atk/atk-enum-types.c", "generated/atk/atkmarshal.c"],
        "libatspi": inventory("atspi", "atspi_sources") + ["atspi/atspi-device-x11.c", "atspi/atspi-gmain.c"] +
                    inventory("dbind", "dbind_sources") + ["generated/atspi/atspi-enum-types.c", "generated/atspi/atspimarshal.c"],
        "libatk-bridge-2.0": inventory("atk-adaptor", "atk_bridge_sources") + inventory("atk-adaptor/adaptors", "source") +
                              inventory("droute", "droute_sources") + ["generated/introspection.c"],
        "at-spi-bus-launcher": ["bus/at-spi-bus-launcher.c"],
        "at-spi2-registryd": inventory("registryd", "registryd_sources") +
                            ["registryd/" + name for name in ("deviceeventcontroller-x11.c", "display.c", "event-source.c", "ucs2keysym.c")] +
                            ["generated/introspection.c"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "xml", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/accessibility/at-spi2-core" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-accessibility-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        merge_archive(args.xml, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("accessibility SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fno-common",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H",
                 "-D_XOPEN_SOURCE=700", "-DG_DISABLE_SINGLE_INCLUDES", "-DATK_DISABLE_SINGLE_INCLUDES",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-Wno-deprecated-declarations", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", install / "usr/include", "-I", install / "usr/include/glib-2.0",
                 "-I", install / "usr/include/dbus-1.0", "-I", install / "usr/include/libxml2"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        config = {"ATK_VERSION": '"' + VERSION + '"', "GETTEXT_PACKAGE": '"at-spi2-core"',
                  "ATSPI_LOCALEDIR": '"/usr/share/locale"', "LIBDIR": '"/usr/lib"',
                  "_ATK_EXTERN": '__attribute__((visibility("default"))) extern'}
        features = {}
        def probe(name, text):
            path = generated / "feature.c"
            path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if result.returncode:
                raise ValueError(name + ":\n" + result.stderr.decode())
        for name, text in {
            "HAVE_BIND_TEXTDOMAIN_CODESET": '#include <libintl.h>\nvoid *f=(void*)&bind_textdomain_codeset;\n',
            "HAVE_X11": '#include <X11/Xlib.h>\nvoid *f=(void*)&XOpenDisplay;\n',
            "HAVE_XKB": '#include <X11/Xlib.h>\n#include <X11/XKBlib.h>\nvoid *f=(void*)&XkbGetMap;\n',
            "HAVE_XRES": '#include <X11/Xlib.h>\n#include <X11/extensions/XRes.h>\nvoid *f=(void*)&XResQueryClientIds;\n',
            "HAVE_DLOPEN": '#include <dlfcn.h>\nvoid *f=(void*)&dlopen;\n',
        }.items():
            probe(name, text)
            if name != "HAVE_DLOPEN": config[name] = 1
        for name, ctype, alignment in (("CHAR", "char", 1), ("DOUBLE", "double", 8), ("DBUS_BOOL_T", "dbus_bool_t", 4),
                                       ("DBUS_INT16_T", "dbus_int16_t", 2), ("DBUS_INT32_T", "dbus_int32_t", 4),
                                       ("DBUS_INT64_T", "dbus_int64_t", 8), ("DBIND_POINTER", "void *", 8),
                                       ("DBIND_STRUCT", "struct { char s1; }", 1)):
            probe("ALIGNOF_" + name, '#include <dbus/dbus.h>\n_Static_assert(_Alignof(' + ctype + f') == {alignment}, "Darwin alignment");\n')
            config["ALIGNOF_" + name] = alignment
        (generated / "config.h").write_text("/* Native Darwin target declarations and original feature choices. */\n" +
            "".join(f"#define {name} {value}\n" for name, value in sorted(config.items())))
        glib = root / "src/Applications/glib/glib/2.90.1/gobject"
        for name in ("glib-mkenums", "glib-genmarshal"):
            (generated / name).write_text((glib / (name + ".in")).read_text().replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
        generated_headers = {}
        for library, include_path, version_header, enum_base, marshal_base, header_variable in (
            ("atk", "atk-1.0/atk", "atkversion.h", "atk-enum-types", "atkmarshal", "atk_headers"),
            ("atspi", "at-spi-2.0/atspi", "atspi-version.h", "atspi-enum-types", "atspimarshal", "atspi_headers")):
            output = generated / library
            output.mkdir()
            include = install / "usr/include" / include_path
            include.mkdir(parents=True)
            recipe = (source / library / "meson.build").read_text()
            headers = meson_list(recipe, header_variable) + (["atk.h"] if library == "atk" else ["atspi-device-x11.h"])
            for header in headers:
                shutil.copyfile(source / library / header, include / header)
            major, minor, micro = map(int, VERSION.split("."))
            values = {library.upper() + "_" + name: value for name, value in
                      (("MAJOR_VERSION", major), ("MINOR_VERSION", minor), ("MICRO_VERSION", micro))}
            if library == "atk": values.update(ATK_BINARY_AGE=10000 * major + 100 * minor + 10 + micro, ATK_INTERFACE_AGE=1)
            (include / version_header).write_text(re.sub(r"@(\w+)@", lambda m: str(values[m[1]]),
                                                        (source / library / (version_header + ".in")).read_text()))
            shutil.copyfile(include / version_header, output / version_header)
            enum_headers = meson_list(recipe, "atk_headers") if library == "atk" else ["atspi-constants.h", "atspi-types.h"]
            for suffix in ("c", "h"):
                with (output / (enum_base + "." + suffix)).open("w") as stream:
                    subprocess.run(["python3", str(generated / "glib-mkenums"), "--template", str(source / library / (enum_base + "." + suffix + ".template")),
                                    *[str(source / library / header) for header in enum_headers]], stdout=stream, check=True)
                with (output / (marshal_base + "." + suffix)).open("w") as stream:
                    subprocess.run(["python3", str(generated / "glib-genmarshal"), "--prefix=" + library + "_marshal",
                                    "--header" if suffix == "h" else "--body", str(source / library / (marshal_base + ".list"))], stdout=stream, check=True)
            shutil.copyfile(output / (enum_base + ".h"), include / (enum_base + ".h"))
            for header in (version_header, enum_base + ".h"):
                generated_headers[str((include / header).relative_to(install))] = digest(include / header)
            # pragma-once upstream headers must come from one inode during
            # compilation. Installed copies are reserved for consumers.
            flags += ["-I", output, "-I", source / library]
        bridge_include = install / "usr/include/at-spi2-atk/2.0"
        bridge_include.mkdir(parents=True)
        shutil.copyfile(source / "atk-adaptor/atk-bridge.h", bridge_include / "atk-bridge.h")
        xml_recipe = (source / "xml/meson.build").read_text()
        xml_inputs = re.findall(r"'([^']+\.xml)'", xml_recipe)
        run(["python3", source / "xml/generate-introspection.py", *[source / "xml" / name for name in xml_inputs],
             "--c-output=" + str(generated / "introspection.c"), "--h-output=" + str(generated / "introspection.h")])
        flags += ["-I", generated, "-I", source, "-I", source / "atk", "-I", source / "atspi", "-I", source / "registryd",
                  "-I", source / "atk-adaptor", "-I", source / "atk-adaptor/adaptors", "-I", source / "droute"]
        inventories = source_inventory(source)
        libraries, programs = {}, {}
        for name, files in inventories.items():
            options = []
            if name == "libatk-1.0":
                options = ["-DATK_COMPILATION", "-DATK_DISABLE_DEPRECATION_WARNINGS", "-DGLIB_DISABLE_DEPRECATION_WARNINGS",
                           "-DG_LOG_USE_STRUCTURED=1", '-DG_LOG_DOMAIN="Atk"']
            if name == "libatspi": options = ['-DG_LOG_DOMAIN="atspi"']
            if name == "at-spi-bus-launcher": options = ['-DSYSCONFDIR="/etc"', '-DDATADIR="/usr/share"', '-DDBUS_DAEMON="/usr/bin/dbus-daemon"']
            jobs = [(generated / path.removeprefix("generated/") if path.startswith("generated/") else source / path,
                     generated / f"{name}_{index:03d}.o") for index, path in enumerate(files)]
            def compile_one(job):
                path, obj = job
                try: run([*flags, *options, "-c", path, "-o", obj])
                except RuntimeError as error: return {"error": str(error)}
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("non-native accessibility object")
                return {"source": "generated/" + str(path.relative_to(generated)) if path.is_relative_to(generated) else str(path.relative_to(root)),
                        "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                records = list(pool.map(compile_one, jobs))
            errors = [entry["error"] for entry in records if "error" in entry]
            if errors: raise ValueError("native accessibility compilation failed:\n" + "\n".join(errors))
            archive = install / ("usr/lib/" + name + ".a" if name.startswith("lib") else "usr/libexec/accessibility/" + name + ".a")
            archive.parent.mkdir(parents=True, exist_ok=True)
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            exported, imported, weak = symbols(archive)
            record = {"sources": records, "exports": sorted(exported), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak),
                      "archive_sha256": digest(archive), "compiler_options": options}
            (libraries if name.startswith("lib") else programs)[name] = record
        configurations = [
            ("bus/accessibility.conf.in", "usr/share/defaults/at-spi2/accessibility.conf"),
            ("bus/org.a11y.Bus.service.in", "usr/share/dbus-1/services/org.a11y.Bus.service"),
            ("bus/at-spi-dbus-bus.desktop.in", "etc/xdg/autostart/at-spi-dbus-bus.desktop"),
            ("registryd/org.a11y.atspi.Registry.service.in", "usr/share/dbus-1/accessibility-services/org.a11y.atspi.Registry.service"),
        ]
        for original, destination in configurations:
            path = install / destination
            path.parent.mkdir(parents=True, exist_ok=True)
            values = {"DATADIR": "/usr/share", "SOCKET_ADDRESS": "unix:dir=/tmp", "libexecdir": "/usr/libexec"}
            path.write_text(re.sub(r"@(\w+)@", lambda m: values[m[1]], (source / original).read_text()))
        licenses = install / "usr/share/licenses/at-spi2-core"
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
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION, "libraries": libraries, "programs": programs,
            "configuration": config, "sdk_features": features, "system_requests": requests, "generated_headers": generated_headers,
            "introspection_inputs": xml_inputs, "bundle_sha256": digest(args.output), "runtime_checked": False,
            "upstream_header_only_declarations": HEADER_ONLY_DECLARATIONS,
            "disabled_optional_backends": ["dbus-glib (original atspi-gmain backend used)", "systemd/dbus-broker", "GTK2 module", "GIR tooling", "Python bindings"]}, indent=2) + "\n")
        print(f"Native accessibility {args.arch}: {sum(len(r['sources']) for r in [*libraries.values(), *programs.values()])} Mach-O objects, three libraries and two services")


if __name__ == "__main__": main()
