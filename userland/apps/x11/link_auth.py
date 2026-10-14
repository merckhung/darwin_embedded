#!/usr/bin/env python3
"""Link every genuine Xau/Xdmcp object and audit its native runtime providers."""
import argparse
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile

DYLIB_COMMANDS = (0xc, 0x80000018, 0x8000001f, 0x80000023)
REEXPORT_DYLIB = 0x8000001f


def load_commands(data):
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xfeedfacf:
        raise ValueError("expected native 64-bit Mach-O")
    count, size = struct.unpack_from("<2I", data, 16)
    if size > len(data) - 32:
        raise ValueError("truncated Mach-O load commands")
    offset = 32
    for _ in range(count):
        if offset + 8 > 32 + size:
            raise ValueError("truncated Mach-O load command")
        command, length = struct.unpack_from("<2I", data, offset)
        if length < 8 or length % 8 or length > 32 + size - offset:
            raise ValueError("invalid Mach-O load command")
        yield command, offset, length
        offset += length
    if offset != 32 + size:
        raise ValueError("inconsistent Mach-O load-command extent")


def image_metadata(data):
    """Read the actual dependency order and public nlist symbols of an image."""
    dependencies, exports, imports, symtab = [], set(), [], None
    for command, offset, length in load_commands(data):
        if command in DYLIB_COMMANDS:
            if length < 24:
                raise ValueError("truncated dylib dependency")
            name = struct.unpack_from("<I", data, offset + 8)[0]
            if name < 24 or name >= length:
                raise ValueError("invalid dylib dependency name")
            end = data.find(b"\0", offset + name, offset + length)
            if end < 0:
                raise ValueError("unterminated dylib dependency name")
            dependencies.append((command, data[offset + name:end].decode()))
        elif command == 2:
            if length != 24 or symtab is not None:
                raise ValueError("invalid native symbol table command")
            symtab = struct.unpack_from("<4I", data, offset + 8)
    if symtab is None:
        raise ValueError("missing native symbol table")
    table, count, strings, string_size = symtab
    if table + count * 16 > len(data) or strings + string_size > len(data):
        raise ValueError("native symbol table outside image")
    for index in range(count):
        position = table + index * 16
        name, kind, _, description, _ = struct.unpack_from("<IBBHQ", data, position)
        if kind & 0xe0 or not kind & 1:  # Debug or local symbol.
            continue
        if name >= string_size:
            raise ValueError("native symbol name outside string table")
        end = data.find(b"\0", strings + name, strings + string_size)
        if end < 0:
            raise ValueError("unterminated native symbol name")
        symbol = data[strings + name:end].decode()
        if kind & 0xe:
            if not kind & 0x10:  # Private externs are not cross-image APIs.
                if (kind & 0xe) not in (2, 0xe):
                    raise ValueError("unsupported public native symbol kind")
                exports.add(symbol)
        else:
            imports.append({"symbol": symbol, "ordinal": description >> 8,
                            "weak": bool(description & 0x40), "nlist_offset": position})
    return {"dependencies": dependencies, "exports": exports, "imports": imports}


def verify_closure(root, images, loader_exports):
    """Audit every eagerly bound dependency in this executable's real scope."""
    reachable = []
    def visit(path):
        if path in reachable:
            return
        if path not in images:
            raise ValueError("unpackaged native dependency: " + path)
        reachable.append(path)
        for command, dependency in images[path]["dependencies"]:
            if command == 0x80000018 and dependency not in images:
                continue
            visit(dependency)
    visit(root)
    return {path: verify_imports(path, images, loader_exports, flat_namespace_root=root) for path in reachable}

