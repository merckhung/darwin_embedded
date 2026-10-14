#!/usr/bin/env python3
"""Compile genuine dyld TLS setup, allocation, and assembly for native Darwin.

The existing destructor-only package remains available. This extended archive
replaces it when compiler TLS is needed, keeping one coherent class definition.
Only complete upstream methods, declarations and member layouts are selected;
registration.cpp supplies the native loader's bulk-notification integration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tlv"))
from build_tlv import extract, function, once, run, symbols


def sha(data):
    return hashlib.sha256(data).hexdigest()


def generate(source, registration):
    text, files, pieces = extract(source)
    header = files["ThreadLocalVariables.h"]
    implementation = files["ThreadLocalVariables.cpp"]
    uh = (source / "mach_o/UnsafeHeader.h").read_text()
    uc = (source / "mach_o/UnsafeHeader.cpp").read_text()
    selected = {}

    def select(name, value):
        selected[name] = value
        return value

    declarations = "\n".join(select("tls_decl_" + name, once(pattern, header)) for name, pattern in (
        ("Thunk", r"^    struct Thunk\n    \{.*?^    \};"),
        ("TLV_Thunkv2", r"^    struct TLV_Thunkv2\n    \{.*?^    \};"),
        ("TLV_Thunkv2_32", r"^    struct TLV_Thunkv2_32\n    \{.*?^    \};"),
        ("initializeThunksFromDisk", r"^    mach_o::Error[^\n]*initializeThunksFromDisk[^\n]*;"),
        ("instantiateVariable", r"^    static void\*[^\n]*instantiateVariable[^\n]*;"),
        ("findInitialContent", r"^    static void[^\n]*findInitialContent[^\n]*;"),
        ("forEachThunkSpan", r"^    mach_o::Error[^\n]*forEachThunkSpan[^\n]*;"),
        ("verbose", r"^    static const bool verbose = false;"),
    ))
    text = text.replace("class VIS_HIDDEN ThreadLocalVariables {\npublic:\n",
                        "using mach_o::UnsafeHeader;\nclass VIS_HIDDEN ThreadLocalVariables {\npublic:\n" + declarations + "\n", 1)
    # The complete original Error class and implementation are compiled separately.
    prefix = """
