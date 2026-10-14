#!/usr/bin/env python3
"""Compile genuine D-Bus Unix client, daemon, helpers and tools in Bazel."""
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

VERSION = "1.16.2"


def inventories(source):
    recipe = (source / "dbus/meson.build").read_text()
    unix = re.search(r"else # Unix(.*?)^endif", recipe, re.M | re.S)[1]
    def additional(name):
        return re.findall(r"'([^']+\.c)'", re.search(r"\b" + name + r"\s*\+=\s*\[(.*?)\]", unix, re.S)[1])
    client = meson_list(recipe, "dbus_lib_sources") + meson_list(recipe, "dbus_shared_sources")
    client += additional("dbus_lib_sources") + additional("dbus_shared_sources")
    utilities = meson_list(recipe, "dbus_util_sources") + additional("dbus_util_sources") + ["dbus-spawn-unix.c"]
    daemon = meson_list((source / "bus/meson.build").read_text(), "libdbus_daemon_internal_sources") + ["dir-watch-kqueue.c"]
    helper = meson_list((source / "bus/meson.build").read_text(), "liblaunch_helper_internal_sources")
    return {"libdbus-1": [source / "dbus" / name for name in client],
            "dbus-internal": [source / "dbus" / name for name in utilities],
            "daemon-internal": [source / "bus" / name for name in daemon],
            "launch-helper-internal": [source / "bus" / name for name in helper]}


