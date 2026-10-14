#!/usr/bin/env python3
"""Build XNU's native Mach IPC, VM and runtime initialization implementation.

This archive complements libsyscall_base; it contains real MIG clients and
Apple's native wrappers. It is not a complete libSystem. IOKit error-string
tables and their two formatting entry points are excluded until their genuine
USB/FireWire SDK headers are available. No replacement implementation is used.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile


INTERNAL_INTERFACES = {"mach_port", "mach_vm", "task", "thread_act", "vm_map"}
# Match Libsyscall.xcodeproj's MIG source list. ledger.defs is not in its target.
INTERFACES = (
    "clock", "clock_priv", "clock_reply", "exc", "host_priv", "host_security",
    "mach_eventlink", "mach_host", "mach_port", "mach_vm", "mach_voucher",
    "memory_entry", "notify", "processor", "processor_set", "restartable",
    "task", "thread_act", "vm_map", "servers/netname",
)
WRAPPERS = (
    "init_cpu_capabilities.c", "__get_cpu_capabilities.s", "mach_absolute_time.s",
    "mach_approximate_time.c", "mach_approximate_time.s", "mach_continuous_time.c",
    "mach_timebase_info.c", "_libkernel_init.c", "_libc_funcptr.c",
    "system-version-compat.c", "system-version-compat-support.c",
    "terminate_with_reason.c", "unix03/getrlimit.c", "spawn/posix_spawn.c", "kdebug_trace.c",
    "cancelable/fcntl.c", "cancelable/fcntl-cancel.c", "libproc/libproc.c",
)


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"native Mach command failed: {command[0]}\n{result.stdout}{result.stderr}")
    return result.stdout


def generate_interfaces(root, arch, sdk, destination, migcom, jobs=8):
    """Generate public/private headers and native clients from real XNU defs.

    The upstream mach_install_mig.sh explicitly requires 32-bit header
    generation even for 64-bit products. Native client generation needs its own
    headers because its message layouts and local VM aliases depend on LP64.
    """
    xnu = root / "src/Kernel/xnu/12377.121.6"
    library = xnu / "libsyscall"
    headers = destination / "headers"
    clients = destination / "clients"
    client_headers = destination / "client_headers"
    for directory in (headers / "usr/include/mach", headers / "usr/include/servers",
                      headers / "usr/local/include/mach", clients, client_headers):
        directory.mkdir(parents=True, exist_ok=True)
    header_arch = {"arm64": "armv7", "x86_64": "i386"}[arch]
    environment = dict(os.environ, MIGCC="clang-20", MIGCOM=str(migcom.resolve()))
    common = ["bash", root / "tools/mig/mig.sh", "-novouchers", "-DKOBJECT_SERVER",
              "-I" + str(xnu / "osfmk"), "-I" + str(sdk / "usr/include"), "-nostdinc",
              "-server", "/dev/null"]
    header_flags = ["-target", f"{header_arch}-apple-darwin", "-arch", header_arch]

    def generate(interface):
        name = Path(interface).name
        source = library / "mach" / (interface + ".defs")
        # mach/notify.h is a normal SDK header defining notification types.
        # The generated notify client declaration must not shadow that header.
        filename = name + ("_user.h" if name == "notify" else ".h")
        folder = "servers" if name == "netname" else "mach"
        public = headers / "usr/include" / folder / filename
        run([*common, *header_flags, "-DLIBSYSCALL_INTERFACE", "-header", public,
             "-user", "/dev/null", source], env=environment)
        internal = public
        if name in INTERNAL_INTERFACES:
            internal = headers / "usr/local/include/mach" / (name + "_internal.h")
            run([*common, *header_flags, "-header", internal,
                 "-user", "/dev/null", source], env=environment)
        client = clients / (name + "User.c")
        run([*common, "-target", f"{arch}-apple-macos26.5", "-arch", arch,
             "-header", client_headers / internal.name, "-user", client, source],
            env=environment)
        return client

    with ThreadPoolExecutor(max_workers=jobs) as workers:
        sources = list(workers.map(generate, INTERFACES))
    return sources, headers, client_headers


def write_headers(directory, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-mach-headers-", dir=output.parent) as temporary:
        result = Path(temporary) / "headers.tar"
        with tarfile.open(result, "w") as archive:
            for path in sorted(directory.rglob("*")):
                info = archive.gettarinfo(str(path), str(path.relative_to(directory)))
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                if info.isfile():
                    info.mode = 0o644
                    with path.open("rb") as source:
                        archive.addfile(info, source)
                else:
                    info.mode = 0o777 if info.issym() else 0o755
                    archive.addfile(info)
        result.replace(output)


def build(root, sdk_archive, arch, output, migcom, headers_output=None, jobs=8):
    root, output = root.resolve(), output.resolve()
    library = root / "src/Kernel/xnu/12377.121.6/libsyscall"
    output.parent.mkdir(parents=True, exist_ok=True)
    resource = run(["clang-20", "-print-resource-dir"]).strip()
    cpu = {"arm64": 0x0100000c, "x86_64": 0x01000007}[arch]
    with tempfile.TemporaryDirectory(prefix=".native-mach-", dir=output.parent) as temporary:
        work = Path(temporary)
        sdk, objects = work / "sdk", work / "objects"
        objects.mkdir()
        with tarfile.open(sdk_archive) as archive:
            archive.extractall(sdk, filter="data")
        manifest = json.loads((sdk / "DarwinSDK.json").read_text())
        if manifest.get("architecture") != arch:
            raise RuntimeError(f"native SDK architecture does not match {arch}")
        generated, headers, client_headers = generate_interfaces(
            root, arch, sdk, work / "mig", migcom, jobs)
        # No duplicate trap/errno objects: those live in libsyscall_base.
        excluded = {"error_codes.c", "mach_error.c", "mach_error_string.c"}
        sources = generated + [p for p in sorted((library / "mach").glob("*.c"))
                               if p.name not in excluded]
        sources += [library / "wrappers" / name for name in WRAPPERS]
        sources += sorted((library / "wrappers/string").glob("*.c"))
        sources += [library / "os/x18.c", library / "os/alloc_once.c"]
        # libsyscall deliberately overrides the SDK string prototypes with
        # its own native bootstrap implementations and forwarding functions.
        includes = [library / "mach", library / "os", library / "wrappers",
                    library / "wrappers/string", library / "wrappers/spawn",
                    library / "wrappers/libproc", library / "custom",
                    headers / "usr/include", headers / "usr/local/include",
                    sdk / "usr/include", sdk / "usr/local/include",
                    sdk / "System/Library/Frameworks/System.framework/Versions/B/PrivateHeaders"]
        flags = [f"--target={arch}-apple-macos26.5", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fno-stack-check", "-nostdinc", "-O2",
                 "-DPRIVATE=1", "-DLIBSYSCALL_INTERFACE", "-D__DARWIN_VERS_1050=1",
                 "-D__DARWIN_UNIX03=1",
                 "-DNO_SYSCALL_LEGACY", "-D_FORTIFY_SOURCE=0", "-Wno-availability",
                 "-Wno-macro-redefined", "-Wno-nullability-completeness",
                 # The same suppression is in Apple's Libsyscall.xcconfig.
                 "-Wno-int-conversion"]
        if arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        else:
            flags += ["-DSYSCALL_PRE1050"]
        for include in includes:
            flags += ["-I", str(include)]
        flags += ["-isystem", resource + "/include"]

        def compile_source(index_source):
            index, source = index_source
            object_path = objects / f"{index:03d}_{source.name}.o"
            extra = (["-I", str(client_headers)] if source in generated else [])
            # Generated clients include their sibling interface headers by
            # basename; handwritten wrappers use the public/private SDK form.
            extra += ["-I", str(headers / "usr/include/mach"),
                      "-I", str(headers / "usr/include/servers"),
                      "-I", str(headers / "usr/local/include/mach")]
            language = "assembler-with-cpp" if source.suffix == ".s" else "c"
            run(["clang-20", *extra, *flags, "-x", language, "-c", source, "-o", object_path])
            data = object_path.read_bytes()
            if len(data) < 32:
                raise RuntimeError(f"truncated native Mach-O object: {source}")
            magic, object_cpu, _, kind = struct.unpack_from("<4I", data)
            if (magic, kind) != (0xfeedfacf, 1):
                raise RuntimeError(f"expected a native Mach-O object: {source}")
            if object_cpu != cpu:
                raise RuntimeError(f"foreign CPU in native Mach object: {source}")
            return object_path

        with ThreadPoolExecutor(max_workers=jobs) as workers:
            built = list(workers.map(compile_source, enumerate(sources)))
        archive = work / "libmach_base.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *built])
        if sorted(run(["llvm-ar-20", "t", archive]).splitlines()) != sorted(p.name for p in built):
            raise RuntimeError("native Mach archive inventory mismatch")
        symbols = {line.split()[-1] for line in run(
            ["llvm-nm-20", "--defined-only", archive]).splitlines() if line and not line.endswith(":")}
        required = {"_mach_init", "_mach_msg2_internal", "_mach_vm_allocate", "_mach_vm_map",
                    "_mach_port_allocate", "_mach_port_deallocate", "_mig_get_reply_port",
                    "_mach_absolute_time", "_mach_timebase_info", "___libkernel_init",
                    "__os_alloc_once_table",
                    "_abort_with_reason", "_getrlimit", "_posix_spawnattr_get_qos_clamp_np",
                    "_posix_spawnattr_set_qos_clamp_np",
                    "_kdebug_trace",
                    "__kernelrpc_mach_vm_allocate", "_vm_page_size", "_mach_task_self_"}
        if missing := required - symbols:
            raise RuntimeError(f"native Mach archive missing exports: {sorted(missing)}")
        if headers_output:
            (headers / "DarwinMachInterfaces.json").write_text(json.dumps({
                "architecture": arch, "xnu": manifest["projects"]["xnu"],
                "interfaces": INTERFACES,
            }, indent=2, sort_keys=True) + "\n")
            write_headers(headers, headers_output)
        archive.replace(output)
        print(f"Built native {arch} Mach runtime: {len(built)} verified Mach-O objects; "
              f"{len(generated)} real MIG clients")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True, help="native userspace SDK tar")
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--migcom", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--headers-output", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build(args.root, args.sdk, args.arch, args.output, args.migcom, args.headers_output, args.jobs)


if __name__ == "__main__":
    main()
