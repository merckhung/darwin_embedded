#!/usr/bin/env python3
"""Compile the full original xfwm4 package and its shipped themes through Bazel."""
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

from build_base import inventory, run, symbols, digest, definitions, pack
from build_ui import enabled_inventory

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    source = root / "src/Applications/xfce/xfwm4/4.20.0"
    manifest = json.loads((source / "UPSTREAM.json").read_text())
    for relative, expected in manifest["upstream_files_sha256"].items():
        if digest(source / relative) != expected:
            raise ValueError("modified original xfwm4 source: " + relative)
    with tempfile.TemporaryDirectory(prefix=".native-xfwm4-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, install, generated = work / "sdk", work / "install", work / "generated"; generated.mkdir()
        os.environ["TMPDIR"] = str(work)
        for path, target in ((args.sdk, sdk), (args.dependencies, install)):
            with tarfile.open(path) as archive:
                archive.extractall(target, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("xfwm4 SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_FORTIFY_SOURCE=0",
                 "-DHAVE_CONFIG_H", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-isystem", resource + "/include", "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include", "-I", generated, "-I", source]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        for include in ("", "glib-2.0", "gtk-3.0", "pango-1.0", "gdk-pixbuf-2.0", "atk-1.0", "cairo", "harfbuzz", "freetype2",
                        "startup-notification-1.0", "libwnck-3.0", "xfce4", "xfce4/xfconf-0", "xfce4/libxfce4ui-2", "xfce4/libxfce4kbd-private-3"):
            flags += ["-I", install / "usr/include" / include]
        values = {"PACKAGE": '"xfwm4"', "PACKAGE_NAME": '"xfwm4"', "PACKAGE_VERSION": '"4.20.0"', "VERSION": '"4.20.0"',
                  "PACKAGE_STRING": '"xfwm4 4.20.0"', "PACKAGE_BUGREPORT": '"https://gitlab.xfce.org/xfce/xfwm4/-/issues"',
                  "GETTEXT_PACKAGE": '"xfwm4"', "ENABLE_NLS": 1, "STDC_HEADERS": 1, "REVISION": '"unknown"',
                  "HAVE_XI2": 1, "HAVE_XRES": 1, "HAVE_XSYNC": 1, "HAVE_RENDER": 1, "HAVE_RANDR": 1,
                  "HAVE_COMPOSITOR": 1,
                  "HAVE_LIBSTARTUP_NOTIFICATION": 1, "DATADIR": '"/usr/share"', "PACKAGE_DATADIR": '"/usr/share/xfwm4"',
                  "PACKAGE_LOCALE_DIR": '"/usr/share/locale"', "LOCALEDIR": '"/usr/share/locale"', "HELPERDIR": '"/usr/libexec"', "SRCDIR": '"/usr/share/xfwm4"'}
        probes = {}
        for header in ("errno.h", "fcntl.h", "math.h", "memory.h", "signal.h", "stdarg.h", "stdio.h", "stdlib.h", "string.h", "sys/stat.h", "sys/time.h", "sys/types.h", "sys/wait.h", "time.h", "unistd.h"):
            file = generated / "probe.c"; file.write_text('#include <' + header + '>\n')
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(file), "-o", str(generated / "probe.o")], capture_output=True)
            key = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper()); probes[key] = result.returncode == 0
            if not result.returncode:
                values[key] = 1
        # The original configure recipe enables Present only when its client
        # library exists. This native dependency bundle has no libXpresent.
        probes["HAVE_PRESENT_EXTENSION"] = (install / "usr/lib/libXpresent.a").is_file() and (install / "usr/include/X11/extensions/Xpresent.h").is_file()
        if probes["HAVE_PRESENT_EXTENSION"]:
            values["HAVE_PRESENT_EXTENSION"] = 1
        (generated / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))
        adaptations = {}
        original = source / "src/settings.c"; text = original.read_text()
        before = '''    if (!xfconf_init (NULL))
    {
        g_critical ("Xfconf could not be initialized");
        return FALSE;
    }'''
        after = '''    GError *initialization_error = NULL;
    if (!xfconf_init (&initialization_error))
    {
        g_critical ("Xfconf could not be initialized: %s", initialization_error ? initialization_error->message : "unknown error");
        g_clear_error (&initialization_error);
        return FALSE;
    }'''
        if text.count(before) != 1:
            raise ValueError("original xfwm4 configuration initialization changed")
        compiled = generated / "settings.c"; compiled.write_text(text.replace(before, after)); adaptations[original] = compiled
        original = source / "src/compositor.c"; text = original.read_text()
        before = '''    TRACE ("entering compositorManageScreen");

    if (compositorCheckCMSelection (screen_info))'''
        after = '''    TRACE ("entering compositorManageScreen");

    /* Initial Xfconf notifications can activate this same compositor before
     * the main initialization pass. Preserve its actual active state. */
    if (screen_info->compositor_active)
    {
        return TRUE;
    }

    if (compositorCheckCMSelection (screen_info))'''
        if text.count(before) != 1:
            raise ValueError("original xfwm4 compositor initialization changed")
        compiled = generated / "compositor.c"; compiled.write_text(text.replace(before, after)); adaptations[original] = compiled
        common = [source / "common" / file for file in inventory(source / "common/Makefile.am", "libxfwm_common_la_SOURCES") if file.endswith(".c")]
        inventories = {
            "xfwm4": ("src", "xfwm4_SOURCES", common, '-DG_LOG_DOMAIN="xfwm4"'),
            "helper-dialog": ("helper-dialog", "helper_dialog_SOURCES", [], None),
            "xfwm4-settings": ("settings-dialogs", "xfwm4_settings_SOURCES", common, None),
            "xfwm4-tweaks-settings": ("settings-dialogs", "xfwm4_tweaks_settings_SOURCES", common, None),
            "xfwm4-workspace-settings": ("settings-dialogs", "xfwm4_workspace_settings_SOURCES", common, "-DWNCK_I_KNOW_THIS_IS_UNSTABLE"),
        }
        programs = {}
        for name, (directory, variable, extra, option) in inventories.items():
            paths = [source / directory / file for file in inventory(source / directory / "Makefile.am", variable) if file.endswith(".c")]
            paths += extra
            jobs = [(file, generated / f"{name}-{index:02d}.o") for index, file in enumerate(paths)]
            local = ["-I", source / directory, "-I", source / "common", "-DSN_API_NOT_YET_FROZEN="] + ([option] if option else [])
            def compile_one(job):
                file, obj = job
                # This original file assumes a transitive PATH_MAX declaration.
                # Select the actual Darwin SDK header without editing upstream.
                options = ["-include", sdk / "usr/include/limits.h"] if file == source / "src/main.c" else []
                try:
                    run([*flags, *local, *options, "-c", adaptations.get(file, file), "-o", obj])
                except RuntimeError as error:
                    return {"error": str(error)}
                if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign native window-manager object")
                return {"source": str(file.relative_to(root)), "source_sha256": digest(file), "member": obj.name, "object_sha256": digest(obj),
                        "compiler_options": ["-include", "DarwinSDK/usr/include/limits.h"] if options else [],
                        "compiled_sha256": digest(adaptations.get(file, file)),
                        "adaptation": "Report actual Xfconf errors" if file.name=="settings.c" else "Preserve an already active real compositor during repeated startup" if file.name=="compositor.c" else None}
            with ThreadPoolExecutor(max_workers=8) as pool:
                records = list(pool.map(compile_one, jobs))
            errors = [record["error"] for record in records if "error" in record]
            if errors:
                raise ValueError("native xfwm4 compilation failed:\n" + "\n".join(errors))
            archive = install / "usr/libexec/xfce" / (name + ".a"); archive.parent.mkdir(parents=True, exist_ok=True)
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            public, imported, weak = symbols(archive)
            programs[name] = {"sources": records, "exports": sorted(public), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak), "archive_sha256": digest(archive)}
        data_records = {}
        def copy_data(file, name):
            destination = install / name.lstrip("/"); destination.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(file, destination)
            data_records[name] = {"source": str(file.relative_to(root)), "sha256": digest(file)}
        copy_data(source / "defaults/defaults", "/usr/share/xfwm4/defaults")
        icons = root / "src/Applications/xfce-prerequisites/hicolor-icon-theme/0.18"
        icon_manifest = json.loads((icons / "UPSTREAM.json").read_text())
        for relative, expected in icon_manifest["upstream_files_sha256"].items():
            if digest(icons / relative) != expected:
                raise ValueError("modified original fallback icon theme")
        copy_data(icons / "index.theme", "/usr/share/icons/hicolor/index.theme")
        copy_data(icons / "COPYING", "/usr/share/licenses/hicolor-icon-theme/COPYING")
        for directory in (source / "icons").iterdir():
            if directory.is_dir():
                recipe = directory / "Makefile.am"
                for prefix in re.findall(r"^(\w+)_DATA\s*=", recipe.read_text(), re.M):
                    destination = inventory(recipe, prefix + "dir")[0].replace("$(datadir)", "/usr/share")
                    for file in inventory(recipe, prefix + "_DATA"):
                        copy_data(directory / file, destination + "/" + file)
        for theme in (source / "themes").iterdir():
            if theme.is_dir():
                recipe = theme / "Makefile.am"
                path = inventory(recipe, "themedir")
                if len(path) != 1 or not path[0].startswith("$(datadir)/themes/"):
                    raise ValueError("original xfwm4 theme destination missing")
                destination = path[0].replace("$(datadir)", "/usr/share")
                for file in enabled_inventory(recipe, "theme_DATA"):
                    copy_data(theme / file, destination + "/" + file)
        for file in (source / "settings-dialogs").glob("*.desktop.in"):
            # Desktop entries retain their original fields; translation marker
            # prefixes are removed for the English catalog in this native root.
            content = re.sub(r"^_(\w+)=", r"\1=", file.read_text(), flags=re.M)
            name = "usr/share/applications/" + file.name.removesuffix(".in"); destination = install / name
            destination.parent.mkdir(parents=True, exist_ok=True); destination.write_text(content)
        copy_data(source / "COPYING", "/usr/share/licenses/xfwm4/COPYING")
        defined, undefined = set(), set()
        for path in install.rglob("*.a"):
            if path.is_relative_to(install / "usr/libexec/xorg"):
                continue
            defined |= definitions(path); undefined |= symbols(path)[1]
        args.imports.write_text("\n".join(sorted(undefined - defined - {"___dso_handle"})) + "\n"); pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "programs": programs, "configuration": values, "sdk_probes": probes,
            "installed_data": data_records, "bundle_sha256": digest(args.output), "runtime_checked": False, "desktop_checked": False,
            "features": "Complete original xfwm4, helper and three settings tools; XRender compositor, XI2/RandR/XRes/XSync and startup notification. Optional OpenGL and Xpresent client support absent."}, indent=2) + "\n")
        print(f"Native xfwm4 {args.arch}: {sum(len(v['sources']) for v in programs.values())} original Mach-O objects, five programs and original themes")

if __name__ == "__main__":
    main()