def verify_imports(image, images, loader_exports, *, flat_namespace_root=None):
    """Resolve each import through its numbered dependency and its reexports."""
    reachable = []

    def visit(path):
        if path in reachable:
            return
        if path not in images:
            raise ValueError("unpackaged native dependency: " + path)
        reachable.append(path)
        for command, dependency in images[path]["dependencies"]:
            if command == 0x80000018 and dependency not in images:
                continue
            visit(dependency)

    visit(image)
    # LibSystem component images intentionally use flat imports across the
    # facade's reexported components. Resolve them within the actual main
    # executable's loaded dependency graph when auditing that runtime context.
    # Merely installing an unrelated provider never puts it in this graph.
    if flat_namespace_root is not None:
        visit(flat_namespace_root)

    def exported_by(path, symbol, seen=()):
        if path not in images or path in seen:
            return None
        metadata = images[path]
        if symbol in metadata["exports"]:
            return path
        for command, dependency in metadata["dependencies"]:
            if command == REEXPORT_DYLIB:
                if provider := exported_by(dependency, symbol, (*seen, path)):
                    return provider
        return None

    results = []
    dependencies = images[image]["dependencies"]
    for entry in images[image]["imports"]:
        symbol, ordinal = entry["symbol"], entry["ordinal"]
        dependency, provider = None, None
        if ordinal == 0:
            dependency = image
            provider = image if symbol in images[image]["exports"] else None
        elif ordinal == 0xfe:
            # Flat lookup can use only this image's actual loaded closure and
            # the matching interpreter; merely packaging another dylib is not
            # sufficient to make its exports available.
            for path in reachable:
                if provider := exported_by(path, symbol):
                    break
            if provider is None and symbol in loader_exports:
                provider = "/usr/lib/dyld"
        elif 1 <= ordinal <= len(dependencies):
            dependency = dependencies[ordinal - 1][1]
            provider = exported_by(dependency, symbol)
        else:
            raise ValueError(f"invalid or unavailable dependency ordinal {ordinal}: {symbol}")
        if provider is None and not entry["weak"]:
            raise ValueError(f"native import has no provider through ordinal {ordinal}: {symbol}")
        results.append({"symbol": symbol, "ordinal": ordinal, "weak": entry["weak"],
                        "dependency": dependency, "provider": provider})
    return results


def main():
    from build_clients import digest, pack, run, symbols
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
    from sign_diagnostic_macho import sign

    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("auth", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xauth-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        auth, tree = work / "auth", work / "system"
        for archive_path, destination in ((args.auth, auth), (args.system, tree)):
            with tarfile.open(archive_path) as archive:
                archive.extractall(destination, filter="data")
        loader_exports = symbols(args.dyld)[0] | {"dyld_stub_binder"}
        images = {"/" + str(component.relative_to(tree)): image_metadata(component.read_bytes())
                  for component in (tree / "usr/lib").rglob("*.dylib")}
        results = {}
        for name, version in (("libXau", "6"), ("libXdmcp", "6")):
            archive = auth / "usr/lib" / (name + ".a")
            image = tree / "usr/lib" / (name + "." + version + ".dylib")
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-dylib", "-no_fixup_chains",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign",
                 "-undefined", "dynamic_lookup", "-install_name", "/usr/lib/" + image.name,
                 "-compatibility_version", "6.0.0", "-current_version", "6.0.0",
                 "-force_load", archive, "-L", tree / "usr/lib/system",
                 tree / "usr/lib/libSystem.B.dylib", "-o", image])
            sign(image)
            magic, cpu, _, kind = struct.unpack_from("<4I", image.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 6):
                raise ValueError("authentication image is not the target Mach-O dylib")
            exports, imports, weak = symbols(image)
            install_path = "/usr/lib/" + image.name
            images[install_path] = image_metadata(image.read_bytes())
            resolutions = verify_imports(install_path, images, loader_exports)
            if {entry["symbol"] for entry in resolutions if not entry["weak"]} != imports:
                raise ValueError(name + " native import inventories disagree")
            if symbols(archive)[0] != exports:
                raise ValueError(name + " did not retain every upstream public API")
            results[name] = {"sha256": digest(image), "exports": sorted(exports),
                             "imports": sorted(imports), "weak_imports": sorted(weak),
                             "import_resolutions": resolutions}
        pack(tree, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "libraries": results,
            "auth_bundle_sha256": digest(args.auth), "system_bundle_sha256": digest(args.system),
            "dyld_sha256": digest(args.dyld), "bundle_sha256": digest(args.output),
            "runtime_checked": False,
            "validation": "All upstream authentication objects retained as signed native Mach-O dylibs; every strong nlist import resolves through its actual dependency ordinal and reexport closure, or the matching dyld."}, indent=2) + "\n")
        print(f"Native X11 authentication {args.arch}: both complete signed dylibs have genuine strong closure")


if __name__ == "__main__":
    main()
