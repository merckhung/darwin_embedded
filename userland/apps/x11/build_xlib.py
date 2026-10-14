#!/usr/bin/env python3
"""Compile the complete selected upstream libX11 inventory through Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile

from build_clients import all_definitions, digest, inventory, pack, run, symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "xcb", "output", "headers", "imports", "report", "support"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    upstream = root / "src/Applications/x11/libX11/1.8.13"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xlib-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk, tree = work / "sdk", work / "install"
        for archive_path, destination in ((args.sdk, sdk), (args.xcb, tree)):
            with tarfile.open(archive_path) as archive:
                archive.extractall(destination, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("Xlib SDK architecture mismatch")
        include = tree / "usr/include"
        for header in (upstream / "include").rglob("*.h"):
            dest = include / header.relative_to(upstream / "include")
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(header, dest)
        conf = (upstream / "include/X11/XlibConf.h.in").read_text()
        conf = conf.replace("#undef XTHREADS", "#define XTHREADS 1")
        conf = conf.replace("#undef XUSE_MTSAFE_API", "#define XUSE_MTSAFE_API 1")
        (include / "X11/XlibConf.h").write_text(conf)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                 "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-DHAVE_CONFIG_H=1", "-Werror=implicit-function-declaration",
                 "-Wno-availability", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", include, "-I", include / "X11", "-I", work]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        for subdir in ("src", "src/xcms", "src/xkb", "src/xlibi18n"):
            flags += ["-I", upstream / subdir]
        features = {}
        probe = work / "feature.c"
        headers = "".join(f"#include <{name}>\n" for name in (
            "stdlib.h", "string.h", "strings.h", "unistd.h", "sys/socket.h", "sys/mman.h",
            "sys/shm.h", "netdb.h", "arpa/inet.h", "dlfcn.h", "poll.h", "pwd.h", "langinfo.h"))
        for function in ("dlopen", "reallocarray", "strtol", "seteuid", "issetugid", "getresuid",
                         "shmat", "poll", "getaddrinfo", "getpagesize", "inet_ntop", "mmap",
                         "strcasecmp", "strlcpy", "getpwuid_r", "nl_langinfo"):
            probe.write_text(headers + f"void *p=(void*)&{function};\n")
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(probe),
                                     "-o", str(work / "feature.o")], capture_output=True)
            features["HAVE_" + function.upper()] = result.returncode == 0
        values = {name: 1 for name, selected in features.items() if selected}
        for header in ("dlfcn.h", "sys/filio.h", "sys/select.h", "sys/ioctl.h", "sys/socket.h",
                       "sys/param.h", "sys/stat.h", "sys/types.h", "unistd.h", "stdint.h",
                       "inttypes.h", "stdio.h", "stdlib.h", "strings.h", "string.h", "wchar.h"):
            probe.write_text(f"#include <{header}>\n")
            result = subprocess.run([*map(str, flags), "-c", str(probe), "-o", str(work / "feature.o")], capture_output=True)
            key = "HAVE_" + header.upper().replace("/", "_").replace(".", "_")
            features[key] = result.returncode == 0
            if not result.returncode:
                values[key] = 1
        # Darwin sockets and wchar_t are verified rather than borrowing the
        # Linux host's configure results. No resolver implementation is implied.
        probe.write_text("#include <sys/socket.h>\n#include <netinet/in.h>\n#include <wchar.h>\n"
                         "_Static_assert(sizeof(wchar_t)==4, \"Xlib WCHAR32\");\n"
                         "int x=sizeof(socklen_t)+sizeof(struct sockaddr_storage)+sizeof(((struct sockaddr_in*)0)->sin_len);\n")
        run([*flags, "-Werror", "-c", probe, "-o", work / "feature.o"])
        values.update({"BSD44SOCKETS": 1, "HAVE_SOCKLEN_T": 1, "HAVE_STRUCT_SOCKADDR_STORAGE": 1,
                       "HAVE___BUILTIN_POPCOUNTL": 1, "XTHREADS": 1, "XUSE_MTSAFE_API": 1,
                       "XCMS": 1, "XLOCALE": 1, "XKB": 1, "XF86BIGFONT": 1,
                       "USE_THREAD_SAFETY_CONSTRUCTOR": 1, "UNIXCONN": 1, "TCPCONN": 1,
                       "IPv6": 1, "USE_POLL": 1, "USE_DYNAMIC_XCURSOR": 1,
                       "LIBXCURSOR": '"/usr/lib/libXcursor.1.dylib"',
                       "X11_DATADIR": '"/usr/share/X11"', "X11_LIBDIR": '"/usr/lib/X11"',
                       "XLOCALEDATADIR": '"/usr/share/X11/locale"', "XLOCALEDIR": '"/usr/share/X11/locale"',
                       "XLOCALELIBDIR": '"/usr/lib/X11/locale"', "XKEYSYMDB": '"/usr/share/X11/XKeysymDB"',
                       "XERRORDB": '"/usr/share/X11/XErrorDB"', "PACKAGE_STRING": '"libX11 1.8.13"'})
        for required in ("HAVE_DLOPEN", "HAVE_GETADDRINFO", "HAVE_GETPWUID_R", "HAVE_POLL"):
            if not features[required]:
                raise ValueError("required native declaration absent: " + required)
        if features["HAVE_ISSETUGID"]:
            values["HASSETUGID"] = 1
        else:
            values["NO_XLOCALEDIR"] = 1
        if features["HAVE_SHMAT"]:
            values["HAS_SHM"] = 1
        if features["HAVE_MMAP"] and features["HAVE_NL_LANGINFO"]:
            values["COMPOSECACHE"] = 1
        (work / "config.h").write_text("/* Selected native Darwin configuration. */\n" +
            "".join(f"#define {name} {value}\n" for name, value in sorted(values.items())))
        generator = work / "makekeys"
        run(["clang-20", "-std=gnu11", "-O2", upstream / "src/util/makekeys.c", "-o", generator])
        (work / "ks_tables.h").write_text(run([generator, *[include / "X11" / name for name in (
            "keysymdef.h", "XF86keysym.h", "Sunkeysym.h", "DECkeysym.h", "HPkeysym.h")]]))
        groups = [
            ("src", "libX11_la_SOURCES", ["-DX11_t", "-DTRANS_CLIENT"]),
            ("src/xcms", "libxcms_la_SOURCES", ['-DXCMSDIR="/usr/share/X11"']),
            ("src/xkb", "libxkb_la_SOURCES", []),
            ("src/xlibi18n", "libi18n_la_SOURCES", []),
            ("modules/im/ximcp", "ximcp_la_SOURCES", ["-DXIM_t", "-DTRANS_CLIENT"]),
            ("modules/lc/def", "xlcDef_la_SOURCES", []),
            ("modules/lc/gen", "xlibi18n_la_SOURCES", []),
            ("modules/lc/Utf8", "xlcUTF8Load_la_SOURCES", []),
            ("modules/om/generic", "xomGeneric_la_SOURCES", []),
        ]
        jobs, inventories = [], []
        for subdir, name, options in groups:
            manifest = upstream / subdir / "Makefile.am"
            files = inventory(manifest, name)
            inventories.append({"source": str(manifest.relative_to(root)), "sha256": digest(manifest),
                                "variable": name, "selected": files})
            for filename in files:
                jobs.append((upstream / subdir / filename, options))
        if not features["HAVE_REALLOCARRAY"]:
            jobs.append((upstream / "src/reallocarray.c", []))
        objects, errors, provenance = [], [], []
        def compile_one(index_job):
            index, (source, options) = index_job
            obj = work / f"{index:03d}_{source.stem}.o"
            result = subprocess.run([*map(str, flags), *options, "-c", str(source), "-o", str(obj)], capture_output=True)
            if result.returncode:
                return None, str(source.relative_to(upstream)) + "\n" + result.stderr.decode(), None
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native Xlib object: " + str(source))
            return obj, None, {"source": str(source.relative_to(root)), "sha256": digest(source),
                               "object_sha256": digest(obj), "options": options}
        with ThreadPoolExecutor(max_workers=8) as pool:
            for obj, error, record in pool.map(compile_one, enumerate(jobs)):
                if error:
                    errors.append(error)
                else:
                    objects.append(obj)
                    provenance.append(record)
        if errors:
            raise RuntimeError(f"libX11 {args.arch}: {len(objects)}/{len(jobs)} source objects compile\n" + "\n".join(errors))
        library = tree / "usr/lib/libX11.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", library, *objects])
        glue = work / "x11_xcb.o"
        run([*flags, "-c", upstream / "src/x11_xcb.c", "-o", glue])
        run(["llvm-ar-20", "--format=darwin", "rcsD", tree / "usr/lib/libX11-xcb.a", glue])
        # Match Apple's actual vDarwinExtsn fdopen build, including the base
        # NON_CANCELABLE setting. This is its real FILE implementation, not an
        # alias to another stdio variant or a file-descriptor shim.
        libc = root / "src/Libraries/25.5.0/Libc"
        support_source = libc / "stdio/FreeBSD/fdopen.c"
        support_obj = work / "fdopen_darwin.o"
        support_flags = ["-D__LIBC__=1", "-DBUILDING_VARIANT=1", "-DVARIANT_DARWINEXTSN=1",
                         "-D__DARWIN_NON_CANCELABLE=1", "-D__DARWIN_UNIX03=1", "-fblocks",
                         "-DPRIVATE=1", "-D_FORTIFY_SOURCE=0", "-isystem", sdk / "usr/local/include",
                         "-Wno-nullability-completeness", "-Wno-deprecated-declarations",
                         "-include", libc / "fbsdcompat/_fbsd_compat_.h"]
        for directory in ("fbsdcompat", "include", "darwin", "gen/FreeBSD", "stdio", "stdio/FreeBSD",
                          "locale", "locale/FreeBSD", "stdtime/FreeBSD"):
            support_flags += ["-I", libc / directory]
        run([*flags, *support_flags, "-c", support_source, "-o", support_obj])
        run(["llvm-ar-20", "--format=darwin", "rcsD", args.support, support_obj])
        shutil.copyfile(args.support, tree / "usr/lib/libnative_x11_libc.a")
        defined, undefined = set(), set()
        for archive in (tree / "usr/lib").glob("*.a"):
            defined |= all_definitions(archive)
            undefined |= symbols(archive)[1]
        # Clang's destructor registration binds this to its own linked image.
        # Darwin's linker defines it; another image must not export it for us.
        linker_defined = {"___dso_handle"}
        required = sorted(undefined - defined - linker_defined)
        args.imports.write_text("\n".join(required) + "\n")
        pack(tree, args.output)
        header_tree = work / "headers"
        shutil.copytree(include, header_tree / "usr/include")
        pack(header_tree, args.headers)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": "1.8.13",
            "source_manifest_sha256": digest(upstream / "UPSTREAM.json"), "inventories": inventories,
            "configuration": values, "feature_declarations": features, "objects": provenance,
            "exports": sorted(symbols(library)[0]), "native_system_imports": required,
            "linker_defined_imports": sorted((undefined - defined) & linker_defined),
            "libc_support": {"source": str(support_source.relative_to(root)), "sha256": digest(support_source),
                             "configuration": [str(flag) for flag in support_flags if str(flag).startswith("-D")],
                             "archive_sha256": digest(args.support)},
            "generated_keysym_table_sha256": digest(work / "ks_tables.h"),
            "host_generator_source_sha256": digest(upstream / "src/util/makekeys.c"),
            "libX11_xcb_source_sha256": digest(upstream / "src/x11_xcb.c"),
            "sdk_sha256": digest(args.sdk), "xcb_bundle_sha256": digest(args.xcb),
            "bundle_sha256": digest(args.output), "runtime_checked": False,
            "omitted_services": ["Apple launchd IPC/service-manager integration",
                                 "loadable i18n (upstream default is static)",
                                 "optional libXcursor.1.dylib is not yet packaged"],
            "validation": "Complete selected upstream Xlib source inventories are native Mach-O; system link closure is separate."}, indent=2) + "\n")
        print(f"Native Xlib {args.arch}: {len(objects) + 1} genuine Mach-O objects; {len(required)} system imports")


if __name__ == "__main__":
    main()
