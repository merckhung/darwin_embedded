#!/usr/bin/env python3
"""Build original Xfce panel/plugins, desktop/file icons and session programs."""
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

RELEASES = {"iceauth": "1.0.11", "libyaml": "0.2.5", "xfce4-panel": "4.20.8", "xfce4-session": "4.20.4", "xfdesktop": "4.20.2"}

def inventory(recipe, variable, enabled):
    """Select original conditional inventories and expand their list references."""
    variables = {}; stack = [True]
    for line in recipe.read_text().replace("\\\n", " ").splitlines():
        if line.startswith("if "):
            condition = line[3:].strip(); inverse = condition.startswith("!")
            value = enabled.get(condition.lstrip("!"), False)
            stack.append(stack[-1] and (not value if inverse else value)); continue
        if line == "else":
            stack[-1] = stack[-2] and not stack[-1]; continue
        if line.startswith("endif"):
            stack.pop(); continue
        if stack[-1]:
            match = re.match(r"(\w+)\s*(\+?=)\s*(.*?)\s*(?:#.*)?$", line)
            if match:
                if match[2] == "=":
                    variables[match[1]] = match[3].split()
                else:
                    variables.setdefault(match[1], []).extend(match[3].split())
    if variable not in variables:
        raise ValueError("missing original inventory: " + str(recipe) + ": " + variable)
    def expand(name, parents):
        if name in parents:
            raise ValueError("cyclic original source inventory")
        result = []
        # Undefined variables in Automake expand to an empty list, including
        # source groups guarded by an unselected optional dependency.
        for token in variables.get(name, []):
            if token == "$(NULL)":
                continue
            match = re.fullmatch(r"\$\((\w+)\)", token)
            result += expand(match[1], [*parents, name]) if match else [token]
        return result
    return list(dict.fromkeys(expand(variable, [])))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xfce-desktop-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, install = work / "sdk", work / "install"
        os.environ["TMPDIR"] = str(work); os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        for path, target in ((args.sdk, sdk), (args.dependencies, install)):
            with tarfile.open(path) as archive:
                archive.extractall(target, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("desktop SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_FORTIFY_SOURCE=0", "-DHAVE_CONFIG_H",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability", "-isystem", resource + "/include",
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        for include in ("", "glib-2.0", "gio-unix-2.0", "gtk-3.0", "pango-1.0", "gdk-pixbuf-2.0", "atk-1.0", "cairo", "harfbuzz", "freetype2",
                        "startup-notification-1.0", "libwnck-3.0", "xfce4", "xfce4/xfconf-0", "xfce4/libxfce4ui-2", "xfce4/libxfce4kbd-private-3", "xfce4/libxfce4panel-2.0", "exo-2", "garcon-1", "garcon-gtk3-1"):
            flags += ["-I", install / "usr/include" / include]
        libraries, programs, modules, configurations, data_records = {}, {}, {}, {}, {}
        enabled = {"ENABLE_X11": True, "ENABLE_DESKTOP_ICONS": True, "ENABLE_FILE_ICONS": True, "BUILD_DESKTOP_MENU": True}
        for project, version in RELEASES.items():
            source = root / "src/Applications" / ("xfce-prerequisites" if project in ("iceauth", "libyaml") else "xfce") / project / version
            manifest = json.loads((source / "UPSTREAM.json").read_text())
            for name, expected in manifest["upstream_files_sha256"].items():
                if digest(source / name) != expected:
                    raise ValueError("modified original desktop source: " + name)
            generated = work / project; generated.mkdir()
            values = {"PACKAGE": json.dumps(project), "PACKAGE_NAME": json.dumps(project), "PACKAGE_VERSION": json.dumps(version),
                      "PACKAGE_BUGREPORT": json.dumps("https://gitlab.xfce.org/xfce/" + project + "/-/issues"),
                      "PACKAGE_STRING": json.dumps(project + " " + version), "VERSION": json.dumps(version), "VERSION_FULL": json.dumps(version), "VERSION_SHORT": '"4.20"',
                      "GETTEXT_PACKAGE": json.dumps(project), "ENABLE_NLS": 1, "STDC_HEADERS": 1, "COPYRIGHT_YEAR": '"2026"',
                      "PACKAGE_LOCALE_DIR": '"/usr/share/locale"', "LOCALEDIR": '"/usr/share/locale"', "DATADIR": '"/usr/share"', "LIBDIR": '"/usr/lib"',
                      "PREFIX": '"/usr"', "SYSCONFDIR": '"/etc"', "XDGCONFIGDIR": '"/etc/xdg"', "BINDIR": '"/usr/bin"',
                      "BACKGROUNDS_DIR": '"/usr/share/backgrounds/xfce"', "DEFAULT_BACKDROP": '"/usr/share/backgrounds/xfce/xfce-blue.jpg"',
                      "XFCEMODDIR": '"/usr/lib/xfce4/modules"', "ICEAUTH_CMD": '"/usr/bin/iceauth"',
                      "XFSM_SHUTDOWN_HELPER_CMD": '"/usr/libexec/xfce4/session/xfsm-shutdown-helper"'}
            if project not in ("iceauth", "libyaml"):
                values.update(ENABLE_X11=1, HAVE_GDK_X11=1, HAVE_GTK_X11=1, HAVE_LIBX11=1, HAVE_LIBWNCK=1)
            tests = {}
            def probe(key, code):
                file = generated / "probe.c"; file.write_text(code)
                result = subprocess.run([*map(str, flags), "-Werror", "-c", str(file), "-o", str(generated / "probe.o")], capture_output=True)
                tests[key] = result.returncode == 0
                if not result.returncode:
                    values[key] = 1
            for header in ("arpa/inet.h", "dlfcn.h", "errno.h", "fcntl.h", "inttypes.h", "libintl.h", "limits.h", "locale.h", "math.h", "memory.h", "netdb.h", "netinet/in.h", "pwd.h", "signal.h", "stdarg.h", "stdio.h", "stdlib.h", "strings.h", "string.h", "sys/param.h", "sys/resource.h", "sys/socket.h", "sys/stat.h", "sys/sysctl.h", "sys/time.h", "sys/types.h", "sys/utsname.h", "sys/wait.h", "time.h", "unistd.h", "wchar.h"):
                probe("HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()), '#include <' + header + '>\n')
            for key, header, function in [("HAVE_GETADDRINFO", "netdb.h", "getaddrinfo"), ("HAVE_GETNAMEINFO", "netdb.h", "getnameinfo"),
                ("HAVE_STRDUP", "string.h", "strdup"), ("HAVE_SIGPROCMASK", "signal.h", "sigprocmask"), ("HAVE_SETSID", "unistd.h", "setsid"),
                ("HAVE_BIND_TEXTDOMAIN_CODESET", "libintl.h", "bind_textdomain_codeset")]:
                probe(key, '#include <' + header + '>\nvoid *p=(void*)&' + function + ';\n')
            if project == "libyaml":
                values.update(YAML_VERSION_MAJOR=0, YAML_VERSION_MINOR=2, YAML_VERSION_PATCH=5, YAML_VERSION_STRING='"0.2.5"')
                shutil.copyfile(source / "include/yaml.h", install / "usr/include/yaml.h")
            elif project == "xfce4-panel":
                values.update(LIBXFCE4PANEL_VERSION_API='"2.0"', XFCE4_PANEL_CONFIG_VERSION=2, HELPERDIR='"/usr/libexec/xfce4/panel"', HELPDIR='"/usr/share/doc/xfce4-panel/html"')
                include = install / "usr/include/xfce4/libxfce4panel-2.0/libxfce4panel"; include.mkdir(parents=True, exist_ok=True)
                replacement = {"LIBXFCE4PANEL_VERSION": version, **{"LIBXFCE4PANEL_VERSION_" + part: number for part, number in zip(("MAJOR", "MINOR", "MICRO"), version.split("."), strict=True)}}
                (generated / "libxfce4panel").mkdir()
                (generated / "libxfce4panel/libxfce4panel-config.h").write_text(re.sub(r"@(\w+)@", lambda m: replacement[m[1]], (source / "libxfce4panel/libxfce4panel-config.h.in").read_text()))
                for header in inventory(source / "libxfce4panel/Makefile.am", "libxfce4panel_include_HEADERS", enabled):
                    file = generated / "libxfce4panel" / header if (generated / "libxfce4panel" / header).is_file() else source / "libxfce4panel" / header
                    shutil.copyfile(file, include / header)
            elif project == "xfdesktop":
                values.update(ENABLE_DESKTOP_ICONS=1, ENABLE_FILE_ICONS=1, BUILD_DESKTOP_MENU=1, HAVE_LIBYAML=1)
                generator = root / "src/Applications/glib/glib/2.90.1/gio/gdbus-2.0/codegen/gdbus-codegen.in"
                codegen = generated / "codegen"; shutil.copytree(generator.parent, codegen)
                (codegen / "config.py").write_text((generator.parent / "config.py.in").read_text().replace("@VERSION@", "2.90.1").replace("@MAJOR_VERSION@", "2").replace("@MINOR_VERSION@", "90"))
                command = codegen / "gdbus-codegen"; command.write_text(generator.read_text().replace("@PYTHON@", "python3"))
                environment = dict(os.environ, PYTHONPATH=str(generated))
                (generated / "src").mkdir()
                for name, prefix in [("file-manager-fdo", "org.freedesktop."), ("file-manager", "org.xfce."), ("thunar", "org.xfce."), ("trash", "org.xfce.")]:
                    subprocess.run(["python3", str(command), "--c-namespace=Xfdesktop", "--interface-prefix=" + prefix,
                        "--generate-c-code=" + str(generated / "src" / ("xfdesktop-" + name + "-proxy")), str(source / "src" / ("xfdesktop-" + name + "-dbus.xml"))], env=environment, check=True, capture_output=True)
                for subdir, stem in [("src", "xfdesktop-monitor-chooser"), ("settings", "xfdesktop-settings")]:
                    (generated / subdir).mkdir(exist_ok=True)
                    for kind, suffix in [("header", ".h"), ("source", ".c")]:
                        run(["glib-compile-resources", "--generate-" + kind, "--manual-register", "--sourcedir=" + str(source / subdir),
                             "--target=" + str(generated / subdir / (stem + "-ui" + suffix)), source / subdir / (stem + "-ui.gresource.xml")])
            elif project == "xfce4-session":
                # Match upstream AC_CHECK_FUNCS against the actual native ICE
                # archive rather than treating an uninstalled private header
                # as evidence that its transport function does not exist.
                key = "HAVE__ICETRANSNOLISTEN"; tests[key] = "__IceTransNoListen" in symbols(install / "usr/lib/libICE.a")[0]
                if tests[key]:
                    values[key] = 1
            (generated / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
            configurations[project] = {"values": values, "probes": tests}
            def paths(subdir, variable):
                result = []
                for token in inventory(source / subdir / "Makefile.am", variable, enabled):
                    if not token.endswith(".c"):
                        continue
                    if token.startswith("$(top_srcdir)/") or token.startswith("$(top_builddir)/"):
                        token = token.split("/", 1)[1]; file = source / token; compiled = generated / token
                    else:
                        file = source / subdir / token; compiled = generated / subdir / token
                    if compiled.is_file():
                        file = compiled
                    if not file.is_file():
                        raise ValueError("missing original/generated desktop source: " + str(file))
                    result.append(file)
                return list(dict.fromkeys(result))
            def build(name, files, category, subdir, options=(), path=None):
                localdir = generated / subdir; localdir.mkdir(parents=True, exist_ok=True)
                local = ["-I", generated, "-I", source, "-I", localdir, "-I", source / subdir, "-I", generated / "common", "-I", source / "common", "-I", source / "src", "-I", source / "libxfsm"]
                if project == "libyaml":
                    local += ["-I", source / "include"]
                if project == "xfce4-panel":
                    local += ["-DLIBXFCE4PANEL_COMPILATION"]
                if project == "xfdesktop":
                    local += ["-DDEFAULT_BACKDROP=" + values["DEFAULT_BACKDROP"]]
                local += list(options)
                local += ["-DWNCK_I_KNOW_THIS_IS_UNSTABLE"]
                local += ["-DG_LOG_DOMAIN=" + json.dumps(project if category == "programs" else name)]
                jobs = [(file, localdir / (name + f"-{index:02d}.o")) for index, file in enumerate(files)]
                def compile_one(job):
                    file, obj = job
                    try:
                        # Original tasklist assumes a transitive math.h include.
                        extra = ["-include", sdk / "usr/include/math.h"] if file.name == "tasklist-widget.c" else []
                        run([*flags, *local, *extra, "-c", file, "-o", obj])
                    except RuntimeError as error:
                        return {"error": str(error)}
                    if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                        raise ValueError("foreign native desktop object")
                    compiled = file.is_relative_to(generated); relative = str(file.relative_to(generated)) if compiled else str(file.relative_to(root))
                    if compiled:
                        saved = install / "build-metadata/generated" / project / relative; saved.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(file, saved)
                    return {"source": relative, "generated": compiled, "source_sha256": digest(file), "member": obj.name, "object_sha256": digest(obj)}
                with ThreadPoolExecutor(max_workers=8) as pool:
                    records = list(pool.map(compile_one, jobs))
                errors = [record["error"] for record in records if "error" in record]
                if errors:
                    raise ValueError("native desktop compilation failed:\n" + "\n".join(errors))
                archive = install / ("usr/lib" if category == "libraries" else "usr/libexec/xfce") / (name + ".a"); archive.parent.mkdir(parents=True, exist_ok=True)
                run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
                public, imported, weak = symbols(archive)
                record = {"sources": records, "exports": sorted(public), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak), "archive_sha256": digest(archive), "archive_path": str(archive.relative_to(install))}
                if path:
                    record["install_path"] = path
                {"libraries": libraries, "programs": programs, "modules": modules}[category][name] = record
            if project == "iceauth":
                build("iceauth", paths(".", "iceauth_SOURCES"), "programs", ".", path="/usr/bin/iceauth")
            elif project == "libyaml":
                build("libyaml-0", paths("src", "libyaml_la_SOURCES"), "libraries", "src")
            elif project == "xfce4-panel":
                common = paths("common", "libpanel_common_la_SOURCES")
                build("libxfce4panel-2.0", paths("libxfce4panel", "libxfce4panel_2_0_la_SOURCES"), "libraries", "libxfce4panel")
                for name, subdir, variable, path in [("xfce4-panel", "panel", "xfce4_panel_SOURCES", "/usr/bin/xfce4-panel"),
                    ("wrapper-2.0", "wrapper", "wrapper_2_0_SOURCES", "/usr/libexec/xfce4/panel/wrapper-2.0"),
                    ("panel-migrate", "migrate", "migrate_SOURCES", "/usr/libexec/xfce4/panel/migrate")]:
                    build(name, paths(subdir, variable) + common, "programs", subdir, path=path)
                for plugin in inventory(source / "plugins/Makefile.am", "SUBDIRS", enabled):
                    name = "lib" + plugin
                    build(name, paths("plugins/" + plugin, name + "_la_SOURCES") + common, "modules", "plugins/" + plugin, path="/usr/lib/xfce4/panel/plugins/" + name + ".so")
            elif project == "xfce4-session":
                common = paths("libxfsm", "libxfsm_la_SOURCES")
                for name, subdir, variable, path in [("xfce4-session", "xfce4-session", "xfce4_session_SOURCES", "/usr/bin/xfce4-session"),
                    ("xfce4-session-settings", "settings", "xfce4_session_settings_SOURCES", "/usr/bin/xfce4-session-settings"),
                    ("xfce4-session-logout", "xfce4-session-logout", "xfce4_session_logout_SOURCES", "/usr/bin/xfce4-session-logout"),
                    ("xfsm-shutdown-helper", "xfsm-shutdown-helper", "xfsm_shutdown_helper_SOURCES", "/usr/libexec/xfce4/session/xfsm-shutdown-helper")]:
                    build(name, paths(subdir, variable) + common, "programs", subdir, path=path)
            else:
                common = paths("common", "libxfdesktop_la_SOURCES")
                for name, subdir, variable in [("xfdesktop", "src", "xfdesktop_SOURCES"), ("xfdesktop-settings", "settings", "xfdesktop_settings_SOURCES")]:
                    files = paths(subdir, variable) + paths(subdir, "nodist_" + variable) + common
                    build(name, list(dict.fromkeys(files)), "programs", subdir, path="/usr/bin/" + name)
            license = install / "usr/share/licenses" / project; license.mkdir(parents=True, exist_ok=True)
            for file in [*source.glob("COPYING*"), *source.glob("LICENSE*")]:
                shutil.copyfile(file, license / file.name)
            def copy_data(file, destination, content=None):
                target = install / destination.lstrip("/"); target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(file.read_bytes() if content is None else content.encode())
                data_records[destination] = {"source": str(file.relative_to(root)), "source_sha256": digest(file), "sha256": digest(target)}
            replacements = {"LIBXFCE4PANEL_VERSION_API": "2.0", "VERSION_SHORT": "4.20", "VERSION_FULL": version,
                            "prefix": "/usr", "libdir": "/usr/lib", "bindir": "/usr/bin", "sysconfdir": "/etc", "datadir": "/usr/share", "libexecdir": "/usr/libexec"}
            def desktop_entry(file, destination):
                text = re.sub(r"^_(\w+)=", r"\1=", file.read_text(), flags=re.M)
                text = re.sub(r"@(\w+)@", lambda m: replacements[m[1]], text)
                copy_data(file, destination, text)
            if project == "xfce4-panel":
                copy_data(source / "migrate/default.xml", "/etc/xdg/xfce4/panel/default.xml")
                copy_data(source / "migrate/default.xml", "/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml")
                for file in (source / "plugins").glob("*/*.desktop.in.in"):
                    desktop_entry(file, "/usr/share/xfce4/panel/plugins/" + file.name.removesuffix(".in.in"))
            elif project == "xfce4-session":
                copy_data(source / "settings/xfce4-session.xml", "/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml")
            elif project == "xfdesktop":
                for file in inventory(source / "backgrounds/Makefile.am", "backgrounds_DATA", enabled):
                    copy_data(source / "backgrounds" / file, "/usr/share/backgrounds/xfce/" + file)
            for directory in (source / "icons", source / "pixmaps"):
                for recipe in directory.rglob("Makefile.am"):
                    text = recipe.read_text()
                    for prefix in re.findall(r"^(\w+)_DATA\s*=", text, re.M):
                        destination = inventory(recipe, prefix + "dir", enabled)[0].replace("$(datadir)", "/usr/share")
                        for file in inventory(recipe, prefix + "_DATA", enabled):
                            copy_data(recipe.parent / file, destination + "/" + Path(file).name)
            for file in source.glob("*/*.desktop.in"):
                # Plugin metadata uses its separate Xfce Panel destination above.
                desktop_entry(file, "/usr/share/applications/" + file.name.removesuffix(".in"))
        garcon = root / "src/Applications/xfce/garcon/4.20.0"
        garcon_manifest = json.loads((garcon / "UPSTREAM.json").read_text())
        for relative, expected in garcon_manifest["upstream_files_sha256"].items():
            if digest(garcon / relative) != expected:
                raise ValueError("modified original desktop menu data")
        copy_data(garcon / "data/xfce/xfce-applications.menu", "/etc/xdg/menus/xfce-applications.menu")
        for file in (garcon / "data/xfce").glob("*.desktop.in"):
            desktop_entry(file, "/usr/share/desktop-directories/" + file.name.removesuffix(".desktop.in") + ".directory")
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install / "usr/libexec/xorg"):
                continue
            defined |= definitions(path); undefined |= symbols(path)[1]
        args.imports.write_text("\n".join(sorted(undefined - defined - {"___dso_handle"})) + "\n"); pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": libraries, "programs": programs, "modules": modules,
            "configuration": configurations, "installed_data": data_records, "bundle_sha256": digest(args.output), "runtime_checked": False, "desktop_checked": False,
            "scope": "Original panel/plugins and wrappers, original Xfce session/ICE/SM and original desktop with genuine file icons; native compile only until guest verification."}, indent=2) + "\n")
        print(f"Native Xfce desktop {args.arch}: {sum(len(v['sources']) for category in [libraries, programs, modules] for v in category.values())} Mach-O objects")

if __name__ == "__main__":
    main()
