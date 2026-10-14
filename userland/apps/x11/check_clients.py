#!/usr/bin/env python3
"""Audit pinned upstream bytes and every archived native client object."""
import hashlib
import json
from pathlib import Path
import struct
import tarfile
import unittest

from link_auth import image_metadata, load_commands, verify_imports


def sha(data):
    return hashlib.sha256(data).hexdigest()


def archive_objects(data, cpu):
    if data[:8] != b"!<arch>\n":
        raise ValueError("not a static library")
    offset, objects = 8, []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise ValueError("invalid archive member")
        name = header[:16].decode().strip()
        length = int(header[48:58])
        member = data[offset + 60:offset + 60 + length]
        if len(member) != length:
            raise ValueError("truncated archive member")
        if name.startswith("#1/"):
            name_length = int(name[3:])
            name = member[:name_length].rstrip(b"\0").decode()
            member = member[name_length:]
        name = name.rstrip("/")
        if not name.startswith("__.SYMDEF"):
            if len(member) < 32 or struct.unpack_from("<4I", member) != (0xfeedfacf, cpu, 0 if cpu == 0x100000c else 3, 1):
                raise ValueError("archive member is not the exact native object type: " + name)
            objects.append(name)
        offset += 60 + length + (length & 1)
    if offset != len(data) or not objects:
        raise ValueError("invalid archive extent or empty library")
    return objects


def tar_files(path):
    with tarfile.open(path) as archive:
        return {entry.name: archive.extractfile(entry).read() for entry in archive if entry.isfile()}


def verify_code_signature(data):
    """Independently verify the emitted XNU SHA-256 CodeDirectory page table."""
    signatures = [(offset, length) for command, offset, length in load_commands(data)
                  if command == 0x1d]
    if len(signatures) != 1 or signatures[0][1] != 16:
        raise ValueError("missing or invalid signature command")
    start, length = struct.unpack_from("<2I", data, signatures[0][0] + 8)
    if start < 32 + struct.unpack_from("<I", data, 20)[0] or length < 108 or start + length != len(data):
        raise ValueError("invalid signature extent")
    magic, size, count, slot, offset = struct.unpack_from(">5I", data, start)
    if (magic, size, count, slot) != (0xfade0cc0, length, 1, 0) or offset < 20 or offset + 88 > length:
        raise ValueError("invalid signature superblob")
    directory = start + offset
    magic, size, version, flags, hashes, identifier, special, pages, limit = struct.unpack_from(
        ">9I", data, directory)
    hash_size, hash_type, _, shift = struct.unpack_from(">4B", data, directory + 36)
    if magic != 0xfade0c02 or version != 0x20400 or size != length - offset or flags != 0x20302:
        raise ValueError("unexpected native CodeDirectory")
    if (hash_size, hash_type, shift, special) != (32, 2, 12, 0):
        raise ValueError("unsupported native code-page hash format")
    if limit != start or pages != (limit + 4095) // 4096:
        raise ValueError("signature does not cover every pre-signature byte")
    if hashes < 88 or hashes + pages * hash_size != size or not 88 <= identifier < hashes:
        raise ValueError("invalid CodeDirectory page table")
    if data.find(b"\0", directory + identifier, directory + hashes) < 0:
        raise ValueError("unterminated code-signing identifier")
    scatter, _, _, limit64 = struct.unpack_from(">3IQ", data, directory + 44)
    if scatter or limit64:
        raise ValueError("unexpected scattered or extended code limit")
    for page in range(pages):
        content = data[page * 4096:min((page + 1) * 4096, limit)]
        expected = data[directory + hashes + page * hash_size:directory + hashes + (page + 1) * hash_size]
        if hashlib.sha256(content).digest() != expected:
            raise ValueError(f"code page hash mismatch: {page}")
    return pages


