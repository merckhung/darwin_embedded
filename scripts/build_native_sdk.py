#!/usr/bin/env python3
"""Assemble Darwin userspace headers from the matching published Apple sources."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


def copy_headers(source, destination):
    """Keep source header paths, without copying test or build artifacts."""
    for header in sorted(source.rglob("*.h")):
        target = destination / header.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() or target.is_symlink():
            target.unlink()
        shutil.copyfile(header, target)


def select_xnu_platform(xnu, destination, unifdef):
    """Apply XNU's PLATFORM_UNIFDEF recipe for the MacOSX userspace SDK."""
    options = (xnu / "makedefs/MakeInc.cmd").read_text()
    values = {}
    for name in ("SUPPORTED_EMBEDDED_PLATFORMS", "SUPPORTED_SIMULATOR_PLATFORMS", "SUPPORTED_PLATFORMS"):
        match = re.search(r"^" + name + r"\s*:=\s*(.*)$", options, re.MULTILINE)
        if not match:
            raise RuntimeError(f"XNU platform list missing: {name}")
        value = match.group(1)
        for key, expansion in values.items():
            value = value.replace("$(" + key + ")", expansion)
        values[name] = value
    flags = [("-D" if name == "MacOSX" else "-U") + "XNU_PLATFORM_" + name
             for name in values["SUPPORTED_PLATFORMS"].split()]
    for header in sorted(destination.rglob("*.h")):
        if header.is_symlink() or b"XNU_PLATFORM_" not in header.read_bytes():
            continue
        result = subprocess.run([str(unifdef.resolve()), *flags, str(header)], capture_output=True)
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stderr.decode())
        header.unlink()
        header.write_bytes(result.stdout)