def programs(source):
    recipe = (source / "tools/meson.build").read_text()
    result = {"dbus-daemon": [source / "bus/main.c"],
              "dbus-daemon-launch-helper": [source / "bus" / name for name in ("activation-helper.c", "activation-helper-bin.c")]}
    for name in ("dbus-cleanup-sockets", "dbus-launch", "dbus-monitor", "dbus-run-session", "dbus-send", "dbus-test-tool", "dbus-update-activation-environment", "dbus-uuidgen"):
        if name == "dbus-launch":
            files = ["dbus-launch.c", "dbus-launch-x11.c", "tool-common.c"]
        else:
            match = re.search(r"executable\('" + name + r"',\s*(.*?)\binclude_directories:", recipe, re.S)
            if not match:
                raise ValueError("missing original D-Bus tool inventory: " + name)
            files = re.findall(r"'([^']+\.c)'", match[1])
        result[name] = [source / "tools" / file for file in files]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "extensions", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Applications/ipc/dbus" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-dbus-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"
        generated.mkdir()
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        merge_archive(args.extensions, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("D-Bus SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fno-common",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc",
                 "-D_DARWIN_C_SOURCE=1", "-D_FORTIFY_SOURCE=0", "-Werror=implicit-function-declaration",
                 "-Werror=incompatible-pointer-types", "-Wno-availability", "-isystem", sdk / "usr/include",
                 "-isystem", resource + "/include", "-I", install / "usr/include", "-I", generated,
                 "-I", source, "-I", source / "dbus", "-I", source / "bus"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        values = {"VERSION": '"' + VERSION + '"', "DBUS_UNIX": 1, "DBUS_COMPILATION": 1,
                  "DBUS_ENABLE_ASSERT": 1, "DBUS_ENABLE_CHECKS": 1, "DBUS_ENABLE_STATS": 1,
                  "ENABLE_TRADITIONAL_ACTIVATION": 1, "DBUS_BUILD_X11": 1, "DBUS_ENABLE_X11_AUTOLAUNCH": 1,
                  "DBUS_EXPORT": '__attribute__((visibility("default")))', "DBUS_PRIVATE_EXPORT": '__attribute__((visibility("default")))',
                  "DBUS_USE_SYNC": 1, "DBUS_EXEEXT": '""', "DBUS_PREFIX": '"/usr"',
                  "DBUS_BINDIR": '"/usr/bin"', "DBUS_DATADIR": '"/usr/share"', "DBUS_RUNSTATEDIR": '"/var/run"',
                  "DBUS_MACHINE_UUID_FILE": '"/var/lib/dbus/machine-id"', "DBUS_SYSTEM_CONFIG_FILE": '"/usr/share/dbus-1/system.conf"',
                  "DBUS_SESSION_CONFIG_FILE": '"/usr/share/dbus-1/session.conf"', "DBUS_SYSTEM_BUS_DEFAULT_ADDRESS": '"unix:path=/var/run/dbus/system_bus_socket"',
                  "DBUS_USER": '"messagebus"', "DBUS_TEST_USER": '"desktop"', "DBUS_DAEMONDIR": '"/usr/bin"', "DBUS_DAEMON_NAME": '"dbus-daemon"',
                  "DBUS_SESSION_SOCKET_DIR": '"/tmp"', "DBUS_SESSION_BUS_CONNECT_ADDRESS": '"autolaunch:"'}
        features = {}
        def probe(name, text, required=False, boolean=False):
            path = generated / "feature.c"
            path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-Wno-error=deprecated-declarations",
                                     "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if required and result.returncode:
                raise ValueError(name + ":\n" + result.stderr.decode())
            if features[name] or boolean:
                values[name] = int(features[name])
        root_recipe = (source / "meson.build").read_text()
        for header in meson_list(root_recipe, "check_headers"):
            # Apple ASL/syslog is outside this embedded runtime. D-Bus's
            # original console logging path reports real messages to stderr.
            if header == "syslog.h":
                continue
            macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
            probe(macro, "#include <" + header + ">\n")
        common_headers = "#include <unistd.h>\n#include <stdlib.h>\n#include <pwd.h>\n#include <grp.h>\n#include <locale.h>\n#include <sys/socket.h>\n#include <sys/resource.h>\n#include <sys/time.h>\n#include <signal.h>\n#include <poll.h>\n#include <time.h>\n"
        for function in meson_list(root_recipe, "check_functions"):
            probe("HAVE_" + function.upper(), common_headers + "void *f=(void *)&" + function + ";\n")
        for name, text in {
            "HAVE_SOCKLEN_T": "#include <sys/socket.h>\nsocklen_t f;\n",
            "HAVE_CMSGCRED": "#include <sys/socket.h>\nstruct cmsgcred f;\n",
            "HAVE_UNPCBID": "#include <sys/un.h>\nint f(struct unpcbid *p){return p->unp_pid;}\n",
            "HAVE_WRITEV": "#include <sys/uio.h>\nvoid *f=(void *)&writev;\n",
            "HAVE_UNIX_FD_PASSING": "#include <sys/socket.h>\nint f=SCM_RIGHTS;\nstruct cmsghdr h;\n",
            "HAVE_MONOTONIC_CLOCK": "#include <pthread.h>\n#include <time.h>\nint f(pthread_condattr_t *a,struct timespec *p){return pthread_condattr_setclock(a,CLOCK_MONOTONIC)+clock_getres(CLOCK_MONOTONIC,p);}\n",
            "HAVE_XML_SETHASHSALT": "#include <expat.h>\nvoid *f=(void *)&XML_SetHashSalt;\n",
            "NATIVE_KQUEUE": "#include <sys/event.h>\nvoid *f=(void *)&kqueue;\n",
            "Darwin_LP64": "#include <stdint.h>\n_Static_assert(__APPLE__ && __MACH__ && sizeof(short)==2 && sizeof(int)==4 && sizeof(long)==8 && sizeof(void*)==8,\"Darwin LP64\");\n",
        }.items():
            probe(name, text, required=name in ("HAVE_WRITEV", "HAVE_UNIX_FD_PASSING", "HAVE_XML_SETHASHSALT", "NATIVE_KQUEUE", "Darwin_LP64"))
        for name, header, symbol in (("HAVE_DECL_ENVIRON", "unistd.h", "environ"), ("HAVE_DECL_MSG_NOSIGNAL", "sys/socket.h", "MSG_NOSIGNAL"),
                                    ("HAVE_DECL_LOG_PERROR", "syslog.h", "LOG_PERROR"), ("HAVE_DECL_SYS_PIDFD_OPEN", "sys/syscall.h", "SYS_pidfd_open")):
            probe(name, f"#include <{header}>\nvoid *f=(void *)&{symbol};\n" if symbol == "environ" else f"#include <{header}>\nint f={symbol};\n", boolean=True)
        (generated / "config.h").write_text("/* Real native Darwin feature selection. */\n" +
            "".join(f"#define {name} {value}\n" for name, value in sorted(values.items())))
        public = install / "usr/include/dbus-1.0/dbus"
        public.mkdir(parents=True)
        headers = re.search(r"install_headers\((.*?)\bsubdir:", (source / "dbus/meson.build").read_text(), re.S)[1]
        for header in re.findall(r"'([^']+\.h)'", headers):
            shutil.copyfile(source / "dbus" / header, public / header)
        arch_values = {"DBUS_INT64_TYPE": "long", "DBUS_INT64_CONSTANT": "(val##L)", "DBUS_UINT64_CONSTANT": "(val##UL)",
                       "DBUS_INT64_MODIFIER": "l", "DBUS_INT32_TYPE": "int", "DBUS_INT16_TYPE": "short", "DBUS_SIZEOF_VOID_P": 8,
                       "DBUS_MAJOR_VERSION": 1, "DBUS_MINOR_VERSION": 16, "DBUS_MICRO_VERSION": 2, "DBUS_VERSION": VERSION}
        configured = re.sub(r"@(\w+)@", lambda m: str(arch_values[m[1]]), (source / "dbus/dbus-arch-deps.h.in").read_text())
        (public / "dbus-arch-deps.h").write_text(configured)
        (generated / "dbus").mkdir()
        (generated / "dbus/dbus-arch-deps.h").write_text(configured)
        flags += ["-I", public.parent, "-fvisibility=hidden"]
        # The original generic getpeereid branch records UID only. Darwin
        # also exposes the connected peer's real PID through LOCAL_PEERPID.
        # Compile a private source copy with that native branch; preserve
        # every release file and never fabricate missing credential values.
        peer_source = source / "dbus/dbus-sysdeps-unix.c"
        peer_copy = generated / "dbus-sysdeps-unix.c"
        original = peer_source.read_text()
        anchor = "#elif defined(HAVE_GETPEEREID)\n"
        if original.count(anchor) != 1:
            raise ValueError("D-Bus credential branch changed")
        native_peer = '''#elif defined(__APPLE__) && defined(LOCAL_PEERPID) && defined(HAVE_GETPEEREID)
    uid_t euid;
    gid_t egid;
    pid_t peer_pid;
    socklen_t peer_pid_length = sizeof (peer_pid);
    if (getpeereid (client_fd.fd, &euid, &egid) == 0)
      uid_read = euid;
    else
      _dbus_verbose ("Failed to getpeereid() credentials: %s\\n", _dbus_strerror (errno));
    if (getsockopt (client_fd.fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &peer_pid_length) == 0
        && peer_pid_length == sizeof (peer_pid) && peer_pid > 0)
      pid_read = peer_pid;
    else
      _dbus_verbose ("Failed to read Darwin peer PID: %s\\n", _dbus_strerror (errno));

'''
        peer_copy.write_text(original.replace(anchor, native_peer + anchor))
        records = {}
        groups = inventories(source)
        program_sources = programs(source)
        groups.update({"program-" + name: files for name, files in program_sources.items()})
        for name, files in groups.items():
            jobs = [(path, generated / (name + f"_{index:03d}.o")) for index, path in enumerate(files)]
            def compile_one(job):
                path, obj = job
                compiled = peer_copy if path == peer_source else path
                try:
                    run([*flags, "-c", compiled, "-o", obj])
                except RuntimeError as error:
                    return {"error": str(error)}
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("non-native D-Bus object")
                return {"source": str(path.relative_to(root)), "source_sha256": digest(path), "compiled_source_sha256": digest(compiled),
                        "member": obj.name, "object_sha256": digest(obj)}
            with ThreadPoolExecutor(max_workers=8) as pool:
                entries = list(pool.map(compile_one, jobs))
            errors = [entry["error"] for entry in entries if "error" in entry]
            if errors:
                raise ValueError(name + " compilation failed:\n" + "\n".join(errors))
            archive = install / ("usr/lib" if name == "libdbus-1" else "usr/libexec/dbus") / (name + ".a")
            archive.parent.mkdir(parents=True, exist_ok=True)
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            defined, imports, weak = symbols(archive)
            records[name] = {"archive_path": str(archive.relative_to(install)), "sources": entries, "exports": sorted(defined),
                             "imports": sorted(imports - definitions(archive)), "weak_imports": sorted(weak)}
        data_values = {"DBUS_USER": "messagebus", "DBUS_LIBEXECDIR": "/usr/libexec", "DBUS_SYSTEM_PID_FILE": "/var/run/dbus/pid",
                       "DBUS_SYSTEM_BUS_DEFAULT_ADDRESS": "unix:path=/var/run/dbus/system_bus_socket",
                       "SYSCONFDIR_FROM_PKGDATADIR": "../../etc", "DBUS_SESSION_BUS_LISTEN_ADDRESS": "unix:tmpdir=/tmp",
                       "DBUS_SESSION_CONF_MAYBE_AUTH_EXTERNAL": "<auth>EXTERNAL</auth>"}
        data = install / "usr/share/dbus-1"
        data.mkdir(parents=True)
        for name in ("session", "system"):
            (data / (name + ".conf")).write_text(re.sub(r"@(\w+)@", lambda m: data_values[m[1]], (source / "bus" / (name + ".conf.in")).read_text()))
        license_dir = install / "usr/share/licenses/dbus"
        license_dir.mkdir(parents=True)
        shutil.copyfile(source / "COPYING", license_dir / "COPYING")
        defined, undefined = set(), set()
        for archive in install.rglob("*.a"):
            defined |= definitions(archive)
            undefined |= symbols(archive)[1]
        for path in (install / "usr/libexec").glob("*.o"):
            undefined |= symbols(path)[1]
        requests = sorted(undefined - defined - {"___dso_handle"})
        args.imports.write_text("\n".join(requests) + "\n")
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION, "libraries": {"libdbus-1": records.pop("libdbus-1")},
            "internal_archives": records, "programs": sorted(program_sources), "configuration": values, "sdk_features": features,
            "architecture_configuration": arch_values, "system_requests": requests, "bundle_sha256": digest(args.output), "runtime_checked": False,
            "native_source_adaptations": {str(peer_source.relative_to(root)): "Real Darwin getpeereid UID and getsockopt(LOCAL_PEERPID) PID; private compilation copy"},
            "disabled_optional_backends": ["Apple ASL/syslog (original stderr logging enabled)", "Apple launchd IPC", "Linux epoll/inotify/systemd/LSM/audit", "Windows"]}, indent=2) + "\n")
        print(f"Native D-Bus {args.arch}: complete Unix client library, daemon/helpers and {len(program_sources)} programs")


if __name__ == "__main__":
    main()
