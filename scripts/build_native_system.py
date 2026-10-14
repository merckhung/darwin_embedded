#!/usr/bin/env python3
"""Package real native Darwin components behind an automatically initialized LibSystem."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import tarfile
import tempfile

from native_components import build_helpers, run, symbols
from sign_diagnostic_macho import sign


def canonicalize_reexports(path, expected):
    """Repair LLVM 20's duplicate load/reexport commands before signing.

    llvmorg-20.1.8/lld/MachO/Writer.cpp emits LC_LOAD_DYLIB followed by
    LC_REEXPORT_DYLIB for each reexport but allocates only one bind ordinal.
    Apple's MachOFile::forEachDependentDylib counts both commands. Keep one
    genuine LC_REEXPORT_DYLIB per dependency, preserving ordinals, versions,
    file offsets, sections, and all other commands. No guest loader exception.
    """
    data = bytearray(path.read_bytes())
    count, size = struct.unpack_from("<II", data, 16)
    commands, offset = [], 32
    for _ in range(count):
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or offset + length > 32 + size:
            raise RuntimeError("malformed linker output load command")
        commands.append(bytearray(data[offset:offset + length]))
        offset += length
    if offset != 32 + size:
        raise RuntimeError("malformed linker output command size")
    fixed, dependencies, index = [], [], 0
    while index < len(commands):
        item = commands[index]
        command = struct.unpack_from("<I", item)[0]
        if command == 0xc:  # LC_LOAD_DYLIB
            name_offset = struct.unpack_from("<I", item, 8)[0]
            name = bytes(item[name_offset:]).split(b"\0", 1)[0].decode()
            if index + 1 == len(commands):
                raise RuntimeError("missing LLVM reexport partner")
            partner = commands[index + 1]
            partner_command, _, partner_name_offset = struct.unpack_from("<III", partner)
            partner_name = bytes(partner[partner_name_offset:]).split(b"\0", 1)[0].decode()
            if partner_command != 0x8000001f or partner_name != name:
                raise RuntimeError("unexpected LLVM reexport layout")
            struct.pack_into("<I", item, 0, 0x8000001f)
            dependencies.append(name)
            index += 1
        elif command in (0x80000018, 0x8000001f, 0x80000023):
            raise RuntimeError("unexpected unpaired facade dependency")
        fixed.append(item)
        index += 1
    if dependencies != expected:
        raise RuntimeError("facade reexport order does not match bind ordinals")
    encoded = b"".join(fixed)
    struct.pack_into("<II", data, 16, len(fixed), len(encoded))
    data[32:32 + size] = encoded + bytes(size - len(encoded))
    path.write_bytes(data)


def verify_import_ordinals(path, components):
    """Check each two-level import against its actual numbered dependency."""
    data = path.read_bytes()
    count = struct.unpack_from("<I", data, 16)[0]
    offset, dependencies, symtab = 32, [], None
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, offset)
        if command in (0xc, 0x80000018, 0x8000001f, 0x80000023):
            name = offset + struct.unpack_from("<I", data, offset + 8)[0]
            dependencies.append(data[name:data.index(0, name)].decode())
        elif command == 2:
            symtab = struct.unpack_from("<4I", data, offset + 8)
        offset += size
    if not symtab:
        raise RuntimeError("native component has no auditable symbol table")
    table, count, strings, string_size = symtab
    for index in range(count):
        name_offset, kind, _, description, _ = struct.unpack_from("<IBBHQ", data, table + index * 16)
        if kind & 0xe or not kind & 1 or kind & 0xe0:
            continue
        ordinal = description >> 8
        if ordinal in (0, 0xfe, 0xff):
            continue
        name_at = strings + name_offset
        name = data[name_at:data.index(0, name_at, strings + string_size)].decode()
        if ordinal > len(dependencies) or name not in components.get(dependencies[ordinal - 1], set()):
            raise RuntimeError("native two-level import does not resolve through its actual ordinal: " + name)


def verify_image(path, arch, required_command=None):
    data = path.read_bytes()
    magic, cpu, _, kind, count, size = struct.unpack_from("<6I", data)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if arch == "arm64" else 0x1000007, 6):
        raise RuntimeError("foreign or non-dylib component: " + str(path))
    offset, commands, initializers = 32, [], 0
    for _ in range(count):
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or offset + length > 32 + size:
            raise RuntimeError("malformed native load command")
        commands.append(command)
        if command == 0x19:
            sections = struct.unpack_from("<I", data, offset + 64)[0]
            for index in range(sections):
                section = offset + 72 + index * 80
                if section + 80 > offset + length:
                    raise RuntimeError("malformed native section table")
                flags = struct.unpack_from("<I", data, section + 64)[0]
                if flags & 0xff == 9:  # S_MOD_INIT_FUNC_POINTERS
                    initializers += struct.unpack_from("<Q", data, section + 40)[0] // 8
        offset += length
    if offset != 32 + size or (required_command is not None and required_command not in commands):
        raise RuntimeError("native dylib command audit failed: " + str(path))
    if required_command is not None and initializers != 1:
        raise RuntimeError("LibSystem must carry its one real automatic initializer")


def append_component_archives(archives, specifications, arch):
    """Append genuine native object archives inside existing components only."""
    records = []
    expected_cpu = {"arm64": 0x100000c, "x86_64": 0x1000007}[arch]
    for item in specifications:
        name, separator, path = item.partition("=")
        if not separator or name not in archives:
            raise ValueError("unknown built-in native component: " + item)
        archive = Path(path).resolve()
        data = archive.read_bytes()
        if data[:8] != b"!<arch>\n":
            raise ValueError("component input is not a real archive: " + item)
        offset, members = 8, 0
        while offset < len(data):
            header = data[offset:offset + 60]
            if len(header) != 60 or header[58:60] != b"`\n":
                raise ValueError("malformed native component archive")
            size = int(header[48:58].strip())
            member = data[offset + 60:offset + 60 + size]
            if len(member) != size:
                raise ValueError("truncated native component member")
            filename = header[:16].rstrip()
            if filename.startswith(b"#1/"):
                length = int(filename[3:])
                filename, member = member[:length].rstrip(b"\0"), member[length:]
            if filename not in (b"__.SYMDEF", b"__.SYMDEF SORTED"):
                if len(member) < 32:
                    raise ValueError("truncated native component object")
                magic, cpu, _, kind = struct.unpack_from("<4I", member)
                if (magic, kind) != (0xfeedfacf, 1):
                    raise ValueError("component member is not a native Mach-O object")
                if cpu != expected_cpu:
                    raise ValueError("component member has the wrong native CPU")
                members += 1
            offset += 60 + size + (size & 1)
        if offset != len(data) or not members:
            raise ValueError("empty or malformed native component archive")
        archives[name].append(archive)
        records.append({"component": name, "archive": archive.name, "objects": members,
                        "sha256": hashlib.sha256(data).hexdigest()})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "dyld", "libsyscall", "libmach",
                 "libplatform", "libpthread", "libpthread-dyld", "libc", "libmalloc",
                 "libmath", "libtlv", "librng", "exports", "output", "facade", "report"):
        parser.add_argument("--" + name, type=Path, required=name in ("root", "sdk", "mach-headers", "output", "report"))
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--compiler-tls", action="store_true")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--helpers", type=Path)
    parser.add_argument("--library", action="append", default=[], help="additional real component NAME=ARCHIVE")
    parser.add_argument("--component-library", action="append", default=[],
                        help="append real archive inside built-in component NAME=ARCHIVE")
    args = parser.parse_args()
    root = args.root.resolve()
    source = root / "userland/native/system"
    requested = set()
    for line in ([] if args.compile_only else args.exports.read_text().splitlines()):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if not re.fullmatch(r"[_A-Za-z][_A-Za-z0-9.$]*", line):
            raise ValueError("invalid requested Mach-O symbol: " + line)
        requested.add(line)
    if not requested and not args.compile_only:
        raise ValueError("native LibSystem requires an explicit nonempty export request")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-system-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        for path in (args.sdk, args.mach_headers):
            with tarfile.open(path) as archive:
                archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("LibSystem SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        libc_source = root / "src/Libraries/25.5.0/Libc"
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-ffreestanding",
                 "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-O2", "-Werror",
                 "-Wno-availability", "-D_FORTIFY_SOURCE=0", "-DPRIVATE=1",
                 "-isystem", str(sdk / "usr/include"), "-isystem", str(sdk / "usr/local/include"),
                 "-isystem", resource + "/include", "-isystem", str(root / "tools/cctools/include"),
                 "-I", str(root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers"),
                 "-I", str(libc_source / "darwin"), "-I", str(work)]
        if args.arch == "arm64":
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"]
        if args.compiler_tls:
            flags += ["-DNATIVE_COMPILER_TLS=1"]
        program_vars = re.search(r"struct ProgramVars\s*\{.*?\};",
                                 (libc_source / "sys/crt_externs.c").read_text(), re.DOTALL)
        if not program_vars:
            raise RuntimeError("published Libc ProgramVars declaration missing")
        (work / "native_programvars.h").write_text(program_vars.group() + "\n")
        if args.helpers:
            helper_tree = work / "helpers"
            with tarfile.open(args.helpers) as archive:
                archive.extractall(helper_tree, filter="data")
            manifest = json.loads((helper_tree / "helpers.json").read_text())
            if manifest["architecture"] != args.arch or manifest["compiler_tls"] != args.compiler_tls:
                raise ValueError("native helper architecture/TLS configuration mismatch")
            objects = {name: helper_tree / path for name, path in manifest["startup_objects"].items()}
            helpers = {name: [helper_tree / path for path in paths]
                       for name, paths in manifest["component_objects"].items()}
            provenance = manifest["helper_sources"]
        else:
            objects = {}
            for name in ("bootstrap.c", "initialize.c"):
                objects[name] = work / (name + ".o")
                run([*flags, "-c", source / name, "-o", objects[name]])
            helpers, provenance = build_helpers(root, work, sdk, flags, args.arch, full_runtime=True)
        if args.compile_only:
            manifest = {
                "architecture": args.arch, "compiler_tls": args.compiler_tls,
                "startup_sources": {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
                                    for name in objects},
                "program_vars_declaration_source": "src/Libraries/25.5.0/Libc/sys/crt_externs.c",
                "program_vars_declaration_sha256": hashlib.sha256(program_vars.group().encode()).hexdigest(),
                "startup_objects": {name: path.name for name, path in objects.items()},
                "component_objects": {name: [path.name for path in paths] for name, paths in helpers.items()},
                "helper_sources": provenance,
            }
            manifest_path = work / "helpers.json"
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            with tarfile.open(args.output, "w") as archive:
                for path in [manifest_path, *objects.values(), *(path for paths in helpers.values() for path in paths)]:
                    info = archive.gettarinfo(str(path), arcname=path.name)
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = "root"
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)
            shutil.copyfile(manifest_path, args.report)
            print(f"native system helpers {args.arch}: actual initialization and component sources compiled")
            return
        archives = {
            "kernel": [args.libmach.resolve(), args.libsyscall.resolve()],
            "platform": [args.libplatform.resolve()],
            "pthread": [args.libpthread.resolve()], "libc": [args.libc.resolve()],
            "malloc": [args.libmalloc.resolve()], "math": [args.libmath.resolve()],
            "blocks": [], "crypto": [], "tlv": [args.libtlv.resolve()],
            "rng": [args.librng.resolve()],
            "bootstrap": [args.libpthread_dyld.resolve(), args.libsyscall.resolve()],
        }
        component_archive_inputs = append_component_archives(archives, args.component_library, args.arch)
        additional_components = []
        for item in args.library:
            name, separator, path = item.partition("=")
            if not separator or not re.fullmatch(r"[a-z][a-z0-9_]*", name) or name in archives:
                raise ValueError("invalid or duplicate real component: " + item)
            archives[name] = [Path(path).resolve()]
            additional_components.append(name)
        extra = {name: list(helpers.get(name, [])) for name in archives}
        extra["bootstrap"] = [objects["bootstrap.c"]]
        selected = {name: set() for name in archives}
        selected["bootstrap"] = {"_native_system_bootstrap"}
        providers = {}
        # Actual private libkernel and dyld-pthread forwarders remain private.
        for name in ["platform", "pthread", "malloc", "libc", "math", "kernel", "blocks", "crypto", "tlv", "rng", *additional_components]:
            for archive in [*extra[name], *archives[name]]:
                for symbol in symbols(archive)[0]:
                    providers.setdefault(symbol, name)
        # These are actual upstream aliases to the identical memory/string
        # entry points, not replacement implementations or ABI translations.
        alias_source = libc_source / "xcodescripts/alias.list"
        aliases = {}
        for line in alias_source.read_text().splitlines():
            original, alias = line.split()
            if alias not in providers and original in providers:
                providers[alias] = providers[original]
                aliases[alias] = original
        loader_exports = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        # Application imports may request real dyld APIs as well as LibSystem.
        # Such APIs stay loader-owned and are recorded separately in the report.
        # dyld discovers these genuine providers by name at run time. Static
        # imports alone cannot keep dlclose finalization or thread-local
        # dlerror state visible through the component export lists.
        loader_runtime_apis = {"___cxa_finalize_ranges", "_pthread_key_create",
                               "_pthread_getspecific", "_pthread_setspecific", "__tlv_bootstrap"}
        wanted = requested | symbols(objects["initialize.c"])[1] | loader_runtime_apis
        missing=wanted-loader_exports-{"_native_system_bootstrap"}-providers.keys()
        if missing:
            raise RuntimeError("requested native APIs need genuine implementations: " + ", ".join(sorted(missing)))
        for symbol in wanted - loader_exports - {"_native_system_bootstrap"}:
            if symbol not in providers:
                raise RuntimeError("requested native API needs a genuine implementation: " + symbol)
            selected[providers[symbol]].add(symbol)
        bundle = work / "bundle"
        component_dir = bundle / "usr/lib/system"
        component_dir.mkdir(parents=True)
        libraries = {name: component_dir / ("libsystem_native_" + name + ".dylib") for name in archives}
        link = ["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains",
                "-dead_strip", "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign"]
        facade = bundle / "usr/lib/libSystem.B.dylib"
        for iteration in range(32):
            for name, output in libraries.items():
                exports = work / (name + ".exports")
                exports.write_text("\n".join(sorted(selected[name])) + "\n")
                roots = [option for symbol in sorted(selected[name]) for option in ("-u", symbol)]
                alias_options = [option for alias, original in aliases.items()
                                 if alias in selected[name] for option in ("-alias", original, alias, "-u", original)]
                run([*link, "-dylib", "-undefined", "dynamic_lookup", "-install_name",
                     "/usr/lib/system/" + output.name, "-exported_symbols_list", exports, *roots,
                     *alias_options, "-o", output, *extra[name], *archives[name]])
            reexports = [option for path in libraries.values() for option in ("-reexport_library", path)]
            run([*link, "-dylib", "-undefined", "dynamic_lookup", "-install_name", "/usr/lib/libSystem.B.dylib",
                 "-current_version", "1356.0.0", "-compatibility_version", "1.0.0",
                 "-o", facade, objects["initialize.c"], *reexports])
            canonicalize_reexports(facade, ["/usr/lib/system/" + path.name for path in libraries.values()])
            imports = {name: symbols(path)[1] for name, path in libraries.items()}
            imports["LibSystem"] = symbols(facade)[1]
            actual_exports = set().union(*(symbols(path)[0] for path in libraries.values()))
            available = actual_exports | loader_exports | symbols(facade)[0]
            missing = (set().union(*imports.values()) | requested) - available
            if not missing:
                break
            additions = missing & providers.keys()
            if any(symbol not in selected[providers[symbol]] for symbol in additions):
                for symbol in additions:
                    selected[providers[symbol]].add(symbol)
                continue
            raise RuntimeError("native LibSystem needs genuine implementations: " + ", ".join(sorted(missing)))
        else:
            raise RuntimeError("native LibSystem closure did not converge")
        records = {}
        component_exports = {"/usr/lib/system/" + path.name: symbols(path)[0]
                             for path in libraries.values()}
        for name, path in {**libraries, "LibSystem": facade}.items():
            verify_import_ordinals(path, component_exports)
            sign(path)
            verify_image(path, args.arch, 0x8000001f if name == "LibSystem" else None)
            public, strong, weak = symbols(path)
            records[name] = {"install_path": "/" + str(path.relative_to(bundle)),
                             "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                             "exports": sorted(public), "imports": sorted(strong), "weak_imports": sorted(weak)}
        report = {
            "architecture": args.arch, "runtime_checked": False,
            "requested_exports": sorted(requested), "requested_loader_exports": sorted(requested & loader_exports),
            "loader_runtime_exports": sorted(loader_runtime_apis),
            "helper_sources": provenance, "libraries": records,
            "component_archive_inputs": component_archive_inputs,
            "helper_configuration": {
                "bit_search": "actual generic fls/ffs C bodies emitted for native CPUs without private optimized providers",
                "stdio_extension": "actual BUILDING_VARIANT/VARIANT_DARWINEXTSN fopen source",
                "platform_initializer": "actual dynamic initializer; unused private AppleFeatures include removed",
                "sha256": "actual portable XNU corecrypto source; unsupported DIT and assembly disabled",
                "mach_errors": "actual error tables and exact published USB/FireWire numeric header declarations",
            },
            "startup_sources": {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
                                for name in objects},
            "program_vars_declaration_source": "src/Libraries/25.5.0/Libc/sys/crt_externs.c",
            "program_vars_declaration_sha256": hashlib.sha256(program_vars.group().encode()).hexdigest(),
            "strong_import_closure": "complete against packaged components and the exact source-built dyld",
            "reexports": "canonical single LC_REEXPORT_DYLIB per ordinal; LLVM20 duplicate-command output repaired before signing",
            "two_level_import_ordinals": "verified against actual numbered dependency exports",
            "upstream_aliases": {alias: original for alias, original in aliases.items() if alias in actual_exports},
            "upstream_alias_source_sha256": hashlib.sha256(alias_source.read_bytes()).hexdigest(),
            "dyld_sha256": hashlib.sha256(args.dyld.read_bytes()).hexdigest(),
            "initializer": "automatic S_MOD_INIT_FUNC_POINTERS using actual ProgramVars and source-built callbacks",
            "initializer_order": ["dyld-pthread bootstrap", "kernel", "platform", "pthread", "libc", "malloc", "tlv"] +
                                 (["compiler TLS"] if args.compiler_tls else []),
            "atfork": "real loader, pthread application/component, malloc, libc, and Mach handlers",
            "cancellation": "actual Apple CANCELABLE variants and native cancellable BSD entry points",
            "timezone": "actual NOTIFY_TZ-disabled implementation; asynchronous daemon updates absent",
            "absent_services": ["dispatch runtime", "XPC", "notify daemon", "CoreServices", "libtrace", "framework initializers"],
        }
        args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n")
        shutil.copyfile(facade, args.facade)
        with tarfile.open(args.output, "w") as archive:
            for path in sorted(bundle.rglob("*")):
                info = archive.gettarinfo(str(path), arcname=str(path.relative_to(bundle)))
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                info.mtime = 0
                info.mode = 0o755
                if path.is_file():
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)
                else:
                    archive.addfile(info)
        print(f"native LibSystem {args.arch}: {len(requested)} requested APIs; complete strong closure; automatic initializer")


if __name__ == "__main__":
    main()
