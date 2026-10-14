#!/usr/bin/env python3
"""Regress the observed reexport-ordinal failure using actual native artifacts."""
from pathlib import Path
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path("scripts").resolve()))
from build_native_system import canonicalize_reexports, verify_image, verify_import_ordinals
from native_components import symbols

checks = 0
for arch in ("arm64", "x86_64"):
    with tempfile.TemporaryDirectory(prefix="native-system-packaging-") as directory:
        root = Path(directory)
        with tarfile.open(f"userland/native/system/base_{arch}.tar") as archive:
            archive.extractall(root, filter="data")
        facade = root / "usr/lib/libSystem.B.dylib"
        providers = {"/" + str(path.relative_to(root)): symbols(path)[0]
                     for path in (root / "usr/lib/system").glob("*.dylib")}
        verify_image(facade, arch, 0x8000001f)
        verify_import_ordinals(facade, providers)
        checks += 1
        original = facade.read_bytes()
        count, size = struct.unpack_from("<II", original, 16)
        commands, expected, offset, symtab = [], [], 32, None
        for _ in range(count):
            command, length = struct.unpack_from("<II", original, offset)
            item = original[offset:offset + length]
            if command == 0x8000001f:
                name_offset = struct.unpack_from("<I", item, 8)[0]
                expected.append(item[name_offset:].split(b"\0", 1)[0].decode())
                commands.append(struct.pack("<I", 0xc) + item[4:])
            elif command == 2:
                symtab = struct.unpack_from("<4I", item, 8)
            commands.append(item)
            offset += length

        # Reproduce LLVM20's paired commands with its unchanged unique-library
        # ordinals. Global symbol availability is unchanged; the image is bad.
        duplicate = bytearray(original)
        encoded = b"".join(commands)
        if any(original[32 + size:32 + len(encoded)]):
            raise AssertionError("expected original linker header padding")
        duplicate[32:32 + len(encoded)] = encoded
        struct.pack_into("<II", duplicate, 16, len(commands), len(encoded))
        facade.write_bytes(duplicate)
        try:
            verify_import_ordinals(facade, providers)
        except RuntimeError:
            checks += 1
        else:
            raise AssertionError("duplicate reexports escaped ordinal audit")
        canonicalize_reexports(facade, expected)
        if facade.read_bytes() != original:
            raise AssertionError("canonical repair changed unrelated native image bytes")
        verify_import_ordinals(facade, providers)
        checks += 1

        # A missing provider must fail even though another component exports
        # the name and the overall strong-symbol union still looks complete.
        damaged = bytearray(original)
        table, count, strings, string_size = symtab
        for index in range(count):
            item = table + index * 16
            name_offset, kind, _, description, _ = struct.unpack_from("<IBBHQ", damaged, item)
            name = original[strings + name_offset:].split(b"\0", 1)[0]
            if name == b"_malloc" and kind == 1:
                wrong = next(n + 1 for n, path in enumerate(expected) if "_malloc" not in providers[path])
                struct.pack_into("<H", damaged, item + 6, wrong << 8 | (description & 0xff))
                break
        else:
            raise AssertionError("actual facade has no allocator callback import")
        facade.write_bytes(damaged)
        try:
            verify_import_ordinals(facade, providers)
        except RuntimeError:
            checks += 1
        else:
            raise AssertionError("wrong import ordinal escaped audit")

print(f"Native packaging: {checks} real-artifact reexport/ordinal checks passed")
