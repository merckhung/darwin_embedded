#!/usr/bin/env python3
"""Compile original LLVM C++/ABI/unwind implementations with native Darwin SDKs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
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
from native_components import run

VERSION = "20.1.8"
def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()
def definitions(file):
    return {line.split()[-1] for line in run(["llvm-nm-20", "--defined-only", "-j", file]).splitlines() if line and not line.endswith(":")}
def symbols(file):
    public, imported, weak = set(), set(), set()
    for line in run(["llvm-nm-20", "-m", file]).splitlines():
        if "external " not in line or "non-external " in line:
            continue
        # LLVM annotates C++ weak definitions with 'automatically hidden'.
        # Those words describe the symbol; they are not its Mach-O name.
        match = re.search(r"(?:^|\s)external\s+(?:(?:\([^)]*\)|\[[^]]*\]|automatically hidden)\s+)*(\S+)", line)
        if not match:
            raise ValueError("unrecognized C++ Mach-O symbol: " + line)
        if "(undefined)" in line:
            (weak if "weak external" in line else imported).add(match[1])
        elif "private external" not in line:
            public.add(match[1])
    return public, imported, weak
def inventory(file, variable):
    # Read upstream inventories as data; Bazel executes every compilation.
    match = re.search(r"set\(" + variable + r"\s+(.*?)\n\s*\)", file.read_text(), re.S)
    if not match:
        raise ValueError("missing original C++ source inventory: " + variable)
    text = re.sub(r"#[^\n]*", "", match[1]); return text.split()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "imports", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    sources = {name: root / "src/Applications/cxx" / name / VERSION for name in ("libcxx", "libcxxabi", "libunwind")}
    shared = root / "src/Applications/cxx/llvm-libc-shared" / VERSION
    for source in [*sources.values(), shared]:
        manifest = json.loads((source / "UPSTREAM.json").read_text())
        for name, expected in manifest["upstream_files_sha256"].items():
            if digest(source / name) != expected:
                raise ValueError("modified original C++ source: " + name)
    with tempfile.TemporaryDirectory(prefix=".native-cxx-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk, install = work / "sdk", work / "install"
        os.environ["TMPDIR"] = str(work); os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("C++ SDK architecture mismatch")
        include = install / "usr/include/c++/v1"; shutil.copytree(sources["libcxx"] / "include", include)
        shutil.copyfile(sources["libcxx"] / "vendor/llvm/default_assertion_handler.in", include / "__assertion_handler")
        site = {"_LIBCPP_ABI_VERSION": "1", "_LIBCPP_ABI_NAMESPACE": "__1", "_LIBCPP_ABI_FORCE_ITANIUM": 1,
                "_LIBCPP_ABI_FORCE_MICROSOFT": 0, "_LIBCPP_HAS_THREADS": 1, "_LIBCPP_HAS_MONOTONIC_CLOCK": 1,
                "_LIBCPP_HAS_TERMINAL": 1, "_LIBCPP_HAS_MUSL_LIBC": 0, "_LIBCPP_HAS_THREAD_API_PTHREAD": 1,
                "_LIBCPP_HAS_THREAD_API_EXTERNAL": 0, "_LIBCPP_HAS_THREAD_API_WIN32": 0,
                "_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS": 0, "_LIBCPP_HAS_FILESYSTEM": 1,
                "_LIBCPP_HAS_RANDOM_DEVICE": 1, "_LIBCPP_HAS_LOCALIZATION": 1, "_LIBCPP_HAS_UNICODE": 1,
                "_LIBCPP_HAS_WIDE_CHARACTERS": 1, "_LIBCPP_HAS_TIME_ZONE_DATABASE": 0,
                "_LIBCPP_INSTRUMENTED_WITH_ASAN": 0, "_LIBCPP_HARDENING_MODE_DEFAULT": "_LIBCPP_HARDENING_MODE_FAST",
                "_LIBCPP_PSTL_BACKEND_SERIAL": 1}
        template = (sources["libcxx"] / "include/__config_site.in").read_text()
        def configure(match):
            boolean, key, tail = match.groups()
            if boolean:
                return "#define " + key + " " + str(site.get(key, 0))
            return "#define " + key + " " + str(site[key]) if key in site else "/* #undef " + key + " */"
        template = re.sub(r"^#cmakedefine(01)?\s+(\w+)([^\n]*)", configure, template, flags=re.M)
        template = template.replace("@_LIBCPP_ABI_DEFINES@", "").replace("@_LIBCPP_EXTRA_SITE_DEFINES@", "")
        if re.search(r"@\w+@|#cmakedefine", template):
            raise ValueError("unexpanded original C++ site template")
        (include / "__config_site").write_text(template)
        shutil.copyfile(sources["libcxxabi"] / "include/cxxabi.h", include / "cxxabi.h")
        for file in (sources["libunwind"] / "include").iterdir():
            if file.is_file():
                shutil.copyfile(file, include / file.name)
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = [f"--target={args.arch}-apple-macos26.5", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                 "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1", "-D_FORTIFY_SOURCE=0", "-Wno-availability",
                 "-isystem", include, "-isystem", resource + "/include", "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        cxx = sources["libcxx"]; abi = sources["libcxxabi"]; unwind = sources["libunwind"]
        original_copy = cxx / "src/filesystem/operations.cpp"
        text = original_copy.read_text()
        before = "#elif defined(__APPLE__) || __has_include(<copyfile.h>)"
        if text.count(before) != 1:
            raise ValueError("original filesystem copy backend selection changed")
        # The embedded SDK has no copyfile API. Select the original real
        # stream-copy implementation, which performs native POSIX file I/O.
        compiled_copy = work / "operations.cpp"; compiled_copy.write_text(text.replace(before, "#elif __has_include(<copyfile.h>)"))
        choices = {}
        choices["libunwind"] = [unwind / "src" / file for variable in ("LIBUNWIND_CXX_SOURCES", "LIBUNWIND_C_SOURCES", "LIBUNWIND_ASM_SOURCES") for file in inventory(unwind / "src/CMakeLists.txt", variable)]
        choices["libc++abi"] = [abi / "src" / file for file in inventory(abi / "src/CMakeLists.txt", "LIBCXXABI_SOURCES")]
        choices["libc++abi"] += [abi / "src" / file for file in ("stdlib_new_delete.cpp", "cxa_exception.cpp", "cxa_personality.cpp")]
        files = [file for file in inventory(cxx / "src/CMakeLists.txt", "LIBCXX_SOURCES") if file.endswith(".cpp")]
        recipe = (cxx / "src/CMakeLists.txt").read_text()
        for feature in ("THREADS", "RANDOM_DEVICE", "LOCALIZATION", "FILESYSTEM"):
            match = re.search(r"if \(LIBCXX_ENABLE_" + feature + r"\)\s*list\(APPEND LIBCXX_SOURCES\s+(.*?)\)", recipe, re.S)
            if not match:
                raise ValueError("missing original C++ feature inventory: " + feature)
            files += [file for file in re.sub(r"#[^\n]*", "", match[1]).split() if file.endswith(".cpp")]
        # libc++abi supplies new/delete; use the original compiler-rt int128
        # implementation only when not supplied by the native compiler runtime.
        files += ["filesystem/int128_builtins.cpp"]
        choices["libc++"] = [cxx / "src" / file for file in files]
        libraries = {}
        for name, files in choices.items():
            generated = work / name; generated.mkdir()
            local = ["-I", cxx / "src", "-I", cxx / "src/filesystem", "-I", abi / "src", "-I", unwind / "src", "-I", unwind / "include", "-I", abi / "include", "-I", shared,
                     "-DLIBC_NAMESPACE=__llvm_libc"]
            if name == "libunwind":
                local += ["-D_LIBUNWIND_IS_NATIVE_ONLY", "-D_LIBUNWIND_BUILDING_LIBRARY", "-fno-exceptions", "-fno-rtti", "-funwind-tables"]
            elif name == "libc++abi":
                local += ["-D_LIBCXXABI_BUILDING_LIBRARY", "-D_LIBCPP_BUILDING_LIBRARY"]
            else:
                local += ["-D_LIBCPP_BUILDING_LIBRARY", "-DLIBCXX_BUILDING_LIBCXXABI", "-D_LIBCPP_USING_ARC4_RANDOM"]
            jobs = [(file, generated / (f"{index:02d}-" + file.stem + ".o")) for index, file in enumerate(files)]
            def compile_one(job):
                file, obj = job; compiler = "clang++-20" if file.suffix == ".cpp" else "clang-20"
                language = ["-std=gnu++23", "-nostdinc++"] if compiler == "clang++-20" else ["-std=gnu11", "-fexceptions"] if file.suffix == ".c" else []
                try:
                    run([compiler, *flags, *language, *local, "-c", compiled_copy if file==original_copy else file, "-o", obj])
                except RuntimeError as error:
                    return {"error": str(error)}
                if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("foreign C++ runtime object")
                return {"source": str(file.relative_to(root)), "source_sha256": digest(file), "member": obj.name, "object_sha256": digest(obj),
                        "compiled_sha256": digest(compiled_copy if file==original_copy else file),
                        "adaptation": "Select original native stream-copy backend when copyfile.h is unavailable" if file==original_copy else None}
            with ThreadPoolExecutor(max_workers=8) as pool:
                records = list(pool.map(compile_one, jobs))
            errors = [record["error"] for record in records if "error" in record]
            if errors:
                raise ValueError("native C++ compilation failed:\n" + "\n".join(errors))
            archive = install / "usr/lib" / (name + ".a"); archive.parent.mkdir(parents=True, exist_ok=True)
            run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
            public, imported, weak = symbols(archive)
            libraries[name] = {"sources": records, "exports": sorted(public), "definitions": sorted(definitions(archive)), "imports": sorted(imported - definitions(archive)), "weak_imports": sorted(weak), "archive_sha256": digest(archive)}
        defined = set().union(*(set(record["exports"]) for record in libraries.values())); imported = set().union(*(set(record["imports"]) for record in libraries.values()))
        args.imports.write_text("\n".join(sorted(imported - defined - {"___dso_handle"})) + "\n")
        for name, source in sources.items():
            license = install / "usr/share/licenses" / name; license.mkdir(parents=True, exist_ok=True); shutil.copyfile(source / "LICENSE.TXT", license / "LICENSE.TXT")
        with tarfile.open(args.output, "w") as archive:
            for file in sorted(install.rglob("*")):
                if file.is_file():
                    info = archive.gettarinfo(str(file), str(file.relative_to(install))); info.uid=info.gid=info.mtime=0; info.uname=info.gname="root"
                    with file.open("rb") as stream:
                        archive.addfile(info, stream)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": libraries, "site_configuration": site,
            "bundle_sha256": digest(args.output), "runtime_checked": False, "scope": "Original native LLVM C++/ABI/unwinder; Bazel compilation and native linkage are separate validation stages."}, indent=2) + "\n")
        print(f"Native C++ {args.arch}: {sum(len(record['sources']) for record in libraries.values())} original Mach-O objects")

if __name__ == "__main__":
    main()
