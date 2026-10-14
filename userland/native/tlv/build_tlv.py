#!/usr/bin/env python3
"""Build Apple's dyld TLV destructor subset without unrelated image-loader code.

This is a source extraction, not a replacement destructor implementation:
the class state, pthread helper inlines, and complete method/function bodies
come from the pinned dyld sources. The only added runtime function calls
ThreadLocalVariables::initialize() during native component startup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def once(pattern, text):
    matches = list(re.finditer(pattern, text, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise ValueError(f"expected one upstream declaration matching {pattern!r}, found {len(matches)}")
    return matches[0].group()


def function(text, declaration):
    """Select a complete pinned function, including all upstream conditionals."""
    match = once(r"^" + re.escape(declaration) + r"\n\{", text)
    start = text.index(match)
    depth = 0
    for position in range(start + len(match) - 1, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start:position + 1]
    raise ValueError("unterminated upstream function: " + declaration)


def extract(source):
    files = {
        name: (source / "libdyld" / name).read_text()
        for name in ("ThreadLocalVariables.h", "ThreadLocalVariables.cpp",
                     "LibSystemHelpers.h", "libdyldGlue.cpp")
    }
    header = files["ThreadLocalVariables.h"]
    implementation = files["ThreadLocalVariables.cpp"]
    helpers = files["LibSystemHelpers.h"]
    glue = files["libdyldGlue.cpp"]
    pieces = {}
    pieces["pthread_key_type"] = once(r"^  typedef pthread_key_t[^\n]*dyld_thread_key_t;", helpers)
    for name in ("key_create", "getspecific", "setspecific"):
        pieces["pthread_" + name] = once(
            r"^  inline[^\n]*dyld_thread_" + name + r"\([^\n]*::pthread_[^\n]*", helpers)
    pieces["TermFunc"] = once(r"^    typedef void[^\n]*\(\*TermFunc\)[^\n]*;", header)
    for name in ("initialize", "addTermFunc", "exit", "finalizeList"):
        pieces["declaration_" + name] = once(r"^    void[ \t]+" + name + r"\([^\n]*;", header)
    pieces["Terminator"] = once(r"^    struct Terminator\n    \{.*?^    \};", header)
    pieces["TerminatorList"] = once(r"^    struct TerminatorList \{.*?^    \};", header)
    pieces["terminator_key"] = once(r"^    dyld_thread_key_t[^\n]*_terminatorsKey[^\n]*;", header)
    declarations = (
        "static void finalizeListTLV(void* list)",
        "void ThreadLocalVariables::initialize()",
        "void ThreadLocalVariables::addTermFunc(TermFunc func, void* objAddr)",
        "void ThreadLocalVariables::exit()",
        "void ThreadLocalVariables::TerminatorList::reverseWalkChain(void (^visit)(TerminatorList*))",
        "void ThreadLocalVariables::finalizeList(void* l)",
    )
    for declaration in declarations:
        pieces[declaration] = function(implementation, declaration)
    pieces["global_state"] = once(r"^ThreadLocalVariables sThreadLocalVariables;", implementation)
    pieces["checkTPROState"] = function(glue, "static ALWAYS_INLINE void checkTPROState()")
    for declaration in ("void _tlv_atexit(void (*termFunc)(void* objAddr), void* objAddr)", "void _tlv_exit()"):
        pieces[declaration] = function(glue, declaration)
    license_headers = []
    for original in files.values():
        notice = original[:original.index("*/") + 2]
        if notice not in license_headers:
            license_headers.append(notice)
    text = "\n\n".join(license_headers) + "\n" + """
