"""Source-built native LibSystem packages with explicit application API closure."""

def native_system(name, arch, exports, libraries = {}, component_libraries = {}):
    """Create name.tar, name.dylib and name.json for an actual export request.

    exports is a newline list of Mach-O names (including their leading '_').
    Extract the tar before linking; pass its usr/lib/libSystem.B.dylib and
    -L <extracted>/usr/lib/system to ld64.lld so it can read the reexports.
    Install the complete tar plus the source-built dyld in the guest root.
    component_libraries maps built-in component names to lists of real source
    archives that must link inside that component (including private symbols).
    """
    if arch not in ["arm64", "x86_64"]:
        fail("native LibSystem requires arm64 or x86_64")
    components = ["kernel", "platform", "pthread", "libc", "malloc", "math", "blocks", "crypto", "tlv", "rng", "bootstrap"]
    for component in component_libraries:
        if component not in components:
            fail("unknown built-in native component: " + component)
    component_inputs = [label for labels in component_libraries.values() for label in labels]
    inputs = {
        "sdk": "//userland/native:sdk_" + arch,
        "mach-headers": "//userland/native:mach.headers." + arch + ".tar",
        "dyld": "//userland/native:dyld_" + arch,
        "libsyscall": "//userland/native:libsyscall_" + arch,
        "libmach": "//userland/native:libmach_base." + arch + ".a",
        "libplatform": "//userland/native:libplatform_" + arch,
        "libpthread": "//userland/native/pthread:libpthread." + arch + ".a",
        "libpthread-dyld": "//userland/native/pthread:libpthread_dyld." + arch + ".a",
        "libc": "//userland/native/libc:libc_base." + arch + ".a",
        "libmalloc": "//userland/native/malloc:libmalloc_base." + arch + ".a",
        "libmath": "//userland/native/math:libmath." + arch + ".a",
        "libtlv": "//userland/native/tls:libtls." + arch + ".a",
        "librng": "//userland/native/rng:libnative_rng." + arch + ".a",
        "exports": exports,
        "helpers": "//userland/native/system:helpers_" + arch + ".tar",
    }
    native.genrule(
        name = name,
        srcs = list(inputs.values()) + list(libraries.values()) + component_inputs + [
            "//scripts:build_native_system.py", "//scripts:native_components.py",
            "//scripts:sign_diagnostic_macho.py", "//userland/native/system:startup_sources",
            "//src/Libraries/25.5.0:sources", "//src/Kernel/xnu/12377.121.6:sources",
            "//tools/cctools:cctools_header_files",
        ],
        outs = [name + ".tar", name + ".dylib", name + ".json"],
        cmd = "python3 $(location //scripts:build_native_system.py) --root $$PWD --arch " + arch +
              " --compiler-tls " +
              " ".join(["--" + option + " $(location " + label + ")" for option, label in inputs.items()]) +
              " ".join([" --library " + component + "=$(location " + label + ")" for component, label in libraries.items()]) +
              " ".join([" --component-library " + component + "=$(location " + label + ")"
                        for component, labels in component_libraries.items() for label in labels]) +
              " --output $(location " + name + ".tar)" +
              " --facade $(location " + name + ".dylib)" +
              " --report $(location " + name + ".json)",
    )
