#!/usr/bin/env python3
"""Compile the original GTK3 X11/Unix inventories as native Mach-O in Bazel."""
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

from build_accessibility import merge_archive, run, symbols, definitions, digest, pack

VERSION = "3.24.52"


def meson_list(recipe, name):
    # Select the first assignment, including files(...). GDK later replaces
    # gdk_sources with an aggregate list; that is not the original C inventory.
    match = re.search(r"^\s*" + re.escape(name) + r"\s*=\s*(files\(|\[)", recipe, re.M)
    if not match: raise ValueError("missing original GTK inventory: " + name)
    begin = match.end() - 1
    stack, quote, comment = [], None, False
    for index in range(begin, len(recipe)):
        char = recipe[index]
        if comment:
            if char == "\n": comment = False
            continue
        if quote:
            if char == quote and recipe[index - 1] != "\\": quote = None
            continue
        if char == "#": comment = True
        elif char in "'\"": quote = char
        elif char in "([": stack.append(char)
        elif char in ")]":
            stack.pop()
            if not stack: return re.findall(r"'([^']+)'", recipe[begin + 1:index])
    raise ValueError("unterminated original GTK inventory: " + name)


def inventory(source, directory, variable):
    return [directory + "/" + name for name in meson_list((source / directory / "meson.build").read_text(), variable)]