// Generated from dyld-1378 by userland/native/tlv/build_tlv.py.
// Complete upstream destructor bodies and class state; no TLV image setup.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "Defines.h"
#if !__has_feature(tls) || DYLD_FEATURE_USE_HW_TPRO
#error "Audit the native TLV target before changing TLS or TPRO support"
#endif
"""
    text += "\n".join(pieces["pthread_" + name] for name in
                      ("key_type", "key_create", "getspecific", "setspecific")) + "\n"
    text += "namespace dyld {\nclass VIS_HIDDEN ThreadLocalVariables {\npublic:\n"
    text += pieces["TermFunc"] + "\n"
    text += "\n".join(pieces["declaration_" + name] for name in
                      ("initialize", "addTermFunc", "exit", "finalizeList")) + "\nprivate:\n"
    text += "\n".join(pieces[name] for name in ("Terminator", "TerminatorList", "terminator_key"))
    text += "\n};\nextern ThreadLocalVariables sThreadLocalVariables;\n"
    text += "\n\n".join(pieces[name] for name in declarations) + "\n"
    text += pieces["global_state"] + "\n}\n"
    text += pieces["checkTPROState"] + '\nextern "C" {\n'
    text += pieces["void _tlv_atexit(void (*termFunc)(void* objAddr), void* objAddr)"] + "\n"
    text += pieces["void _tlv_exit()"] + "\n"
    text += "void native_tlv_init() { dyld::sThreadLocalVariables.initialize(); }\n}\n"
    return text, files, pieces


def symbols(path):
    public, private, undefined = set(), set(), set()
    for line in run(["llvm-nm-20", "-m", "-g", path]).splitlines():
        match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
        if match:
            destination = undefined if "(undefined)" in line else private if "private external" in line else public
            destination.add(match.group(1))
    return public, private, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "report", "source-output", "probe-output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "src/Libraries/25.5.0/dyld"
    extracted, files, pieces = extract(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-tlv-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("TLV and SDK architectures differ")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-DBUILDING_LIBDYLD=1",
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include",
                 "-isystem", resource + "/include", "-I", source / "common"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        generated = work / "tlv.cpp"
        generated.write_text(extracted)
        object_path = work / "tlv.o"
        run([*flags, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-fblocks",
             "-c", generated, "-o", object_path])
        probe = work / "test_tlv.o"
        run([*flags, "-c", root / "userland/native/tlv/test_tlv.c", "-o", probe])
        for path in (object_path, probe):
            magic, cpu, _, kind = struct.unpack_from("<4I", path.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native TLV object: " + str(path))
        output = work / "libtlv.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", output, object_path])
        public, private, undefined = symbols(output)
        required = {"__tlv_atexit", "__tlv_exit", "_native_tlv_init"}
        if required - public:
            raise ValueError("missing genuine TLV entry points")
        report = {
            "architecture": args.arch,
            "provider": json.loads((source / "UPSTREAM.json").read_text()),
            "source_sha256": {**{"libdyld/" + name: hashlib.sha256(text.encode()).hexdigest()
                                  for name, text in files.items()},
                              "common/Defines.h": hashlib.sha256((source / "common/Defines.h").read_bytes()).hexdigest()},
            "extracted_piece_sha256": {name: hashlib.sha256(text.encode()).hexdigest()
                                       for name, text in pieces.items()},
            "generated_source_sha256": hashlib.sha256(extracted.encode()).hexdigest(),
            "archive_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "object_count": 1, "exports": sorted(public), "private_symbols": sorted(private),
            "remaining_external_references": sorted(undefined - public - private),
            "scope": "Exact Apple TLV destructor registration and finalization subset; native_tlv_init calls the actual initializer once after pthread and malloc initialization.",
            "limitations": ["Does not set up Mach-O TLS thunks, instantiate thread-local variables, or implement dlopen.",
                            "Uses the real allocator, pthread keys, and BlocksRuntime classes from other native components.",
                            "Build-only verification; destructor behavior requires the separate guest probe."],
        }
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        args.source_output.write_text(extracted)
        probe.replace(args.probe_output)
        output.replace(args.output)
        print(f"Native dyld TLV {args.arch}: exact upstream destructor methods compiled")


if __name__ == "__main__":
    main()
