#!/usr/bin/env python3
"""Build the pinned current XNU on Linux in an isolated, writable source copy."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = "12377.121.6"


def copy_tree(source, destination):
    for path in source.rglob("*"):
        if path.is_file():
            target = destination / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or target.read_bytes() != path.read_bytes():
                shutil.copy2(path, target)


def write(path, contents, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != contents:
        path.write_text(contents)
    if executable:
        path.chmod(0o755)

def report_failure(log):
    lines = log.read_text(errors="replace").splitlines()
    diagnostics = [line for line in lines if any(
        marker in line for marker in ("error:", "Undefined symbols", "not found",
                                      "No rule to make target", "***"))]
    print("\n".join(diagnostics[-60:]))
    print("\n".join(lines[-20:]))


def main():
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--symbols-output", type=Path)
    parser.add_argument("--headers-output", type=Path)
    parser.add_argument("--stage", choices=("headers", "kernel"), default="kernel")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--bin-dir", type=Path)
    parser.add_argument("--host-ld", type=Path,
                        help="derive the host tool output directory from Bazel's ld executable")
    args = parser.parse_args()
    ROOT = args.root.resolve()
    bin_dir = args.bin_dir
    if not bin_dir and args.host_ld:
        bin_dir = args.host_ld.absolute().parents[2]
    bin_dir = (bin_dir or ROOT / "bazel-bin").resolve()
    work = args.workdir.resolve()
    src, obj, sdk, tools = (work / name for name in ("src", "obj", "sdk", "tools"))
    copy_tree(ROOT / "src/Kernel/xnu" / VERSION, src)
    # Public kernel banners identify the project instead of the build account.
    version_script = src / "config/newvers.pl"
    version_text = version_script.read_text()
    builder = "my $BUILDER=`whoami`;"
    if version_text.count(builder) != 1:
        raise SystemExit("XNU kernel builder declaration changed")
    write(version_script, version_text.replace(builder,
          'my $BUILDER="DarwinEmbedded"; # Modified for public build identity, 2026-10-13.'))
    if args.arch == "arm64":
        # BCM2711 has no Apple AMCC. These unpublished Apple hardware drivers
        # appear in the public build list but have no source in this release.
        files = src / "osfmk/conf/files.arm64"
        write(files, "\n".join(line for line in files.read_text().splitlines()
                              if not line.startswith("osfmk/arm64/amcc_rorgn")) + "\n")
        # Omit exports for hardware absent from BCM2711. Real undefined
        # references still fail linking; this only changes the private API list.
        excluded_path = src / "config/BCM2711.unsupported-exports"
        excluded = {line for line in excluded_path.read_text().splitlines()
                    if line and not line.startswith("#")}
        exports = src / "config/Private.arm64.exports"
        original = exports.read_text().splitlines()
        if excluded - set(original):
            raise SystemExit("BCM2711 unsupported export list contains unknown symbols")
        additional = (src / "config/BCM2711.additional-exports").read_text().splitlines()
        write(exports, "\n".join(line for line in original if line not in excluded) +
              "\n" + "\n".join(additional) + "\n")
    doconf = src / "SETUP/config/doconf"
    tcsh = shutil.which("tcsh")
    if not tcsh:
        raise SystemExit("XNU configuration requires tcsh; install the tcsh host package")
    write(doconf, doconf.read_text().replace("#!/bin/csh -f", f"#!{tcsh} -f"), executable=True)
    copy_tree(ROOT / "src/Kernel/KernelHeaders/bootstrap", sdk / "usr/include")
    availability = ROOT / "src/Libraries/25.5.0/AvailabilityVersions"
    copy_tree(availability / "include", sdk / "usr/include")
    av_tool = sdk / "usr/local/libexec/availability.pl"
    av_tool.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["python3", str(availability / "availability"), "--av_version", "157.2",
                    "--preprocess", str(availability / "availability"), str(av_tool)], check=True)
    av_tool.chmod(0o755)
    for template in (availability / "templates").glob("*.h"):
        dest = sdk / "usr/include" / template.name
        if template.name == "os_availability.h":
            dest = sdk / "usr/include/os/availability.h"
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["python3", str(av_tool), "--preprocess", str(template), str(dest)], check=True)
        # XNU's vendored availability headers must match the SDK generation.
        if template.name.startswith("Availability"):
            shutil.copy2(dest, src / "EXTERNAL_HEADERS" / template.name)
    dispatch = ROOT / "src/Libraries/25.5.0/libdispatch"
    pthread = ROOT / "src/Libraries/25.5.0/libpthread"
    copy_tree(dispatch / "os", sdk / "usr/local/include/kernel/os")
    copy_tree(pthread / "include", sdk / "usr/local/include/kernel")
    for makefile in src.rglob("Make*"):
        if not makefile.is_file():
            continue
        contents = makefile.read_text()
        contents = contents.replace("/usr/sbin/sysctl", "sysctl")
        contents = contents.replace("/usr/bin/xcrun", "xcrun")
        contents = contents.replace("/usr/bin/install", "install")
        write(makefile, contents)
    driverkit_makefile = src / "iokit/DriverKit/Makefile"
    write(driverkit_makefile, driverkit_makefile.read_text() +
          "\n$(GENERATED_HEADERS): $(IIG)\n$(GENERATED_IMPL): $(IIG)\n")
    clang = shutil.which("clang-20") or shutil.which("clang")
    clangxx = shutil.which("clang++-20") or shutil.which("clang++")
    resource = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()
    q = shlex.quote
    target = f"{args.arch}-apple-macos26.5"
    extra = [f"--target={target}", "-fno-use-cxa-atexit", "-fno-register-global-dtors-with-atexit",
             "-fblocks", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
             "-fno-unwind-tables", "-isystem", resource + "/include",
             "-idirafter", str(sdk / "usr/include"),
             "-include", str(src / "pd_apple_builtins.h")]
    extra += ["-DOS_FIREHOSE_SPI=1"]
    # The public embedded profile has no Apple Image4/AMFI trust-cache runtime.
    # Trust-cache load/query operations remain unsupported and never succeed.
    extra += ["-DDARWIN_EMBEDDED_NO_APPLE_TRUST_CACHE=1"]
    # XNU runs __mod_term_func entries itself; process atexit is unavailable.
    extra += ["-mllvm", "-disable-atexit-based-global-dtor-lowering"]
    # Stock Clang enables new diagnostics absent from Apple's build toolchain.
    extra += ["-Wno-reserved-identifier", "-Wno-unsafe-buffer-usage"]
    if args.arch == "arm64":
        extra += ["-mcpu=cortex-a72", "-mno-outline-atomics",
                  "-DVM_KERNEL_KC_ADDRESS=0xfffffff006c00000"]
    for name, compiler in (("clang-darwin", clang), ("clangxx-darwin", clangxx)):
        write(tools / name, '#!/bin/bash\nargs=()\nfor arg in "$@"; do\n'
              'case "$arg" in -mno-implicit-sme) continue;; esac\n'
              'args+=("$arg")\ndone\nexec ' +
              q(compiler) + " " + " ".join(map(q, extra)) + ' "${args[@]}"\n', executable=True)
    host_options = ["-D__private_extern__=", "-D__LITTLE_ENDIAN__", "-D__APPLE__", "-D__MACH__",
                    "-I" + str(ROOT / "tools/cctools/include/foreign"),
                    "-I" + str(ROOT / "tools/cctools/include")]
    host_libs = [str(bin_dir / "tools/cctools/liblibmacho.a"),
                 str(bin_dir / "tools/cctools/liblibstuff.a"), "-lstdc++", "-luuid"]
    write(tools / "host-cc", "#!/bin/bash\nargs=()\nwhile (( $# )); do\n"
          "case \"$1\" in -mmacosx-version-min=*) shift; continue;; -isysroot) shift 2; continue;; esac\n"
          "args+=(\"$1\"); shift\ndone\nexec gcc " + " ".join(map(q, host_options)) +
          ' "${args[@]}" ' + " ".join(map(q, host_libs)) + "\n", executable=True)
    write(tools / "sysctl", '#!/bin/bash\ncase "$*" in *hw.memsize*) echo 17179869184;; '
          '*hw.ncpu*|*hw.physicalcpu*|*hw.logicalcpu*) echo 8;; *) exec /usr/sbin/sysctl "$@";; esac\n', True)
    write(tools / "xcrun", '#!/bin/bash\nwhile [[ "$1" == -* ]]; do\n'
          'case "$1" in -sdk|--sdk) shift 2;; -find|--find) shift; exec which "$1";; '
          '-show-sdk-path) echo /; exit;; -show-sdk-version) echo 26.5; exit;; '
          '*) shift;; esac\ndone\nexec "$@"\n', True)
    write(tools / "install", '#!/bin/bash\nargs=()\nfor arg in "$@"; do\n'
          '[[ "$arg" == "-S" ]] || args+=("$arg")\ndone\nexec /usr/bin/install "${args[@]}"\n', True)
    write(tools / "sw_vers", '#!/bin/bash\necho 26.5\n', True)
    tools.mkdir(parents=True, exist_ok=True)
    (tools / "gm4").unlink(missing_ok=True)
    (tools / "gm4").symlink_to(shutil.which("m4"))
    (tools / "unifdef").unlink(missing_ok=True)
    (tools / "unifdef").symlink_to(bin_dir / "tools/unifdef/unifdef")
    obj.mkdir(parents=True, exist_ok=True)
    board = "NONE" if args.arch == "x86_64" else "BCM2711"
    values = {
        "SRCROOT": src, "OBJROOT": obj, "DSTROOT": work / "dst", "SYMROOT": work / "sym",
        "SDKROOT": sdk, "SDKROOT_RESOLVED": sdk, "HOST_SDKROOT_RESOLVED": "/",
        "PLATFORM": "MacOSX", "SDKVERSION": "26.5",
        "TARGET_CONFIGS": f"RELEASE {args.arch.upper()} {board}",
        "RC_DARWIN_KERNEL_VERSION": "25.5.0", "RC_ProjectNameAndSourceVersion": "xnu-" + VERSION,
        "RC_ProjectSourceVersion": VERSION, "BUILD_LTO": "0", "BUILD_WERROR": "0",
        "BOUND_CHECKS": "0", "BUILD_DSYM": "0", "DO_CTFMERGE": "0", "MAKEJOBS": f"-j{args.jobs}",
        "ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG": args.arch,
        "CC": tools / "clang-darwin", "CXX": tools / "clangxx-darwin",
        "HOST_CC": tools / "host-cc", "HOST_CXX": tools / "host-cc",
        "HOST_CFLAGS": "", "HOST_CXXFLAGS": "", "HOST_BISON": "bison", "HOST_FLEX": "flex",
        # Linux host executables do not use Mach-O code signing.
        "HOST_CODESIGN": "true",
        "HOST_GM4": "m4", "MIGCC": tools / "clang-darwin", "MIG": ROOT / "tools/mig/mig.sh",
        "MIGCOM": bin_dir / "tools/mig/migcom", "UNIFDEF": bin_dir / "tools/unifdef/unifdef",
        "IIG": bin_dir / "tools/iig/iig",
        "LIPO": bin_dir / "tools/cctools/lipo", "STRIP": bin_dir / "tools/cctools/strip",
        "NM": bin_dir / "tools/cctools/nm", "NMEDIT": bin_dir / "tools/cctools/nmedit",
        "LIBTOOL": bin_dir / "tools/cctools/libtool", "CTFINSERT": bin_dir / "tools/cctools/ctf_insert",
        "LD": f"{tools / 'clangxx-darwin'} -nostdlib -fuse-ld={bin_dir / 'tools/cctools/ld'}",
        "INSTALL": tools / "install", "PYTHON": "python3",
        "PLUTIL": f"python3 {ROOT / 'scripts/plutil.py'}",
        "LD_KERNEL_LIBS": "-lclang_rt.cc_kext",
    }
    env = os.environ.copy()
    env["KERNEL_BUILD_OBJROOT"] = "xnu-" + VERSION + "/RELEASE_" + args.arch.upper()
    env["PATH"] = str(tools) + os.pathsep + env["PATH"]
    command = ["make", "--output-sync=recurse", "-C", str(src), f"-j{args.jobs}"] + [f"{k}={v}" for k, v in values.items()]
    stage = "exporthdrs"
    log = work / f"{stage}.log"
    print(f"Building xnu-{VERSION} Darwin 25.5.0 {args.arch}; log: {log}", flush=True)
    with log.open("w") as stream:
        result = subprocess.run(command + [stage], env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        report_failure(log)
        raise SystemExit(result.returncode)
    if args.stage == "kernel":
        library = sdk / "usr/local/lib/kernel"
        library.mkdir(parents=True, exist_ok=True)
        firehose_obj = obj / "firehose_buffer.o"
        flags = ["-mkernel", "-fno-builtin", "-nostdinc", "-std=gnu17", "-O2",
                 "-DKERNEL=1", "-DKERNEL_PRIVATE=1", "-DXNU_KERNEL_PRIVATE=1",
                 "-DDISPATCH_USE_DTRACE=0", "-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1",
                 "-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0", "-D__LITTLE_ENDIAN__"]
        includes = [src / "osfmk", src / "bsd", src / "libkern", src / "pexpert",
                    src / "libkern/firehose", obj / "EXPORT_HDRS/libkern/firehose",
                    src / "EXTERNAL_HEADERS", dispatch, dispatch / "src/firehose",
                    sdk / "usr/local/include/kernel"]
        includes += [obj / "EXPORT_HDRS" / name for name in ("osfmk", "bsd", "libkern", "pexpert")]
        for include in includes:
            flags += ["-I", str(include)]
        if args.arch == "arm64":
            flags += ["-DARM64_BOARD_CONFIG_BCM2711", "-DARM_LARGE_MEMORY=0"]
        subprocess.run([str(tools / "clang-darwin"), *flags, "-c",
                        str(dispatch / "src/firehose/firehose_buffer.c"), "-o", str(firehose_obj)],
                       env=env, check=True)
        ar = shutil.which("llvm-ar-20") or shutil.which("llvm-ar")
        subprocess.run([ar, "rcs", str(library / "libfirehose_kernel.a"), str(firehose_obj)], check=True)
        runtime = ROOT / "src/Libraries/25.5.0/compiler-rt"
        runtime_objects = []
        for name in ("InstrProfiling", "InstrProfilingBuffer", "InstrProfilingPlatformDarwin",
                     "InstrProfilingWriter", "InstrProfilingInternal", "InstrProfilingVersionVar"):
            runtime_obj = obj / (name + ".o")
            runtime_flags = ["-mkernel", "-ffreestanding", "-nostdinc", "-O2",
                            "-DKERNEL=1", "-DKERNEL_USE", "-DCOMPILER_RT_HAS_ATOMICS=1",
                            "-I", str(runtime / "include"), "-I", str(runtime / "lib/profile"),
                            "-I", str(src / "EXTERNAL_HEADERS")]
            for directory in ("osfmk", "bsd", "libkern", "pexpert"):
                runtime_flags += ["-I", str(obj / "EXPORT_HDRS" / directory)]
            runtime_flags += ["-I", str(ROOT / "src/Libraries/25.5.0/Libc/include")]
            if args.arch == "arm64":
                runtime_flags += ["-DARM64_BOARD_CONFIG_BCM2711", "-DARM_LARGE_MEMORY=0"]
            subprocess.run([str(tools / "clang-darwin"), *runtime_flags,
                            "-c", str(runtime / "lib/profile" / (name + ".c")),
                            "-o", str(runtime_obj)], env=env, check=True)
            runtime_objects.append(str(runtime_obj))
        subprocess.run([ar, "rcs", str(library / "libclang_rt.cc_kext.a"), *runtime_objects], check=True)
        stage = "all"
        log = work / "all.log"
        with log.open("w") as stream:
            result = subprocess.run(command + [stage], env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            report_failure(log)
            raise SystemExit(result.returncode)
    if args.output and args.stage == "kernel":
        config = f"RELEASE_{args.arch.upper()}"
        if board != "NONE":
            config += "_" + board
        name = "kernel" if board == "NONE" else "kernel.release." + board.lower()
        kernel = obj / config / name
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(kernel, args.output)
        if args.symbols_output:
            args.symbols_output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(kernel.with_name(name + ".unstripped"), args.symbols_output)
        if args.headers_output:
            exported = work / "kext-sdk"
            copy_tree(sdk / "usr/include", exported / "usr/include")
            copy_tree(sdk / "usr/local/include/kernel", exported / "usr/include")
            for component in (obj / "EXPORT_HDRS").iterdir():
                if component.is_dir() and component.name != "config":
                    copy_tree(component, exported / "usr/include")
            copy_tree(src / "EXTERNAL_HEADERS", exported / "usr/include")
            # Some public-source private IOKit APIs are omitted by exporthdrs.
            # Retain generated/exported headers; fill only absent declarations.
            for header in (src / "iokit/IOKit").rglob("*.h"):
                destination = exported / "usr/include/IOKit" / header.relative_to(src / "iokit/IOKit")
                if not destination.exists():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(header, destination)
            for header in (src / "osfmk/libsa").rglob("*.h"):
                destination = exported / "usr/include/libsa" / header.relative_to(src / "osfmk/libsa")
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(header, destination)
            for component in ("osfmk/kern", "osfmk/machine", "osfmk/i386", "osfmk/arm",
                              "osfmk/arm64", "osfmk/ipc", "osfmk/vm", "bsd/sys"):
                directory = src / component
                prefix = directory.name
                for header in directory.rglob("*.h"):
                    destination = exported / "usr/include" / prefix / header.relative_to(directory)
                    if not destination.exists():
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(header, destination)
            copy_tree(obj / config / "config/System.kext", exported / "System.kext")
            args.headers_output.parent.mkdir(parents=True, exist_ok=True)
            with tarfile.open(args.headers_output, "w") as archive:
                archive.add(exported / "usr", arcname="usr")
                archive.add(exported / "System.kext", arcname="System.kext")
    print(f"Completed {stage} for {args.arch}", flush=True)


if __name__ == "__main__":
    main()
