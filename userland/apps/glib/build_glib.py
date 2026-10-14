#!/usr/bin/env python3
"""Compile genuine GLib/GObject/GModule Darwin source inventories in Bazel."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "dependencies", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    upstream = root / "src/Applications/glib/glib/2.90.1"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-glib-core-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install = work / "sdk", work / "install"
        for source in (args.sdk, args.mach_headers):
            with tarfile.open(source) as archive:
                archive.extractall(sdk, filter="data")
        with tarfile.open(args.dependencies) as archive:
            archive.extractall(install, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("GLib SDK architecture mismatch")
        includes = install / "usr/include/glib-2.0"
        includes.mkdir()
        for name in ("glib", "gobject", "gmodule"):
            for source in (upstream / name).rglob("*.h"):
                if "tests" not in source.parts:
                    destination = includes / (source.relative_to(upstream) if name != "gmodule" else source.name)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, destination)
        for name in ("glib.h", "glib-object.h", "glib-unix.h"):
            shutil.copyfile(upstream / "glib" / name, includes / name)
        generated = work / "generated"
        generated.mkdir()
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                 "-D_FORTIFY_SOURCE=0", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-availability", "-fvisibility=hidden", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", install / "usr/include", "-I", includes,
                 "-I", includes / "glib", "-I", generated, "-I", upstream]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        configuration, features = {}, {}

        def probe(name, text, required=False):
            source = generated / "feature.c"
            source.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-Wno-deprecated-declarations",
                                     "-c", str(source), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode:
                raise ValueError(name + ":\n" + result.stderr.decode())
            return result.returncode == 0

        probe("Darwin_types", "#include <stdint.h>\n#include <sys/types.h>\n#include <stddef.h>\n"
              "_Static_assert(__APPLE__ && __MACH__ && sizeof(void*)==8 && sizeof(long)==8 && sizeof(int)==4, \"Darwin LP64\");\n"
              "_Static_assert(sizeof(off_t)==8 && sizeof(wchar_t)==4, \"native file/character types\");\n"
              "_Static_assert(__atomic_always_lock_free(8,0), \"real lock-free atomics\");\n"
              "void f(int64_t *a, size_t *b) { long long *c=a; unsigned long *d=b; (void)c; (void)d; }\n", True)
        recipe = (upstream / "meson.build").read_text()
        match = re.search(r"^headers = \[(.*?)^\]", recipe, re.M | re.S)
        if not match:
            raise ValueError("missing GLib header feature inventory")
        headers = re.findall(r"'([^']+)'", match[1])
        available = []
        for header in headers:
            macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
            if probe(macro, "#include <" + header + ">\n"):
                configuration[macro] = 1
                available.append(header)
        # Native Darwin declarations only. Every selected runtime import must
        # later resolve to a genuine provider; a declaration is not link proof.
        prefix = "".join("#include <" + header + ">\n" for header in available)
        prefix += "#include <stdio.h>\n#include <time.h>\n#include <math.h>\n#include <sys/mman.h>\n#include <net/if.h>\n#include <pthread.h>\n#include <crt_externs.h>\n#include <dlfcn.h>\n"
        match = re.search(r"^functions = \[(.*?)^\]", recipe, re.M | re.S)
        if not match:
            raise ValueError("missing GLib function feature inventory")
        functions = re.findall(r"'([^']+)'", match[1]) + ["_NSGetEnviron", "if_nametoindex", "if_indextoname", "statfs",
            "posix_memalign", "posix_spawn", "posix_spawnp", "clock_gettime", "pthread_attr_setstacksize",
            "pthread_attr_setinheritsched", "pthread_condattr_setclock", "pthread_cond_timedwait_relative_np", "pthread_getname_np"]
        for function in functions:
            macro = "HAVE_" + function.upper()
            if probe(macro, prefix + "void *native_feature=(void*)&" + function + ";\n"):
                configuration[macro] = 1
        # This embedded system currently has no native syslog/ASL runtime or
        # daemon. Keep the upstream writer's honest UNHANDLED branch; normal
        # structured logging still uses the real standard-stream writer.
        configuration.pop("HAVE_SYSLOG_H", None)
        probe("native_poll_socket_values", "#include <poll.h>\n#include <sys/socket.h>\n"
              "_Static_assert(POLLIN==1 && POLLOUT==4 && POLLPRI==2 && POLLERR==8 && POLLHUP==16 && POLLNVAL==32, \"Darwin poll\");\n"
              "_Static_assert(AF_UNIX==1 && AF_INET==2 && AF_INET6==30 && MSG_OOB==1 && MSG_PEEK==2 && MSG_DONTROUTE==4, \"Darwin socket ABI\");\n", True)
        configuration.update(GLIB_MAJOR_VERSION=2, GLIB_MINOR_VERSION=90, GLIB_MICRO_VERSION=1,
            GLIB_INTERFACE_AGE=1, GLIB_BINARY_AGE=9001, ENABLE_NLS=1, THREADS_POSIX=1,
            HAVE_PTHREAD_SETNAME_NP_WITHOUT_TID=1, HAVE_LANGINFO_CODESET=1, HAVE_LC_MESSAGES=1,
            HAVE_STRERROR_R=1, HAVE_GOOD_PRINTF=1, HAVE_VASPRINTF=1, HAVE_C99_VSNPRINTF=1,
            USE_SYSTEM_PRINTF=1,
            HAVE_GNUC_VISIBILITY=1, G_HAVE_GNUC_VISIBILITY=1, HAVE_LONG_LONG_FORMAT=1,
            ALIGNOF_GUINT32=4, ALIGNOF_GUINT64=8, SIZEOF_CHAR=1, SIZEOF_SHORT=2, SIZEOF_INT=4,
            SIZEOF_LONG=8, SIZEOF_LONG_LONG=8, SIZEOF_SIZE_T=8, SIZEOF_SSIZE_T=8, SIZEOF_VOID_P=8,
            SIZEOF_WCHAR_T=4, GLIB_LOCALE_DIR='"/usr/share/locale"', GETTEXT_PACKAGE='"glib20"',
            PACKAGE_NAME='"glib"', PACKAGE_VERSION='"2.90.1"', PACKAGE_STRING='"glib 2.90.1"',
            G_ENABLE_DEBUG=1)
        (generated / "config.h").write_text("/* SDK-verified native Darwin configuration. */\n" +
            "".join(f"#define {key} {value}\n" for key, value in sorted(configuration.items())))
        values = dict(gint16="short", gint16_modifier='"h"', gint16_format='"hi"', guint16_format='"hu"',
            gint32="int", gint32_modifier='""', gint32_format='"i"', guint32_format='"u"',
            gint64="long long", gint64_modifier='"ll"', gint64_format='"lli"', guint64_format='"llu"',
            gint64_constant="(G_GNUC_EXTENSION (val##LL))", guint64_constant="(G_GNUC_EXTENSION (val##ULL))",
            glib_extension="G_GNUC_EXTENSION ", glib_void_p=8, glib_long=8, glib_size_t=8, glib_ssize_t=8,
            glib_size_type_define="long", glib_msize_type="LONG", gsize_modifier='"l"', gssize_modifier='"l"',
            gsize_format='"lu"', gssize_format='"li"', glib_intptr_type_define="long", gintptr_modifier='"l"',
            gintptr_format='"li"', guintptr_format='"lu"', glib_gpi_cast="(glong)", glib_gpui_cast="(gulong)",
            g_pollfd_format='"%d"', glib_os="#define G_OS_UNIX 1", glib_vacopy="", g_threads_impl_def="POSIX",
            g_bs_native="LE", g_bs_alien="BE", glongbits=64, gintbits=32, gsizebits=64,
            g_byte_order="G_LITTLE_ENDIAN", g_module_suffix="so", g_pid_type="int", g_pid_format='"i"',
            g_dir_separator="/", g_searchpath_separator=":", g_pollin=1, g_pollout=4, g_pollpri=2,
            g_pollhup=16, g_pollerr=8, g_pollnval=32, g_af_unix=1, g_af_inet=2, g_af_inet6=30,
            g_msg_oob=1, g_msg_peek=2, g_msg_dontroute=4, GLIB_MAJOR_VERSION=2, GLIB_MINOR_VERSION=90,
            GLIB_MICRO_VERSION=1, GLIB_HAVE_ALLOCA_H=1, GLIB_USING_SYSTEM_PRINTF=1, G_ATOMIC_LOCK_FREE=1)
        text = (upstream / "glib/glibconfig.h.in").read_text()
        text = re.sub(r"^#mesondefine (\w+)$", lambda match: "#define " + match[1] + " " + str(values[match[1]])
                      if match[1] in values else "/* #undef " + match[1] + " */", text, flags=re.M)
        text = re.sub(r"@(\w+)@", lambda match: str(values[match[1]]), text)
        (includes / "glibconfig.h").write_text(text)
        visibility = upstream / "tools/gen-visibility-macros.py"
        run(["python3", visibility, "2.90.1", "versions-macros", upstream / "glib/gversionmacros.h.in",
             includes / "glib/gversionmacros.h"])
        for name, namespace in (("glib", "GLIB"), ("gobject", "GOBJECT"), ("gmodule", "GMODULE")):
            destination = includes / name / (name + "-visibility.h")
            destination.parent.mkdir(parents=True, exist_ok=True)
            run(["python3", visibility, "2.90.1", "visibility-macros", namespace, destination])
        module_values = {"G_MODULE_IMPL": "G_MODULE_IMPL_DL", "G_MODULE_HAVE_DLERROR": 1,
                         "G_MODULE_NEED_USCORE": 0, "G_MODULE_BROKEN_RTLD_GLOBAL": 0}
        text = re.sub(r"@(\w+)@", lambda match: str(module_values[match[1]]),
                      (upstream / "gmodule/gmoduleconf.h.in").read_text())
        (generated / "gmoduleconf.h").write_text(text)
        generator = generated / "glib-mkenums"
        generator.write_text((upstream / "gobject/glib-mkenums.in").read_text().replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
        for suffix in ("h", "c"):
            with (generated / ("glib-enumtypes." + suffix)).open("w") as stream:
                subprocess.run(["python3", str(generator), "--template", str(upstream / "gobject" / ("glib-enumtypes." + suffix + ".template")),
                                str(upstream / "glib/gunicode.h")], stdout=stream, check=True)
        shutil.copyfile(generated / "glib-enumtypes.h", includes / "gobject/glib-enumtypes.h")
        records = {}
        for name in ("glib", "gobject", "gmodule"):
            source = upstream / name
            if name == "gmodule":
                files = [source / "gmodule.c", source / "gmodule-deprecated.c"]
            else:
                match = re.search(name + r"_sources \+= files\((.*?)\n\)", (source / "meson.build").read_text(), re.S)
                if not match:
                    raise ValueError("missing upstream source inventory: " + name)
                files = [source / path for path in re.findall(r"'([^']+\.c)'", match[1])]
            if name == "glib":
                # gthread.c itself includes the selected POSIX backend, as
                # upstream specifies; do not compile that body a second time.
                files += [source / path for path in ("glib-unix.c", "gspawn-posix.c", "giounix.c", "libcharset/localcharset.c")]
            if name == "gobject": files.append(generated / "glib-enumtypes.c")
            jobs = [(path, generated / (name + f"_{index:03d}.o")) for index, path in enumerate(files)]
            local = [*flags, "-I", source, "-I", upstream / "glib", "-I", includes / "gobject",
                     "-D" + name.upper() + "_COMPILATION", '-DGLIB_CHARSETALIAS_DIR="/usr/lib"']

            def compile_one(job):
                path, obj = job
                run([*local, "-c", path, "-o", obj])
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign GLib object")
                return {"source": str(path.relative_to(root)) if path.is_relative_to(root) else path.name,
                        "sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}

            with ThreadPoolExecutor(max_workers=8) as pool:
                sources = list(pool.map(compile_one, jobs))
            output = install / "usr/lib" / ("lib" + name + "-2.0.a")
            run(["llvm-ar-20", "--format=darwin", "rcsD", output, *[obj for _, obj in jobs]])
            public, imports, weak = symbols(output)
            records[name] = {"sources": sources, "exports": sorted(public),
                             "imports": sorted(imports - definitions(output)), "weak_imports": sorted(weak), "sha256": digest(output)}
        defined, undefined = set(), set()
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
                    with path.open("rb") as stream: archive.addfile(info, stream)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": "2.90.1", "components": records,
            "sdk_features": features, "configuration": configuration, "glibconfig_values": values,
            "imports": requests, "bundle_sha256": digest(args.output), "runtime_checked": False,
            "scope": "Native Darwin source compilation; separate strong closure and guest GLib/GObject/GModule tests required."}, indent=2) + "\n")


if __name__ == "__main__":
    main()
