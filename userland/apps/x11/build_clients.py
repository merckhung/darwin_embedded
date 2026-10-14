#!/usr/bin/env python3
"""Build genuine X.Org client sources with native Darwin headers inside Bazel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import symbols

VERSIONS = {"xorgproto": "2026.1", "xtrans": "1.6.0", "libXau": "1.0.12",
            "libXdmcp": "1.1.5", "xcb-proto": "1.17.0", "libxcb": "1.17.0"}


def run(command, cwd=None):
    result = subprocess.run(list(map(str, command)), cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def all_definitions(path):
    """Private externs satisfy archive-internal imports without becoming APIs."""
    result = set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        if "external " in line and "(undefined)" not in line:
            match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
            if not match:
                raise ValueError("unrecognized native symbol: " + line)
            result.add(match.group(1))
    return result


def inventory(path, name):
    text = path.read_text().replace("\\\n", " ")
    match = re.search(r"^" + re.escape(name) + r"\s*=\s*(.*)$", text, re.M)
    if not match:
        raise ValueError(f"missing upstream inventory {name}")
    return [value for value in match[1].split() if value.endswith(".c")]


def pack(tree, output):
    with tarfile.open(output, "w") as archive:
        for source in sorted(tree.rglob("*")):
            if source.is_file():
                info = archive.gettarinfo(source, str(source.relative_to(tree)))
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                with source.open("rb") as stream:
                    archive.addfile(info, stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "report", "imports", "headers"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--stage", choices=("auth", "xcb"), required=True)
    parser.add_argument("--auth", type=Path)
    parser.add_argument("--support", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    upstream = {name: root / "src/Applications/x11" / name / version
                for name, version in VERSIONS.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-x11-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("X11 SDK architecture mismatch")
        tree = work / "install"
        includes = tree / "usr/include"
        includes.mkdir(parents=True)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                 "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-DHAVE_CONFIG_H=1", "-Werror=implicit-function-declaration",
                 "-Wno-availability", "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                 "-I", includes]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        features = {}

        def probe(name, source, required=False):
            file = work / "feature.c"
            file.write_text(source)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(file),
                                     "-o", str(work / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode:
                raise ValueError(f"Darwin feature {name}:\n{result.stderr.decode()}")
            return features[name]

        def config(directory, values):
            (directory / "config.h").write_text("/* Native Darwin feature selection; no ABI substitutions. */\n" +
                "".join(f"#define {key} {value}\n" for key, value in sorted(values.items())))

        records = []
        archives = []

        def build(name, source, files, values):
            build_dir = work / name
            build_dir.mkdir()
            config(build_dir, values)
            jobs = []
            for index, path in enumerate(files):
                item = Path(path) if Path(path).is_absolute() else source / path
                obj = build_dir / f"{index:03d}_{item.stem}.o"
                jobs.append((item, obj))
            def compile_one(job):
                item, obj = job
                run([*flags, "-I", build_dir, "-I", source, "-c", item, "-o", obj])
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError(f"non-native X11 object: {item}")
                return {"source": str(item.relative_to(root)) if item.is_relative_to(root) else item.name,
                        "sha256": digest(item), "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                sources = list(pool.map(compile_one, jobs))
            archive = tree / "usr/lib" / (name + ".a")
            archive.parent.mkdir(parents=True, exist_ok=True)
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            defined, undefined, weak = symbols(archive)
            records.append({"component": name, "configuration": values, "sources": sources,
                            "exports": sorted(defined), "imports": sorted(undefined - all_definitions(archive)),
                            "weak_imports": sorted(weak), "archive_sha256": digest(archive)})
            archives.append(archive)

        if args.stage == "auth":
            for header in (upstream["xorgproto"] / "include").rglob("*.h"):
                dest = includes / header.relative_to(upstream["xorgproto"] / "include")
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(header, dest)
            probe("fd_set_fds_bits", "#include <sys/select.h>\nint x=sizeof(((fd_set*)0)->fds_bits);\n", True)
            template = upstream["xorgproto"] / "include/X11/Xpoll.h.in"
            (includes / "X11/Xpoll.h").write_text(template.read_text().replace("@USE_FDS_BITS@", "fds_bits"))
            transport = includes / "X11/Xtrans"
            transport.mkdir()
            for filename in ("Xtrans.h", "Xtrans.c", "Xtransint.h", "Xtranslcl.c", "Xtranssock.c", "Xtransutil.c", "transport.c"):
                shutil.copyfile(upstream["xtrans"] / filename, transport / filename)
            for name in ("libXau", "libXdmcp"):
                shutil.copytree(upstream[name] / "include", includes, dirs_exist_ok=True)
            declarations = "#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n"
            for function in ("explicit_bzero", "explicit_memset", "pathconf", "arc4random_buf", "getentropy"):
                probe("HAVE_" + function.upper(), declarations + f"void *p=(void*)&{function};\n")
            values = {key: 1 for key, value in features.items() if value and key.startswith("HAVE_")}
            values.update({"XTHREADS": 1, "HAVE_UNISTD_H": 1})
            build("libXau", upstream["libXau"], inventory(upstream["libXau"] / "Makefile.am", "libXau_la_SOURCES"), values)
            values = {"HAVE_ARC4RANDOM_BUF": 1, "HASXDMAUTH": 1}
            if not features["HAVE_ARC4RANDOM_BUF"]:
                raise ValueError("native secure random API is required")
            files = inventory(upstream["libXdmcp"] / "Makefile.am", "libXdmcp_la_SOURCES")
            files += inventory(upstream["libXdmcp"] / "Makefile.am", "nodist_libXdmcp_la_SOURCES")
            build("libXdmcp", upstream["libXdmcp"], files, values)
            wrappers = root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers"
            build("libnative_x11_support", wrappers, ["unlink.c", "rmdir.c"], {})
            if not args.support:
                raise ValueError("authentication stage must expose its genuine XNU support archive")
            shutil.copyfile(archives[-1], args.support)
        else:
            if not args.auth:
                raise ValueError("XCB requires genuine authentication headers and archives")
            with tarfile.open(args.auth) as archive:
                archive.extractall(tree, filter="data")
            src = upstream["libxcb"] / "src"
            gen = work / "generated"
            gen.mkdir()
            for name in ("xcb.h", "xcbext.h", "xcbint.h"):
                shutil.copyfile(src / name, gen / name)
            protocols = ["xproto", "bigreq", "xc_misc"]
            # configure.ac's actual default enabled extensions. DRI3 is selected
            # because Darwin's real sendmsg supports descriptor transfer; this
            # builds wire-protocol bindings, not a DRM device implementation.
            extension_names = re.findall(r"^XCB_EXTENSION\(([^,]+), (yes|\$have_sendmsg)\)",
                                         (upstream["libxcb"] / "configure.ac").read_text(), re.M)
            translated = {"resource": "res", "xfree86-dri": "xf86dri"}
            extensions = [translated.get(name.lower(), name.lower()) for name, _ in extension_names]
            protocols += extensions
            for protocol in protocols:
                run(["python3", src / "c_client.py", "-c", "libxcb 1.17.0", "-l", "X Version 11",
                     "-s", "3", "-p", upstream["xcb-proto"],
                     upstream["xcb-proto"] / "src" / (protocol + ".xml")], cwd=gen)
            probe("native_socket_types", "#include <sys/socket.h>\n#include <sys/un.h>\n#include <poll.h>\n"
                  "int x=sizeof(((struct msghdr*)0)->msg_control)+sizeof(((struct sockaddr_un*)0)->sun_len);\n", True)
            probe("native_getaddrinfo", "#include <netdb.h>\nvoid *p=(void*)&getaddrinfo;\n", True)
            values = {"GCC_HAS_VISIBILITY": 1, "HASXDMAUTH": 1, "HAVE_GETADDRINFO": 1,
                      "HAVE_SENDMSG": 1, "HAVE_SOCKADDR_SUN_LEN": 1, "USE_POLL": 1,
                      "XCB_QUEUE_BUFFER_SIZE": 16384, "PACKAGE_STRING": '"libxcb 1.17.0"'}
            flags += ["-I", gen]
            files = inventory(src / "Makefile.am", "libxcb_la_SOURCES")
            files = [src / item for item in files] + [gen / (name + ".c") for name in protocols[:3]]
            build("libxcb", src, files, values)
            for extension in extensions:
                build("libxcb-" + extension, gen, [extension + ".c"], values)
            public_headers = includes / "xcb"
            public_headers.mkdir()
            for name in ["xcb.h", "xcbext.h", *[protocol + ".h" for protocol in protocols]]:
                shutil.copyfile(gen / name, public_headers / name)
            probe("protocol_wire_abi", "#include <X11/Xproto.h>\n#include <xcb/xproto.h>\n"
                  "_Static_assert(sizeof(xEvent)==32, \"X11 wire event\");\n"
                  "_Static_assert(sizeof(xConnClientPrefix)==12, \"X11 client prefix\");\n"
                  "_Static_assert(sizeof(xcb_setup_request_t)==12, \"XCB setup wire header\");\n"
                  "_Static_assert(sizeof(xcb_key_press_event_t)==32, \"XCB core event\");\n", True)
            archives = sorted((tree / "usr/lib").glob("*.a"))
        all_defined, all_undefined = set(), set()
        for archive in archives:
            defined, undefined, _ = symbols(archive)
            all_defined |= all_definitions(archive)
            all_undefined |= undefined
        required = sorted(all_undefined - all_defined)
        args.imports.write_text("\n".join(required) + "\n")
        pack(tree, args.output)
        headers = work / "headers"
        shutil.copytree(tree / "usr/include", headers / "usr/include")
        pack(headers, args.headers)
        args.report.write_text(json.dumps({"architecture": args.arch, "stage": args.stage,
            "versions": VERSIONS, "components": records, "native_system_imports": required,
            "feature_declarations": features, "sdk_sha256": digest(args.sdk),
            "source_manifests": {name: digest(path / "UPSTREAM.json") for name, path in upstream.items()},
            "bundle_sha256": digest(args.output), "runtime_checked": False,
            "validation": "Every object is target Mach-O; system imports require a separate strong-closure link."}, indent=2) + "\n")
        print(f"Native X11 {args.stage} {args.arch}: {sum(len(r['sources']) for r in records)} genuine Mach-O objects; {len(required)} system imports")


if __name__ == "__main__":
    main()