def assemble(root, arch, kernel_headers, destination, unifdef):
    root, destination = root.resolve(), destination.resolve()
    libraries = root / "src/Libraries/25.5.0"
    xnu = root / "src/Kernel/xnu/12377.121.6"
    with tarfile.open(kernel_headers) as archive:
        archive.extractall(destination, filter="data")
    includes = destination / "usr/include"
    private = destination / "usr/local/include"
    availability = libraries / "AvailabilityVersions"
    tool = destination / "usr/local/libexec/availability.pl"
    tool.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["python3", str(availability / "availability"), "--av_version", "157.2",
                    "--preprocess", str(availability / "availability"), str(tool)], check=True)
    tool.chmod(0o755)
    subprocess.run(["bash", str(xnu / "bsd/sys/make_symbol_aliasing.sh"), str(destination),
                    str(includes / "sys/_symbol_aliasing.h")],
                   env=dict(os.environ, DRIVERKITROOT=""), check=True)
    subprocess.run(["bash", str(xnu / "bsd/sys/make_posix_availability.sh"),
                    str(includes / "sys/_posix_availability.h")], check=True)

    # Use Libc's install recipe: its source headers contain internal-only regions
    # and platform choices which must be stripped before becoming SDK headers.
    with tempfile.TemporaryDirectory(prefix=".native-headers-") as directory:
        work = Path(directory)
        (work / "unifdef").symlink_to(unifdef.resolve())
        env = dict(os.environ, PATH=str(work) + os.pathsep + os.environ["PATH"],
                   SRCROOT=str(libraries / "Libc"), ARCHS=arch,
                   PLATFORM_NAME="macosx", VARIANT_PLATFORM_NAME="macosx",
                   DEPLOYMENT_LOCATION="YES", DSTROOT=str(destination),
                   SDK_INSTALL_HEADERS_ROOT="", SDK_INSTALL_ROOT="",
                   DERIVED_FILES_DIR=str(work / "derived"))
        for key in ("DRIVERKIT", "DRIVERKITSDK", "DRIVERKITROOT"):
            env.pop(key, None)
        try:
            subprocess.run(["bash", "-e", str(libraries / "Libc/xcodescripts/headers.sh")],
                           env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        except subprocess.CalledProcessError as failure:
            sys.stderr.buffer.write(failure.stderr)
            raise
        subprocess.run(["perl", str(libraries / "Libc/xcodescripts/generate_features.pl")],
                       env=env, check=True, stdout=subprocess.DEVNULL)
        private.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(work / "derived" / arch / "libc-features.h", private / "libc-features.h")

    for project in ("libpthread", "libplatform", "libmalloc"):
        copy_headers(libraries / project / "include", includes)
        copy_headers(libraries / project / "private", private)
    # Preserve the exact public and private legacy include paths from upstream.
    subprocess.run(["bash", str(libraries / "libpthread/xcodescripts/install-symlinks.sh")],
                   env=dict(os.environ, ACTION="installhdrs", DSTROOT=str(destination),
                            SDK_INSTALL_HEADERS_ROOT=""), check=True)
    copy_headers(xnu / "libsyscall/os", private / "os")
    copy_headers(xnu / "libsyscall/mach/mach", includes / "mach")
    shutil.copyfile(xnu / "libsyscall/wrappers/gethostuuid.h", includes / "gethostuuid.h")
    shutil.copyfile(xnu / "libsyscall/wrappers/libproc/libproc.h", includes / "libproc.h")
    shutil.copyfile(xnu / "libsyscall/wrappers/spawn/spawn.h", includes / "spawn.h")
    # libSystem sources use PRIVATE declarations that are not part of XNU's
    # kernel header export. Public, installed Mach headers retain precedence.
    copy_headers(xnu / "osfmk/mach", private / "mach")
    copy_headers(libraries / "dyld/include", includes)
    # The published dyld 1378 header uses this ExclaveKit-only annotation but
    # omits its definition. This SDK targets MacOSX, where it has no effect.
    dyld_header = includes / "mach-o/dyld.h"
    dyld_header.write_text(dyld_header.read_text().replace(
        "#include <TargetConditionals.h>",
        "#include <TargetConditionals.h>\n#define DYLD_EXCLAVEKIT_UNAVAILABLE\n", 1))
    shutil.copyfile(includes / "dyld_version_defines.h", includes / "mach-o/dyld_version_defines.h")
    shutil.copyfile(libraries / "Libsystem/alloc_once_private.h", private / "os/alloc_once_private.h")
    for cpu in ("arm", "i386"):
        target = includes / cpu / "disklabel.h"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(xnu / "bsd" / cpu / "disklabel.h", target)
    # Legacy Libc source uses the System framework spelling of this same XNU
    # header. Keep the processed native declarations in both install paths.
    (includes / "System/sys").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(includes / "sys/fsctl.h", includes / "System/sys/fsctl.h")
    (includes / "servers").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(libraries / "launchd/liblaunch/bootstrap.h", includes / "servers/bootstrap.h")
    copy_headers(libraries / "syslog/libsystem_asl.tproj/include", includes)
    for project, source_name, install_name in (
        ("Libinfo", "lookup.subproj/pwd.h", "pwd.h"),
        ("Libinfo", "lookup.subproj/grp.h", "grp.h"),
        ("Libinfo", "lookup.subproj/netdb.h", "netdb.h"),
        ("Libinfo", "gen.subproj/ifaddrs.h", "ifaddrs.h"),
        ("Libinfo", "membership.subproj/membership.h", "membership.h"),
        ("Libinfo", "membership.subproj/membershipPriv.h", "membershipPriv.h"),
        ("Libinfo", "membership.subproj/ntsid.h", "ntsid.h"),
        ("Libnotify", "notify.h", "notify.h"),
        ("Libnotify", "notify_keys.h", "notify_keys.h"),
        ("libresolv", "arpa/nameser.h", "arpa/nameser.h"),
        ("libresolv", "nameser.h", "nameser.h"),
        ("libresolv", "resolv.h", "resolv.h"),
    ):
        target = includes / install_name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            target.unlink()
        shutil.copyfile(libraries / project / source_name, target)
    copy_headers(libraries / "architecture", includes / "architecture")
    copy_headers(libraries / "libdispatch/dispatch", includes / "dispatch")
    copy_headers(libraries / "libdispatch/os", includes / "os")
    copy_headers(libraries / "openlibm/include", includes)
    for header in ("cdefs-compat.h", "types-compat.h"):
        shutil.copyfile(libraries / "openlibm/src" / header, includes / header)
    for header in ("math", "fenv", "complex"):
        shutil.copyfile(libraries / "openlibm/include" / f"openlibm_{header}.h",
                        includes / f"{header}.h")
    shutil.copyfile(root / "tools/cctools/include/mach-o/getsect.h", includes / "mach-o/getsect.h")
    # XNU publishes the logging interfaces but not the userspace log-pack
    # layout. Apple's Swift OS overlay publishes the exact legacy ABI consumed
    # by Libc's _os_crash_fmt; extract that declaration without inventing it.
    swift_os = (libraries / "swift-os/os.m").read_text()
    log_pack = re.search(r"typedef struct os_log_pack_s \{.*?\} os_log_pack_s, \*os_log_pack_t;",
                         swift_os, re.DOTALL)
    if not log_pack:
        raise RuntimeError("published Swift os_log_pack_s declaration missing")
    log_private = includes / "os/log_private.h"
    log_text = log_private.read_text()
    log_private.unlink()
    log_private.write_text(log_text + "\n"
        "#ifndef __DARWIN_SWIFT_OS_LOG_PACK_ABI__\n"
        "#define __DARWIN_SWIFT_OS_LOG_PACK_ABI__\n"
        "#include <time.h>\n" + swift_os.split("#include", 1)[0] +
        log_pack.group() + "\n#endif\n")
    # Published pthread headers still use the former xros spelling. Preserve
    # their availability annotation using AvailabilityVersions' visionOS name.
    internal_availability = includes / "AvailabilityInternal.h"
    availability_text = internal_availability.read_text()
    internal_availability.unlink()
    internal_availability.write_text(availability_text + "\n"
        "#ifndef __API_AVAILABLE_PLATFORM_xros\n"
        "#define __API_AVAILABLE_PLATFORM_xros(x) __API_AVAILABLE_PLATFORM_visionos(x)\n"
        "#endif\n")
    select_xnu_platform(xnu, destination, unifdef)
    projects = {"xnu": json.loads((xnu / "UPSTREAM.json").read_text())}
    for name in ("AvailabilityVersions", "Libc", "libpthread", "libplatform", "libmalloc", "dyld", "Libsystem",
                 "Libinfo", "Libnotify", "libresolv", "architecture", "libdispatch", "openlibm",
                 "syslog", "launchd", "swift-os"):
        projects[name] = json.loads((libraries / name / "UPSTREAM.json").read_text())
    (destination / "DarwinSDK.json").write_text(json.dumps({
        "architecture": arch, "projects": projects,
    }, indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--kernel-headers", type=Path, required=True)
    parser.add_argument("--unifdef", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-sdk-", dir=args.output.parent) as directory:
        sdk = Path(directory) / "sdk"
        assemble(args.root, args.arch, args.kernel_headers, sdk, args.unifdef)
        output = Path(directory) / "sdk.tar"
        with tarfile.open(output, "w") as archive:
            for path in sorted(sdk.rglob("*")):
                info = archive.gettarinfo(str(path), str(path.relative_to(sdk)))
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                if info.isfile():
                    info.mode = 0o755 if path.name == "availability.pl" else 0o644
                    with path.open("rb") as source:
                        archive.addfile(info, source)
                else:
                    info.mode = 0o777 if info.issym() else 0o755
                    archive.addfile(info)
        output.replace(args.output)


if __name__ == "__main__":
    main()