class NativeClients(unittest.TestCase):
    def test_pinned_upstream_bytes(self):
        manifests = sorted(Path("src/Applications/x11").glob("*/*/UPSTREAM.json"))
        self.assertEqual(len(manifests), 7)
        for path in manifests:
            manifest = json.loads(path.read_text())
            self.assertTrue(manifest["archive_url"].startswith("https://xorg.freedesktop.org/archive/individual/"))
            self.assertEqual(len(manifest["archive_sha256"]), 64)
            self.assertEqual(manifest["local_changes"], [])
            expected = manifest["upstream_files_sha256"]
            actual = {str(file.relative_to(path.parent)): sha(file.read_bytes())
                      for file in path.parent.rglob("*") if file.is_file() and file != path}
            self.assertEqual(actual, expected, path)

    def test_complete_native_archives(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            files = tar_files(Path(f"userland/apps/x11/xlib.{arch}.tar"))
            reports = {name: json.loads(Path(f"userland/apps/x11/{name}.{arch}.json").read_text())
                       for name in ("auth", "xcb", "xlib")}
            expected = {}
            for stage in ("auth", "xcb"):
                for component in reports[stage]["components"]:
                    expected[component["component"] + ".a"] = len(component["sources"])
            expected["libX11.a"] = len(reports["xlib"]["objects"])
            expected["libX11-xcb.a"] = 1
            expected["libnative_x11_libc.a"] = 1
            actual = {Path(name).name: len(archive_objects(data, cpu))
                      for name, data in files.items() if name.endswith(".a")}
            self.assertEqual(actual, expected)
            self.assertEqual(sum(actual.values()), 465)
            imports = Path(f"userland/apps/x11/xlib.{arch}.imports").read_text().splitlines()
            self.assertEqual(imports, reports["xlib"]["native_system_imports"])
            self.assertEqual(set(imports) & {"_getaddrinfo", "_freeaddrinfo", "_gai_strerror"},
                             {"_getaddrinfo", "_freeaddrinfo", "_gai_strerror"})
            self.assertNotIn("___dso_handle", imports)
            self.assertFalse(any(name.startswith(("_X", "__X", "_xcb_", "__xcb_")) for name in imports))

    def test_native_archive_rejects_host_code(self):
        files = tar_files(Path("userland/apps/x11/xlib.arm64.tar"))
        archive = files["usr/lib/libXau.a"]
        with self.assertRaises(ValueError):
            archive_objects(archive, 0x1000007)
        damaged = archive.replace(b"\xcf\xfa\xed\xfe", b"\x7fELF", 1)
        self.assertNotEqual(archive, damaged)
        with self.assertRaises(ValueError):
            archive_objects(damaged, 0x100000c)

    def test_architecture_independent_public_contract(self):
        # Apple's real x86_64 ABI versions stat/fstat with $INODE64; its
        # compiler also selects libplatform's published __bzero entry point.
        # Keep those actual spellings in production manifests and normalize
        # only this explicit equivalence set for a cross-CPU coverage check.
        pc_spellings = {"_stat$INODE64": "_stat", "_fstat$INODE64": "_fstat", "___bzero": "_bzero"}
        for stage in ("auth", "xcb", "xlib"):
            arm = tar_files(Path(f"userland/apps/x11/{stage}.arm64.headers.tar"))
            pc = tar_files(Path(f"userland/apps/x11/{stage}.x86_64.headers.tar"))
            self.assertEqual(arm, pc)
            arm_imports = set(Path(f"userland/apps/x11/{stage}.arm64.imports").read_text().splitlines())
            pc_imports = set(Path(f"userland/apps/x11/{stage}.x86_64.imports").read_text().splitlines())
            self.assertEqual(arm_imports, {pc_spellings.get(name, name) for name in pc_imports})
        self.assertNotIn("usr/include/xcb/xcbint.h", arm)

    def test_signed_authentication_images(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            bundle = Path(f"userland/apps/x11/auth-runtime.{arch}.tar")
            report = json.loads(Path(f"userland/apps/x11/auth-runtime.{arch}.json").read_text())
            self.assertEqual(sha(bundle.read_bytes()), report["bundle_sha256"])
            files = tar_files(bundle)
            for name in ("libXau", "libXdmcp"):
                data = files[f"usr/lib/{name}.6.dylib"]
                magic, actual_cpu, _, kind, commands = struct.unpack_from("<5I", data)
                self.assertEqual((magic, actual_cpu, kind), (0xfeedfacf, cpu, 6))
                self.assertGreater(verify_code_signature(data), 0)
                self.assertEqual(image_metadata(data)["dependencies"], [(0xc, "/usr/lib/libSystem.B.dylib")])
                self.assertEqual(sha(data), report["libraries"][name]["sha256"])

    def test_code_signature_rejects_changed_code_page(self):
        for arch in ("arm64", "x86_64"):
            files = tar_files(Path(f"userland/apps/x11/auth-runtime.{arch}.tar"))
            for name in ("libXau", "libXdmcp"):
                data = bytearray(files[f"usr/lib/{name}.6.dylib"])
                # Alter signed content beyond the load commands, preserving
                # their structure so rejection requires page-hash validation.
                changed = 32 + struct.unpack_from("<I", data, 20)[0] + 16
                data[changed] ^= 1
                with self.assertRaisesRegex(ValueError, "code page hash mismatch"):
                    verify_code_signature(data)

    def test_actual_authentication_dependency_ordinals(self):
        for arch in ("arm64", "x86_64"):
            files = tar_files(Path(f"userland/apps/x11/auth-runtime.{arch}.tar"))
            images = {"/" + name: image_metadata(data) for name, data in files.items() if name.endswith(".dylib")}
            loader = image_metadata(Path(f"userland/native/dyld.{arch}").read_bytes())["exports"]
            loader.add("dyld_stub_binder")  # Actual native dyld built-in.
            report = json.loads(Path(f"userland/apps/x11/auth-runtime.{arch}.json").read_text())
            for name in ("libXau", "libXdmcp"):
                path = f"/usr/lib/{name}.6.dylib"
                resolutions = verify_imports(path, images, loader)
                self.assertEqual(resolutions, report["libraries"][name]["import_resolutions"])
                self.assertTrue(all(item["provider"] for item in resolutions))
                data = bytearray(files[path[1:]])
                imported = next(entry for entry in images[path]["imports"] if entry["ordinal"] == 1)
                position = imported["nlist_offset"] + 6
                description = struct.unpack_from("<H", data, position)[0]
                struct.pack_into("<H", data, position, (description & 0xff) | (2 << 8))
                invalid = {**images, path: image_metadata(data)}
                with self.assertRaisesRegex(ValueError, "dependency ordinal 2"):
                    verify_imports(path, invalid, loader)
            # An available symbol in an installed component is insufficient:
            # the actual numbered facade must reexport that component.
            path = "/usr/lib/libXdmcp.6.dylib"
            provider = next(entry["provider"] for entry in verify_imports(path, images, loader)
                            if entry["symbol"] == "_arc4random_buf")
            facade = "/usr/lib/libSystem.B.dylib"
            invalid = {**images, facade: {**images[facade], "dependencies": [
                entry for entry in images[facade]["dependencies"] if entry[1] != provider]}}
            with self.assertRaisesRegex(ValueError, "no provider through ordinal 1: _arc4random_buf"):
                verify_imports(path, invalid, loader)


if __name__ == "__main__":
    unittest.main()
