#!/usr/bin/env python3
"""Build complete upstream POSIX GIO sources as native Mach-O using Bazel."""
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

from build_dependencies import definitions, digest
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols


def inventory(recipe, name, suffix):
    match = re.search(r"^" + name + r" = files\((.*?)\n\)", recipe, re.M | re.S)
    if not match:
        raise ValueError("missing upstream GIO inventory: " + name)
    return re.findall(r"'([^']+\." + suffix + r")'", match[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "core", "core-report", "graphics", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    upstream = root / "src/Applications/glib/glib/2.90.1"
    source = upstream / "gio"
    recipe = (source / "meson.build").read_text()
    core = json.loads(args.core_report.read_text())
    if core["architecture"] != args.arch or core["bundle_sha256"] != digest(args.core):
        raise ValueError("GIO core inputs do not match")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-gio-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        for archive_path in (args.sdk, args.mach_headers):
            with tarfile.open(archive_path) as archive:
                archive.extractall(sdk, filter="data")
        with tarfile.open(args.core) as archive:
            archive.extractall(install, filter="data")
        with tarfile.open(args.graphics) as archive:
            selected = [archive.getmember(name) for name in
                        ("usr/lib/libz.a", "usr/include/zlib.h", "usr/include/zconf.h")]
            selected += [member for member in archive.getmembers()
                         if member.name.startswith("usr/share/licenses/zlib/") and member.isfile()]
            archive.extractall(install, members=selected, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("GIO SDK architecture mismatch")
        includes = install / "usr/include/glib-2.0"
        (includes / "gio").mkdir()
        for path in source.glob("*.h"):
            shutil.copyfile(path, includes / "gio" / path.name)
        visibility = upstream / "tools/gen-visibility-macros.py"
        run(["python3", visibility, "2.90.1", "visibility-macros", "GIO", includes / "gio/gio-visibility.h"])
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-D_FORTIFY_SOURCE=0", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-fvisibility=hidden", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", generated, "-I", install / "usr/include",
                 "-I", includes, "-I", includes / "glib", "-I", includes / "gobject", "-I", includes / "gio", "-I", source,
                 "-I", upstream, "-I", upstream / "glib", "-I", upstream / "subprojects/gvdb"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        configuration = dict(core["configuration"])
        features = {}
        adaptations = {}
        resolver_header = root / "userland/native/resolver/records.h"
        shutil.copyfile(resolver_header, generated / "native-resolver-records.h")
        original = source / "gthreadedresolver.c"
        text = original.read_text()
        replacements = {
            '#include "config.h"': '#include "config.h"\n#include "native-resolver-records.h"',
            "dn_expand (": "native_resolver_expand (",
            "res_query (rrname, C_IN, rrtype, answer->data, answer->len)":
                "native_resolver_query (rrname, C_IN, rrtype, answer->data, answer->len, &herr)",
            "  herr = h_errno;": "  /* Native ASR returns the error with this query. */",
        }
        for before, after in replacements.items():
            if before not in text:
                raise ValueError("upstream threaded resolver adaptation boundary changed")
            text = text.replace(before, after)
        native_resolver = generated / original.name
        native_resolver.write_text(text)
        adaptations[original] = native_resolver
        # Upstream's POSIX app-info implementation is selected when Cocoa is
        # absent. Register its deprecated extension point under the same choice.
        original = source / "giomodule.c"
        text = original.read_text()
        if text.count("defined(G_OS_UNIX) && !defined(__APPLE__)") != 2:
            raise ValueError("upstream desktop app-info registration boundary changed")
        text = text.replace("defined(G_OS_UNIX) && !defined(__APPLE__)", "defined(G_OS_UNIX) && !defined(HAVE_COCOA)")
        native_module = generated / original.name
        native_module.write_text(text)
        adaptations[original] = native_module

        def probe(name, text, required=False):
            path = generated / "feature.c"
            path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-Wno-deprecated-declarations", "-c", str(path),
                                     "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if result.returncode and required:
                raise ValueError(name + ":\n" + result.stderr.decode())
            return result.returncode == 0

        probe("native_kqueue", "#include <sys/event.h>\n#include <sys/time.h>\n"
              "void *a=(void*)&kqueue, *b=(void*)&kevent;\n", True)
        configuration["FILE_MONITOR_BACKEND_KQUEUE"] = 1
        probe("native_statfs", "#include <sys/mount.h>\n"
              "void f(struct statfs *s){s->f_bavail=0;s->f_fstypename[0]=0;}\n", True)
        configuration.update(USE_STATFS=1, HAVE_STRUCT_STATFS_F_BAVAIL=1,
                             HAVE_STRUCT_STATFS_F_FSTYPENAME=1, HAVE_SIOCGIFADDR=1,
                             MAJOR_IN_TYPES=1, GLIB_RUNSTATEDIR='"/var/run"', GLIB_LOCALSTATEDIR='"/var"')
        # Select the real Darwin extended-attribute call signatures, which
        # differ from the Linux signatures. Strong closure is checked later.
        if probe("native_xattr", "#include <sys/xattr.h>\n"
                 "void f(void){char x[8];getxattr(\"/\",\"x\",x,8,0,0);listxattr(\"/\",x,8,0);\n"
                 "setxattr(\"/\",\"x\",x,8,0,0);removexattr(\"/\",\"x\",0);}\n"):
            configuration.update(HAVE_XATTR=1, HAVE_SYS_XATTR_H=1, HAVE_XATTR_NOFOLLOW=1)
        nameser = "#include <sys/types.h>\n#include <arpa/nameser.h>\n"
        compatibility = ""
        if not probe("native_C_IN", nameser + "int native_class=C_IN;\n"):
            compatibility = "#include <arpa/nameser_compat.h>"
            probe("native_C_IN_compat", nameser + compatibility + "\nint native_class=C_IN;\n", True)
        networking = (source / "gnetworking.h.in").read_text().replace("@NAMESER_COMPAT_INCLUDE@", compatibility)
        (includes / "gio/gnetworking.h").write_text(networking)
        (generated / "config.h").write_text("/* Native Darwin configuration, inherited from verified GLib core. */\n" +
            "".join(f"#define {key} {value}\n" for key, value in sorted(configuration.items())))
        generator = generated / "glib-mkenums"
        generator.write_text((upstream / "gobject/glib-mkenums.in").read_text()
                             .replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
        headers = []
        for name in ("gio_headers", "application_headers", "settings_headers", "gdbus_headers"):
            headers.extend(source / name for name in inventory(recipe, name, "h"))
        headers.append(includes / "gio/gnetworking.h")
        for suffix in ("h", "c"):
            with (generated / ("gioenumtypes." + suffix)).open("w") as stream:
                subprocess.run(["python3", str(generator), "--template", str(source / ("gioenumtypes." + suffix + ".template")),
                                *map(str, headers)], stdout=stream, check=True)
        shutil.copyfile(generated / "gioenumtypes.h", includes / "gio/gioenumtypes.h")
        codegen_source = source / "gdbus-2.0/codegen"
        codegen = generated / "codegen"
        shutil.copytree(codegen_source, codegen)
        replacements = {"PYTHON": "python3", "VERSION": "2.90.1", "MAJOR_VERSION": "2", "MINOR_VERSION": "90",
                        "DATADIR": str(generated / "share")}
        for original, target in (("config.py.in", "config.py"), ("gdbus-codegen.in", "gdbus-codegen")):
            text = (codegen_source / original).read_text()
            text = re.sub(r"@(\w+)@", lambda match: replacements[match[1]], text)
            (codegen / target).write_text(text)
        generators = []
        for name, prefix, namespace, xml in (
            ("xdp-dbus", "org.freedesktop.portal.", "GXdp", ["org.freedesktop.portal." + name + ".xml"
                for name in ("Documents", "OpenURI", "ProxyResolver", "Trash")]),
            ("gdbus-daemon-generated", "org.", "_G", ["dbus-daemon.xml"]),
        ):
            run(["python3", codegen / "gdbus-codegen", "--interface-prefix", prefix,
                 "--output-directory", generated, "--generate-c-code", name, "--c-namespace", namespace,
                 *[source / path for path in xml]])
            generators.append({"name": name, "inputs": {str((source / path).relative_to(root)): digest(source / path)
                                                        for path in xml},
                               "outputs": {name + "." + suffix: digest(generated / (name + "." + suffix))
                                           for suffix in ("h", "c")}})
        files = []
        for name in ("gio_base_sources", "gdbus_sources", "settings_sources", "application_sources", "local_sources"):
            files.extend(source / path for path in inventory(recipe, name, "c"))
        unix_match = re.search(r"  unix_sources = files\((.*?)\n  \)", recipe, re.S)
        portal_match = re.search(r"  portal_sources = \[files\((.*?)\n    \),", recipe, re.S)
        if not unix_match or not portal_match:
            raise ValueError("missing upstream GIO POSIX inventory")
        for match in (unix_match, portal_match):
            files.extend(source / path for path in re.findall(r"'([^']+\.c)'", match[1]))
        files.extend(source / name for name in ("gcontenttype-fdo.c", "gdesktopappinfo.c", "gmemorymonitorpsi.c", "gdbusdaemon.c"))
        files.append(upstream / "glib/gtrace.c")
        files.extend(generated / name for name in ("gioenumtypes.c", "xdp-dbus.c", "gdbus-daemon-generated.c"))
        kqueue = source / "kqueue"
        files.extend(kqueue / name for name in re.findall(r"'([^']+\.c)'", (kqueue / "meson.build").read_text()))
        xdgmime = source / "xdgmime"
        files.extend(xdgmime / name for name in inventory((xdgmime / "meson.build").read_text(), "xdgmime_sources", "c"))
        gvdb = upstream / "subprojects/gvdb"
        files.extend(gvdb / name for name in inventory((gvdb / "meson.build").read_text(), "libgvdb_sources", "c"))
        if len(files) != len(set(files)):
            raise ValueError("duplicate GIO compilation unit")
        local = [*flags, "-DGIO_COMPILATION", '-DG_LOG_DOMAIN="GLib-GIO"',
                 '-DGIO_LAUNCH_DESKTOP="/usr/libexec/gio-launch-desktop"',
                 '-DGIO_MODULE_DIR="/usr/lib/gio/modules"', '-DLOCALSTATEDIR="/var"']
        jobs = [(path, generated / f"gio_{index:03d}.o") for index, path in enumerate(files)]

        def compile_one(job):
            path, obj = job
            extra = ["-DHAVE_CONFIG_H", "-DXDG_PREFIX=_gio_xdg"] if path.parent == xdgmime else []
            try:
                run([*local, *extra, "-c", adaptations.get(path, path), "-o", obj])
            except RuntimeError as error:
                return {"error": str(error)}
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("foreign GIO object")
            return {"source": str(path.relative_to(root)) if path.is_relative_to(root) else path.name,
                    "sha256": digest(path), "compiled_sha256": digest(adaptations.get(path, path)),
                    "member": obj.name, "object_sha256": digest(obj)}

        with ThreadPoolExecutor(max_workers=8) as pool:
            records = list(pool.map(compile_one, jobs))
        errors = [record["error"] for record in records if "error" in record]
        if errors:
            raise ValueError("GIO compilation failed:\n" + "\n".join(errors))
        output = install / "usr/lib/libgio-2.0.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, obj in jobs]])
        launch = install / "usr/libexec/gio-launch-desktop.o"
        launch.parent.mkdir()
        run([*flags, "-c", source / "gio-launch-desktop.c", "-o", launch])
        defined, undefined = set(), symbols(launch)[1]
        for archive in (install / "usr/lib").glob("*.a"):
            defined |= definitions(archive)
            undefined |= symbols(archive)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        with tarfile.open(args.output, "w") as archive:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    info = archive.gettarinfo(path, str(path.relative_to(install)))
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ""
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)
        public, imports, weak = symbols(output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": "2.90.1", "sources": records,
            "exports": sorted(public), "imports": sorted(imports - definitions(output)), "weak_imports": sorted(weak),
            "configuration": configuration, "sdk_features": features, "generators": generators,
            "port_adaptations": ["query-owned native ASR DNS records and compressed-name parser; no legacy BIND state",
                                 "POSIX desktop-app extension registration when Cocoa is absent"],
            "resolver_header_sha256": digest(resolver_header),
            "core_sha256": digest(args.core), "core_report_sha256": digest(args.core_report),
            "graphics_sha256": digest(args.graphics), "archive_sha256": digest(output),
            "launch_desktop": {"source": str((source / "gio-launch-desktop.c").relative_to(root)),
                               "source_sha256": digest(source / "gio-launch-desktop.c"), "object_sha256": digest(launch)},
            "bundle_sha256": digest(args.output), "system_requests": requests, "runtime_checked": False,
            "scope": "Complete POSIX GIO, native kqueue, D-Bus, settings, resources, desktop-app handling; requires strong native closure and guest verification."}, indent=2) + "\n")
        print(f"Native GIO {args.arch}: {len(records)} genuine Mach-O objects")


if __name__ == "__main__":
    main()