#include <span>
#include <string_view>
#include <utility>
#include <stdio.h>
#include <assert.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <mach/vm_prot.h>
#include "Error.h"
#include "Misc.h"
extern "C" void abort_report_np(const char*, ...) __attribute__((noreturn));
"""
    # Build a declaration-only subset of UnsafeHeader, with its exact original
    # layout and method bodies. Unrelated parsers and shared-cache code are not
    # required by disk image TLS and remain in the source distribution.
    uh_decls = []
    for name in ("SegmentInfo", "SectionInfo"):
        uh_decls.append(select("unsafe_" + name, once(r"^    struct " + name + r" \{.*?^    \};", uh)))
    for pattern in (
        r"^    bool[ \t]+hasMachOBigEndianMagic\(\) const;",
        r"^    Error[ \t]+forEachLoadCommand\([^\n]*;",
        r"^    void[ \t]+forEachLoadCommandSafe\([^\n]*;[^\n]*",
        r"^    uint64_t[ \t]+preferredLoadAddress\(\) const;",
        r"^    int64_t[ \t]+getSlide\(\) const;",
        r"^    void[ \t]+forEachSegment\(void \(\^callback\)\(const SegmentInfo& info, bool& stop\)\) const;",
        r"^    void[ \t]+forEachSection\(void \(\^callback\)\(const SectionInfo&, bool& stop\)\) const;",
        r"^    static std::string_view[ \t]+name16\(const char name\[16\]\);",
        r"^    mach_header[ \t]+mh;",
    ):
        uh_decls.append(select("unsafe_decl_" + str(len(uh_decls)), once(pattern, uh)))
    unsafe_methods = (
        "bool UnsafeHeader::hasMachOBigEndianMagic() const",
        "Error UnsafeHeader::forEachLoadCommand(void (^callback)(const load_command* cmd, bool& stop)) const",
        "void UnsafeHeader::forEachLoadCommandSafe(void (^callback)(const load_command* cmd, bool& stop)) const",
        "uint64_t UnsafeHeader::preferredLoadAddress() const",
        "int64_t UnsafeHeader::getSlide() const",
        "std::string_view UnsafeHeader::name16(const char name[16])",
        "void UnsafeHeader::forEachSegment(void (^callback)(const SegmentInfo& infos, bool& stop)) const",
        "void UnsafeHeader::forEachSection(void (^callback)(const SectionInfo&, bool& stop)) const",
    )
    prefix += uh[:uh.index("*/") + 2] + "\nnamespace mach_o {\nstruct VIS_HIDDEN UnsafeHeader {\n"
    prefix += "\n".join(uh_decls) + "\n};\n"
    prefix += "\n".join(select(name, function(uc, name)) for name in unsafe_methods) + "\n}\n"
    prefix += "namespace dyld { class ThreadLocalVariables; }\n"
    # _tlv_get_addr has the special compiler calling convention and is only
    # called by compiler-emitted descriptor calls, never ordinary C code.
    prefix += 'extern "C" void* _tlv_get_addr(void*);\n'
    marker = "namespace dyld {\nusing mach_o::UnsafeHeader;"
    if text.count(marker) != 1:
        raise ValueError("unexpected destructor extraction namespace")
    text = text.replace(marker, prefix + "\n" + marker, 1)
    tls_methods = (
        "void ThreadLocalVariables::findInitialContent(const UnsafeHeader* hdr, std::span<const uint8_t>& initialContent, bool& allZeroFill)",
        "Error ThreadLocalVariables::forEachThunkSpan(const UnsafeHeader* hdr, Error (^visit)(std::span<Thunk>))",
        "Error ThreadLocalVariables::initializeThunksFromDisk(const UnsafeHeader* hdr)",
        "void* ThreadLocalVariables::instantiateVariable(const Thunk& thunk)",
    )
    text += "\nnamespace dyld {\nusing mach_o::Error;\n"
    text += "\n\n".join(select(name, function(implementation, name)) for name in tls_methods) + "\n}\n"
    text += '\nextern "C" void _tlv_bootstrap_error();\n'
    text += select("bootstrap_error", function(files["libdyldGlue.cpp"], "VIS_HIDDEN void _tlv_bootstrap_error()"))
    text += "\n" + registration
    return text, {**pieces, **selected}, {
        **{"libdyld/" + name: value for name, value in files.items()},
        "mach_o/UnsafeHeader.h": uh, "mach_o/UnsafeHeader.cpp": uc,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "report", "source-output", "assembly-output", "probe-output", "module-output", "zero-module-output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Libraries/25.5.0/dyld"
    own = root / "userland/native/tls"
    generated, pieces, originals = generate(source, (own / "registration.cpp").read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-tls-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("TLS and SDK architectures differ")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        xnu = root / "src/Kernel/xnu/12377.121.6"
        (work / "System").symlink_to(xnu / "osfmk", target_is_directory=True)
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-DBUILDING_LIBDYLD=1", "-DTARGET_OS_EXCLAVEKIT=0",
                 "-isystem", resource + "/include", "-isystem", sdk / "usr/include",
                 "-isystem", sdk / "usr/local/include", "-I", source / "common", "-I", source / "mach_o"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        cpp = ["-std=c++20", "-fno-exceptions", "-fno-rtti", "-fblocks",
               "-isystem", "/usr/include/c++/v1"]
        path = work / "tls.cpp"
        path.write_text(generated)
        original_assembly = (source / "libdyld/threadLocalHelpers.s").read_text()
        boundary = "\n#if !TARGET_OS_EXCLAVEKIT\n     // dyld_stub_binder"
        if original_assembly.count(boundary) != 1:
            raise ValueError("upstream TLS assembly boundary changed")
        # Preserve complete TLS assembly; unrelated legacy binder and lazy
        # image-loading entry points require different loader functionality.
        assembly = original_assembly[:original_assembly.index(boundary)]
        assembly_path = work / "threadLocalHelpers.s"
        assembly_path.write_text(assembly)
        objects = []
        for name, input_path, options in (
            ("tls", path, cpp),
            ("Error", source / "mach_o/Error.cpp", cpp),
            ("threadLocalHelpers", assembly_path,
             ["-x", "assembler-with-cpp", "-I", work, "-idirafter", xnu / "osfmk"]),
            ("test_tls", own / "test_tls.c", []),
            ("test_module", own / "test_module.c", []),
            ("test_zero_module", own / "test_zero_module.c", []),
        ):
            output = work / (name + ".o")
            run([flags[0], *options, *flags[1:], "-c", input_path, "-o", output])
            magic, cpu, _, kind = struct.unpack_from("<4I", output.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native TLS object: " + name)
            objects.append(output)
        output = work / "libtls.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", output, *objects[:3]])
        public, private, undefined = symbols(output)
        if {"__tlv_atexit", "__tlv_exit", "__tlv_bootstrap", "_native_tlv_init", "_native_tls_init"} - public:
            raise ValueError("missing TLS entry points")
        for probe in objects[3:]:
            headers = run(["llvm-objdump-20", "--macho", "--section-headers", probe])
            if "__thread_vars" not in headers or "__thread_bss" not in headers:
                raise ValueError("compiler did not emit native TLS templates/descriptors")
            if ("__thread_data" in headers) != (probe != objects[5]):
                raise ValueError("unexpected initialized versus all-zero TLS template")
        report = {
            "architecture": args.arch,
            "provider": json.loads((source / "UPSTREAM.json").read_text()),
            "source_sha256": {**{name: sha(value.encode()) for name, value in originals.items()},
                **{name: sha((source / name).read_bytes()) for name in (
                    "common/Defines.h", "common/va_list_wrap.h", "mach_o/Error.h",
                    "mach_o/Error.cpp", "mach_o/Misc.h", "libdyld/threadLocalHelpers.s")}},
            "integration_sha256": {name: sha((own / name).read_bytes()) for name in
                                   ("registration.cpp", "test_tls.c", "test_module.c", "test_zero_module.c")},
            "extracted_piece_sha256": {name: sha(value.encode()) for name, value in pieces.items()},
            "generated_source_sha256": sha(generated.encode()),
            "selected_assembly_sha256": sha(assembly.encode()),
            "archive_sha256": sha(output.read_bytes()),
            "object_count": 3, "exports": sorted(public), "private_symbols": sorted(private),
            "remaining_external_references": sorted(undefined - public - private),
            "cxx_headers": {"path": "/usr/include/c++/v1", "provider": "host LLVM libc++ headers targeting Darwin; no host runtime linked",
                            "config_sha256": sha(Path("/usr/include/c++/v1/__config").read_bytes())},
            "scope": "Apple dyld1378 disk-image TLS setup, instantiation, architecture assembly, and existing destructors; loader bulk callbacks register images before constructors.",
            "limitations": ["64-bit non-shared-cache images only; native target has no dyld shared cache.",
                            "Compiler TLS images remain loaded, matching Apple MachOAnalyzer::neverUnload.",
                            "Build verifies Mach-O objects and TLS sections; runtime isolation is checked separately in the guest."],
        }
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        args.source_output.write_text(generated)
        args.assembly_output.write_text(assembly)
        objects[3].replace(args.probe_output)
        objects[4].replace(args.module_output)
        objects[5].replace(args.zero_module_output)
        output.replace(args.output)
        print(f"Native compiler TLS {args.arch}: Apple setup, allocation and assembly compiled")


if __name__ == "__main__":
    main()