def source_inventory(source):
    xim = re.search(r"\[\s*'xim',\s*files\(\[(.*?)\]\)", (source / "meson.build").read_text(), re.S)
    if not xim: raise ValueError("original XIM inventory missing")
    xim_sources = ["modules/input/" + name for name in re.findall(r"'([^']+\.c)'", xim[1])]
    return {
        "libgdk-3": inventory(source, "gdk", "gdk_sources") + inventory(source, "gdk", "deprecated_gdk_sources") +
                    inventory(source, "gdk/x11", "gdk_x11_sources") +
                    ["generated/gdk/" + name for name in ("gdkenumtypes.c", "gdkmarshalers.c", "gdkresources.c")],
        "libgtk-3": inventory(source, "gtk", "gtk_sources") + inventory(source, "gtk", "gtk_unix_sources") + ["gtk/filetransferportal.c"] +
                    inventory(source, "gtk", "gtk_use_x11_sources") + inventory(source, "gtk", "gtk_use_wayland_or_x11_sources") +
                    ["gtk/gtkclipboard.c", "gtk/gtkdnd.c", "gtk/gtkdragdest.c"] +
                    inventory(source, "gtk/deprecated", "gtk_deprecated_sources") + inventory(source, "gtk/a11y", "a11y_sources") +
                    inventory(source, "gtk/inspector", "inspector_sources") +
                    ["generated/gtk/" + name for name in ("gtkdbusgenerated.c", "gtkmarshalers.c", "gtkprivatetypebuiltins.c", "gtktypebuiltins.c", "gtkresources.c")] + xim_sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "epoxy", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/widgets/gtk" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-gtk-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        for path in (generated, generated / "gdk", generated / "gtk"): path.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.sdk) as archive: archive.extractall(sdk, filter="data")
        merge_archive(args.epoxy, install)
        # GLib installs this source file as a public include. The earlier
        # header-only SDK inventory omitted it; preserve its original bytes.
        notify_source = root / "src/Applications/glib/glib/2.90.1/gobject/gobjectnotifyqueue.c"
        notify_header = install / "usr/include/glib-2.0/gobject/gobjectnotifyqueue.c"
        shutil.copyfile(notify_source, notify_header)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch: raise ValueError("GTK SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fno-common",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_GNU_SOURCE=1", "-D_FORTIFY_SOURCE=0",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability", "-Wno-deprecated-declarations",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include", "-I", install / "usr/include"]
        for include in ("glib-2.0", "dbus-1.0", "libxml2", "atk-1.0", "at-spi-2.0", "at-spi2-atk/2.0", "cairo", "pango-1.0",
                        "gdk-pixbuf-2.0", "harfbuzz", "freetype2", "fribidi"):
            flags += ["-I", install / "usr/include" / include]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        root_recipe = (source / "meson.build").read_text()
        config = {"PACKAGE_VERSION": '"' + VERSION + '"', "GETTEXT_PACKAGE": '"gtk30"', "ENABLE_NLS": 1,
                  "PACKAGE_NAME": '"gtk+"', "PACKAGE_TARNAME": '"gtk+"', "PACKAGE_STRING": '"gtk+ ' + VERSION + '"',
                  "PACKAGE_URL": '""', "PACKAGE_BUGREPORT": '"http://bugzilla.gnome.org/enter_bug.cgi?product=gtk%2B"',
                  "GTK_LOCALEDIR": '"/usr/share/locale"', "GTK_DATADIR": '"/usr/share"', "GTK_LIBDIR": '"/usr/lib"', "GTK_SYSCONFDIR": '"/etc"',
                  "GLIB_DISABLE_DEPRECATION_WARNINGS": 1, "HAVE_GIO_UNIX": 1, "HAVE_HARFBUZZ": 1, "HAVE_PANGOFT": 1,
                  "ISO_CODES_PREFIX": '"/usr"', "GTK_PRINT_BACKENDS": '"file"'}
        features = {}
        def probe(name, text, required=False):
            path = generated / "feature.c"; path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode: raise ValueError(name + ":\n" + result.stderr.decode())
            if not result.returncode: config[name] = 1
        for header in meson_list(root_recipe, "check_headers"):
            probe("HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()), "#include <" + header + ">\n")
        headers = "".join("#include <" + h + ">\n" for h in ("unistd.h", "sys/stat.h", "sys/mman.h", "math.h", "stdio.h", "time.h", "stdlib.h", "libintl.h"))
        for name in meson_list(root_recipe, "check_functions"):
            probe("HAVE_" + name.upper(), headers + "void *p=(void*)&" + name + ";\n")
        for name in ("isinf", "isnan"):
            probe("HAVE_DECL_" + name.upper(), "#include <math.h>\nint f(double x){return " + name + "(x);}\n", required=True)
        for name, includes, symbol in (
            ("HAVE_BIND_TEXTDOMAIN_CODESET", "libintl.h", "bind_textdomain_codeset"),
            ("HAVE_XDAMAGE", "X11/extensions/Xdamage.h", "XDamageCreate"),
            ("HAVE_XCURSOR", "X11/Xcursor/Xcursor.h", "XcursorLibraryLoadCursor"),
            ("HAVE_XCOMPOSITE", "X11/extensions/Xcomposite.h", "XCompositeRedirectWindow"),
            ("HAVE_XFIXES", "X11/extensions/Xfixes.h", "XFixesQueryExtension"),
            ("HAVE_XKB", "X11/XKBlib.h", "XkbQueryExtension"),
            ("HAVE_XSYNC", "X11/extensions/sync.h", "XSyncQueryExtension"),
            ("HAVE_XGENERICEVENTS", "X11/Xlib.h", "XGetEventData"),
            ("XINPUT_2", "X11/extensions/XInput2.h", "XIQueryVersion"),
            ("XINPUT_2_2", "X11/extensions/XInput2.h", "XIAllowTouchEvents"),
            ("HAVE_XFREE_XINERAMA", "X11/extensions/Xinerama.h", "XineramaQueryExtension"),
            ("HAVE_RANDR", "X11/extensions/Xrandr.h", "XRRQueryExtension"),
            ("HAVE_RANDR15", "X11/extensions/Xrandr.h", "XRRGetMonitors"),
        ):
            probe(name, '#include <X11/Xlib.h>\n#include <' + includes + '>\nvoid *p=(void*)&' + symbol + ';\n', required=True)
        config["HAVE_XINERAMA"] = 1
        probe("XINPUT_2_4", '#include <X11/Xlib.h>\n#include <X11/extensions/XInput2.h>\nint f(XIGesturePinchEvent *p){return p->type;}\n', required=True)
        for name in ("_NL_MEASUREMENT_MEASUREMENT", "_NL_PAPER_HEIGHT", "_NL_PAPER_WIDTH", "_NL_TIME_FIRST_WEEKDAY"):
            probe("HAVE_" + name, '#include <langinfo.h>\nint f(void){return ' + name + ';}\n')
        (generated / "config.h").write_text("/* Native Darwin target probes; GTK X11/Unix. */\n" +
            "".join(f"#define {name} {value}\n" for name, value in sorted(config.items())))
        gtk_recipe, gdk_recipe = (source / "gtk/meson.build").read_text(), (source / "gdk/meson.build").read_text()
        groups = {
            "gdk": inventory(source, "gdk", "gdk_gir_public_headers") + inventory(source, "gdk", "gdk_nogir_public_headers"),
            "gtk": inventory(source, "gtk", "gtk_gir_public_headers") + inventory(source, "gtk", "gtk_nogir_public_headers"),
            "deprecated": inventory(source, "gtk/deprecated", "gtk_deprecated_headers"),
            "a11y": inventory(source, "gtk/a11y", "a11y_headers"),
        }
        # Install public release headers while compiling against original
        # source inodes, keeping generated headers in a separate tree.
        public = [*groups["gdk"], *groups["gtk"], *groups["deprecated"], *groups["a11y"],
                  *inventory(source, "gdk/x11", "gdk_x11_public_headers"), "gdk/x11/gdkx.h", "gdk/deprecated/gdkcolor.h", "gtk/gtktextlayout.h"]
        for name in public:
            relative = "gdk/gdkx.h" if name == "gdk/x11/gdkx.h" else name
            destination = install / "usr/include/gtk-3.0" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, destination)
        for name in meson_list(gtk_recipe, "gtk_unix_print_headers"):
            path = install / "usr/include/gtk-3.0/unix-print/gtk" / name; path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / "gtk" / name, path)
        major, minor, micro = map(int, VERSION.split("."))
        age = int(re.search(r"^gtk_interface_age\s*=\s*(\d+)$", root_recipe, re.M)[1])
        values = {"GTK_MAJOR_VERSION": major, "GTK_MINOR_VERSION": minor, "GTK_MICRO_VERSION": micro, "GTK_BINARY_AGE": 100 * minor + micro,
                  "GTK_INTERFACE_AGE": age, "GTK_API_VERSION": "3.0", "GTK_BINARY_VERSION": "3.0.0", "GTK_VERSION": VERSION, "LT_CURRENT_MINUS_AGE": 0}
        for directory, name in (("gtk", "gtkversion.h"), ("gdk", "gdkversionmacros.h")):
            template = (source / directory / (name + ".in")).read_text()
            (generated / directory / name).write_text(re.sub(r"@(\w+)@", lambda m: str(values[m[1]]), template))
        template = (source / "gdk/gdkconfig.h.meson").read_text()
        (generated / "gdk/gdkconfig.h").write_text(re.sub(r"^#mesondefine (\w+)$", lambda m:
            "#define " + m[1] + " 1" if m[1] == "GDK_WINDOWING_X11" else "/* #undef " + m[1] + " */", template, flags=re.M))
        glib = root / "src/Applications/glib/glib/2.90.1"
        for name in ("glib-mkenums", "glib-genmarshal"):
            (generated / name).write_text((glib / "gobject" / (name + ".in")).read_text().replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
        for directory, enum_base, headers_for_enum in (("gdk", "gdkenumtypes", groups["gdk"]),
            ("gtk", "gtktypebuiltins", groups["gtk"] + groups["deprecated"] + groups["a11y"]),
            ("gtk", "gtkprivatetypebuiltins", inventory(source, "gtk", "gtk_private_type_headers"))):
            for suffix in ("c", "h"):
                with (generated / directory / (enum_base + "." + suffix)).open("w") as stream:
                    subprocess.run(["python3", str(generated / "glib-mkenums"), "--template", str(source / directory / (enum_base + "." + suffix + ".template")),
                                    *[str(source / name) for name in headers_for_enum]], stdout=stream, check=True)
        for directory in ("gdk", "gtk"):
            base = directory + "marshalers"
            for suffix in ("c", "h"):
                with (generated / directory / (base + "." + suffix)).open("w") as stream:
                    subprocess.run(["python3", str(generated / "glib-genmarshal"), "--prefix=_" + directory + "_marshal", "--valist-marshallers",
                                    "--header" if suffix == "h" else "--body", str(source / directory / (base + ".list"))], stdout=stream, check=True)
            xml = generated / directory / (directory + ".gresource.xml")
            run(["python3", source / directory / ("gen-" + directory + "-gresources-xml.py"), source / directory, xml])
            for suffix in ("c", "h"):
                run(["glib-compile-resources", "--sourcedir=" + str(source / directory), "--c-name=_" + directory,
                     "--manual-register", "--generate-source" if suffix == "c" else "--generate-header",
                     "--target=" + str(generated / directory / (directory + "resources." + suffix)), xml])
        codegen_source = glib / "gio/gdbus-2.0/codegen"; codegen = generated / "codegen"
        shutil.copytree(codegen_source, codegen)
        replacements = {"PYTHON": "python3", "VERSION": "2.90.1", "MAJOR_VERSION": "2", "MINOR_VERSION": "90", "DATADIR": str(generated / "share")}
        for original, target in (("config.py.in", "config.py"), ("gdbus-codegen.in", "gdbus-codegen")):
            (codegen / target).write_text(re.sub(r"@(\w+)@", lambda m: replacements[m[1]], (codegen_source / original).read_text()))
        run(["python3", codegen / "gdbus-codegen", "--interface-prefix", "org.Gtk.", "--output-directory", generated / "gtk",
             "--generate-c-code", "gtkdbusgenerated", "--c-namespace", "_Gtk", source / "gtk/gtkdbusinterfaces.xml"])
        run(["python3", source / "gtk/gentypefuncs.py", generated / "gtk/gtktypefuncs.inc",
             *[source / name for name in groups["gdk"] + groups["gtk"] + groups["deprecated"]],
             generated / "gtk/gtktypebuiltins.h", generated / "gdk/gdkenumtypes.h"])
        for directory, names in (("gdk", ("gdkconfig.h", "gdkenumtypes.h", "gdkversionmacros.h")), ("gtk", ("gtkversion.h", "gtktypebuiltins.h"))):
            for name in names: shutil.copyfile(generated / directory / name, install / "usr/include/gtk-3.0" / directory / name)
        schemas = install / "usr/share/glib-2.0/schemas"; schemas.mkdir(parents=True, exist_ok=True)
        for name in meson_list(gtk_recipe, "gtk_settings_schemas"): shutil.copyfile(source / "gtk" / name, schemas / name)
        run(["glib-compile-schemas", "--strict", schemas])
        flags += ["-I", generated, "-I", generated / "gdk", "-I", generated / "gtk", "-I", source, "-I", source / "gdk", "-I", source / "gdk/x11", "-I", source / "gtk",
                  "-DHAVE_CONFIG_H", "-DGTK_COMPILATION", "-DGDK_COMPILATION", '-DGTK_VERSION="' + VERSION + '"']
        inventories = source_inventory(source); libraries = {}
        for name, files in inventories.items():
            options = ['-DG_LOG_DOMAIN="Gdk"'] if name == "libgdk-3" else ['-DG_LOG_DOMAIN="Gtk"', '-DGTK_BINARY_VERSION="3.0.0"',
                '-DGTK_HOST="darwin"', '-DGTK_DATA_PREFIX="/usr"', '-DGTK_LIBDIR="/usr/lib"', '-DGTK_LOCALEDIR="/usr/share/locale"',
                '-DGTK_DATADIR="/usr/share"', '-DGTK_SYSCONFDIR="/etc"', '-DX11_DATA_PREFIX="/usr"', "-DGTK_PRINT_BACKEND_ENABLE_UNSUPPORTED", "-DINCLUDE_IM_xim"]
            jobs = [(generated / path.removeprefix("generated/") if path.startswith("generated/") else source / path,
                     generated / f"{name}_{index:03d}.o") for index, path in enumerate(files)]
            def compile_one(job):
                path, obj = job
                source_options = ["-Wno-error=incompatible-pointer-types-discards-qualifiers"] if path.name in ("gdkscreen-x11.c", "gdkcairo.c") else []
                local_include = source / ("gdk" if name == "libgdk-3" else "gtk")
                try: run([*flags[:1], "-I", local_include, *flags[1:], *options, *source_options, "-c", path, "-o", obj])
                except RuntimeError as error: return {"error": str(error)}
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1): raise ValueError("non-native GTK object")
                return {"source": "generated/" + str(path.relative_to(generated)) if path.is_relative_to(generated) else str(path.relative_to(root)),
                        "source_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj), "compiler_options": source_options}
            with ThreadPoolExecutor(max_workers=8) as pool: records = list(pool.map(compile_one, jobs))
            errors = [entry["error"] for entry in records if "error" in entry]
            if errors: raise ValueError("native GTK compilation failed:\n" + "\n".join(errors))
            archive = install / "usr/lib" / (name + ".a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            exported, imported, weak = symbols(archive)
            libraries[name] = {"sources": records, "exports": sorted(exported), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak)}
        print_source = source / "modules/printbackends/file/gtkprintbackendfile.c"
        print_object = generated / "gtkprintbackendfile.o"
        run([*flags[:1], "-I", source / "gtk", *flags[1:], "-DGTK_DISABLE_DEPRECATION_WARNINGS", "-DGTK_PRINT_BACKEND_ENABLE_UNSUPPORTED",
             "-c", print_source, "-o", print_object])
        print_archive = install / "usr/libexec/gtk/printbackend-file.a"
        print_archive.parent.mkdir(parents=True, exist_ok=True)
        run(["llvm-ar-20", "--format=darwin", "rcsD", print_archive, print_object])
        modules = {"printbackend-file": {"sources": [{"source": str(print_source.relative_to(root)), "source_sha256": digest(print_source),
                   "member": print_object.name, "object_sha256": digest(print_object)}], "archive_sha256": digest(print_archive)}}
        licenses = install / "usr/share/licenses/gtk"; licenses.mkdir(parents=True)
        shutil.copyfile(source / "COPYING", licenses / "COPYING")
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            defined |= definitions(path); undefined |= symbols(path)[1]
        for path in (install / "usr/libexec").glob("*.o"): undefined |= symbols(path)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n"); pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION, "libraries": libraries, "modules": modules,
            "configuration": config, "sdk_features": features, "system_requests": requests, "bundle_sha256": digest(args.output),
            "generated_headers": {str(path.relative_to(install)): digest(path) for path in (install / "usr/include/gtk-3.0").rglob("*.h") if path.name in
                                  ("gtkversion.h", "gtktypebuiltins.h", "gdkconfig.h", "gdkenumtypes.h", "gdkversionmacros.h")},
            "host_resource_compiler": {"version": run(["glib-compile-resources", "--version"]).strip(), "schemas_version": run(["glib-compile-schemas", "--version"]).strip()},
            "installed_source_headers": {str(notify_header.relative_to(install)): {"source": str(notify_source.relative_to(root)), "sha256": digest(notify_source)}},
            "runtime_checked": False, "x11_backend": True, "quartz_backend": False, "wayland_backend": False,
            "disabled_optional_backends": ["CUPS backend", "Tracker3", "cloudproviders", "profiler", "GIR tooling", "Wayland/Quartz/Broadway/Windows"]}, indent=2) + "\n")
        print(f"Native GTK3 {args.arch}: {sum(len(r['sources']) for r in libraries.values())} original/generated Mach-O objects, X11 backend")


if __name__ == "__main__": main()
