"""Reusable, provenance-audited source helpers for native Darwin components."""
import hashlib
import re
import struct
import subprocess


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def symbols(path):
    public, undefined, weak = set(), set(), set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        if "external " not in line:
            continue
        match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
        if not match:
            raise RuntimeError("unrecognized native symbol listing: " + line)
        name = match.group(1)
        if "(undefined)" in line:
            (weak if "weak external" in line else undefined).add(name)
        elif "private external" not in line:
            public.add(name)
    return public, undefined, weak


def build_helpers(root, work, sdk, flags, arch, *, full_runtime=False, crypto_probe=False):
    """Compile actual component sources needed by native LibSystem startup."""
    libraries = root / "src/Libraries/25.5.0"
    xnu = root / "src/Kernel/xnu/12377.121.6"
    platform = libraries / "libplatform"
    libc = libraries / "Libc"
    extra = {name: [] for name in ("kernel", "platform", "libc", "blocks", "crypto")}
    provenance = []

    def compile_source(component, path, options=(), text=None):
        output = work / (component + "_" + str(len(extra[component])) + "_" + path.name + ".o")
        original = path.read_bytes()
        compile_path = path
        if text is not None:
            compile_path = work / (output.stem + path.suffix)
            compile_path.write_text(text)
        language = ["-x", "assembler-with-cpp"] if path.suffix in (".s", ".S") else []
        run([*flags, *options, *language, "-c", compile_path, "-o", output])
        magic, cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
        if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if arch == "arm64" else 0x1000007, 1):
            raise RuntimeError("non-native helper object: " + str(path))
        extra[component].append(output)
        provenance.append({"source": str(path.relative_to(root)),
                           "sha256": hashlib.sha256(original).hexdigest(),
                           "component": component,
                           "configuration": [str(option) for option in options
                                             if str(option).startswith(("-D", "-U", "-f", "-m"))],
                           "compiled_sha256": hashlib.sha256(compile_path.read_bytes()).hexdigest()})

    platform_flags = ["-DVARIANT_STATIC=0", "-DVARIANT_NO_RESOLVERS=1", "-DCONFIG_MTE=0",
                      "-Wno-int-conversion", "-I", str(platform / "private"),
                      "-I", str(platform / "include"), "-I", str(platform / "internal"),
                      "-I", str(platform / "src/os/resolver")]
    init = platform / "src/init.c"
    text = init.read_text()
    include = "#include <AppleFeatures/AppleFeatures.h>\n"
    if text.count(include) != 1:
        raise RuntimeError("unexpected Apple platform initializer")
    # This private header is included but no declaration/macro from it is used.
    compile_source("platform", init, platform_flags, text.replace(include, "", 1))
    if arch == "arm64":
        compile_source("platform", platform / "src/atomics/init.c", platform_flags)
    for relative in ("src/setjmp/generic/sigaction.c", "src/setjmp/generic/sigtramp.c"):
        compile_source("platform", platform / relative, platform_flags)
    if arch == "x86_64":
        compile_source("platform", platform / "src/setjmp/x86_64/_sigtramp.s", platform_flags)

    if full_runtime:
        setjmp = ["src/setjmp/arm64/setjmp.s"] if arch == "arm64" else [
            "src/setjmp/x86_64/setjmp.s", "src/setjmp/x86_64/_setjmp.s"]
        for relative in [*setjmp, "src/setjmp/generic/setjmperr.c"]:
            compile_source("platform", platform / relative, platform_flags)
        for name in ("flsll.c", "ffsll.c"):
            path = platform / "src/string/generic" / name
            original = path.read_text()
            guard = "#if VARIANT_DYLD && TARGET_OS_SIMULATOR"
            if original.count(guard) != 1:
                raise RuntimeError("unexpected portable bit-search source guard")
            # Apple's generic fls/ffs bodies are valid on both native CPUs.
            # Emit them here because this runtime has no private optimized
            # libsystem_platform bit-search implementation.
            compile_source("platform", path, platform_flags, original.replace(guard, "#if 1", 1))

    mach_flags = ["-DLIBSYSCALL_INTERFACE", "-D__DARWIN_VERS_1050=1", "-D__DARWIN_UNIX03=1",
                  "-DNO_SYSCALL_LEGACY", "-Wno-int-conversion",
                  "-I", str(xnu / "libsyscall/custom"), "-I", str(xnu / "libsyscall/mach"),
                  "-I", str(xnu / "osfmk"), "-I", str(sdk / "usr/include/mach"),
                  "-I", str(sdk / "usr/local/include/mach")]
    if arch == "x86_64":
        mach_flags += ["-DSYSCALL_PRE1050"]
    for relative in ("wrappers/ioctl.c", "wrappers/kill.c", "wrappers/open-base.c",
                     "wrappers/__commpage_gettimeofday.c", "wrappers/unix03/mmap.c",
                     "wrappers/unix03/munmap.c"):
        compile_source("kernel", xnu / "libsyscall" / relative, mach_flags)

    if full_runtime:
        for relative in ("cancelable/open.c", "cancelable/sigsuspend-cancel.c",
                         "cancelable/select-cancel.c", "cancelable/pselect-darwinext-cancel.c",
                         "unix03/setrlimit.c", "rename.c", "rmdir.c", "unlinkat.c", "mach_boottime.c", "remove-counter.c"):
            compile_source("kernel", xnu / "libsyscall/wrappers" / relative, mach_flags)

    # Error tables need only published USB/FireWire numeric declarations,
    # not the unrelated CoreFoundation plug-in interfaces in those headers.
    error_include = work / "error-include"
    usb = libraries / "IOUSBFamily/IOUSBFamily/Headers/USB.h"
    firewire = libraries / "IOFireWireFamily/IOFireWireFamily.kmodproj/IOFireWireFamilyCommon.h"
    for path, installed, first, last in (
        (usb, "IOKit/usb/USB.h", b"#define\tiokit_usb_err", b"/*!\n@defined IOUSBFamily message codes"),
        (firewire, "IOKit/firewire/IOFireWireLib.h", b"#define\tiokit_fw_err", b"// =================================================================\n// Pseudo address space response codes"),
    ):
        original = path.read_bytes()
        start = original.index(first)
        end = original.index(last, start)
        selected_header = original[:original.index(b"*/") + 2] + b"\n#include <libkern/OSTypes.h>\n#include <IOKit/IOReturn.h>\n" + original[start:end]
        if path == firewire:
            enum = re.search(rb"enum\s*\{[^}]*\bkFWResponseBusResetError\b[^}]*\};", original)
            if not enum:
                raise RuntimeError("published FireWire response enum missing")
            selected_header += enum.group() + b"\n"
        destination = error_include / installed
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(selected_header)
        provenance.append({"source": str(path.relative_to(root)),
                           "sha256": hashlib.sha256(original).hexdigest(),
                           "component": "kernel-error-constants",
                           "compiled_sha256": hashlib.sha256(selected_header).hexdigest()})
    error_flags = [*mach_flags, "-I", str(error_include), "-I", str(xnu / "iokit"),
                   "-I", str(xnu / "libkern")]
    for name in ("mach_error_string.c", "error_codes.c", "mach_error.c"):
        compile_source("kernel", xnu / "libsyscall/mach" / name, error_flags)

    blocks = libraries / "libdispatch/src/BlocksRuntime"
    for name in ("runtime.c", "data.c"):
        compile_source("blocks", blocks / name, ["-fblocks", "-I", str(blocks)])

    # The portable upstream SHA-256 implementation is valid on both CPUs.
    # Cortex-A72 and Penryn do not implement ARM's DIT register.
    crypto_include = work / "crypto-include"
    crypto_include.mkdir()
    (crypto_include / "corecrypto").symlink_to(xnu / "EXTERNAL_HEADERS/corecrypto", target_is_directory=True)
    crypto_flags = ["-DCC_USE_ASM=0", "-DCC_DIT_MAYBE_SUPPORTED=0", "-DCC_INTERNAL_SDK=0",
                    "-I", str(crypto_include), "-I", str(xnu / "osfmk/corecrypto")]
    for name in ("cc_clear.c", "ccdigest_init.c", "ccdigest_update.c", "ccdigest_final_64be.c",
                 "ccsha256_di.c", "ccsha256_ltc_di.c", "ccsha256_ltc_compress.c",
                 "ccsha256_initial_state.c", "ccsha256_K.c"):
        compile_source("crypto", xnu / "osfmk/corecrypto" / name, crypto_flags)

    if crypto_probe:
        compile_source("crypto", root / "userland/native/malloc/test_crypto.c", crypto_flags)

    # Build Apple's normal cancellable API variant, not a symbol alias to the
    # non-cancellable archive entry. Its nanosleep dependency remains audited.
    libc_flags = ["-D__LIBC__=1", "-DVARIANT_CANCELABLE=1", "-D__DARWIN_NON_CANCELABLE=0",
                  "-D__DARWIN_UNIX03=1", "-fblocks", "-Wno-nullability-completeness", "-include", str(libc / "fbsdcompat/_fbsd_compat_.h"),
                  "-I", str(libc / "fbsdcompat"), "-I", str(libc / "include"),
                  "-I", str(libc / "gen/FreeBSD")]
    if full_runtime:
        # The CANCELABLE inventory is exactly variants.xcconfig's macOS list.
        # BUILDING_VARIANT shares globals with the non-cancellable base objects.
        variants = (
            "compat-43/FreeBSD/creat.c", "compat-43/sigcompat.c",
            "gen/FreeBSD/lockf.c", "gen/nanosleep.c", "gen/FreeBSD/pause.c",
            "gen/FreeBSD/sleep.c", "gen/FreeBSD/termios.c", "gen/FreeBSD/usleep.c",
            "gen/FreeBSD/wait.c", "gen/FreeBSD/waitpid.c",
            "net/FreeBSD/recv.c", "net/FreeBSD/send.c", "stdlib/FreeBSD/system.c",
        )
        for relative in variants:
            compile_source("libc", libc / relative,
                           [*libc_flags, "-DBUILDING_VARIANT=1", "-Wno-deprecated-declarations"])
        # vDarwinExtsn inherits the real base NON_CANCELABLE=1 setting.
        # fopen reserves a FILE before opening its descriptor, so its internal
        # open must use that original non-cancelling variant.
        fopen_flags = [flag.replace("-D__DARWIN_NON_CANCELABLE=0", "-D__DARWIN_NON_CANCELABLE=1")
                       for flag in libc_flags if flag != "-DVARIANT_CANCELABLE=1"]
        # The published extended getgroups API reads account membership,
        # rather than aliasing the kernel's current credential vector. Keep
        # Apple's actual wrapper and supply its real file-backed group count.
        compile_source("libc", libc / "sys/getgroups.c",
                       [*fopen_flags, "-DBUILDING_VARIANT=1", "-DVARIANT_DARWINEXTSN=1",
                        "-Wno-deprecated-declarations"])
        compile_source("libc", libc / "stdio/FreeBSD/fopen.c",
                       [*fopen_flags, "-DBUILDING_VARIANT=1", "-DVARIANT_DARWINEXTSN=1",
                        "-Wno-deprecated-declarations",
                        "-I", str(libc / "stdio"), "-I", str(libc / "stdio/FreeBSD"),
                        "-I", str(libc / "locale"), "-I", str(libc / "locale/FreeBSD"),
                        "-I", str(libc / "stdtime/FreeBSD")])
        compile_source("libc", libc / "stdio/FreeBSD/fdopen.c",
                       [*fopen_flags, "-DBUILDING_VARIANT=1", "-DVARIANT_DARWINEXTSN=1",
                        "-Wno-deprecated-declarations",
                        "-I", str(libc / "stdio"), "-I", str(libc / "stdio/FreeBSD"),
                        "-I", str(libc / "locale"), "-I", str(libc / "locale/FreeBSD"),
                        "-I", str(libc / "stdtime/FreeBSD")])
        compile_source("libc", libc / "gen/FreeBSD/popen.c",
                       [*fopen_flags, "-DBUILDING_VARIANT=1", "-DVARIANT_DARWINEXTSN=1",
                        "-Wno-deprecated-declarations",
                        "-I", str(libc / "stdio"), "-I", str(libc / "stdio/FreeBSD"),
                        "-I", str(libc / "locale"), "-I", str(libc / "locale/FreeBSD"),
                        "-I", str(libc / "stdtime/FreeBSD")])
    else:
        compile_source("libc", libc / "gen/FreeBSD/sleep.c", libc_flags)
        compile_source("libc", libc / "gen/nanosleep.c", [*libc_flags, "-DBUILDING_VARIANT=1"])
    timezone_config = work / "timezone_config.h"
    timezone_config.write_text("#include <sys/cdefs.h>\n#undef NOTIFY_TZ\n")
    timezone_flags = [flag.replace("-D__DARWIN_NON_CANCELABLE=0", "-D__DARWIN_NON_CANCELABLE=1")
                      for flag in libc_flags if flag != "-DVARIANT_CANCELABLE=1"]
    timezone_flags += ["-include", str(timezone_config), "-I", str(libc / "stdtime/FreeBSD")]
    # No notification daemon is installed in this native runtime. Select the
    # upstream feature branch that calculates timezones without that service.
    compile_source("libc", libc / "stdtime/FreeBSD/localtime.c", timezone_flags)
    return extra, provenance
