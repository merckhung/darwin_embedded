"""Ad-hoc SHA-256 signing for the source-built native Mach-O userland.

Uses the public XNU CodeDirectory/SuperBlob ABI. This grants no entitlements
or publisher trust. CS_HARD and CS_KILL require rejection of changed pages.
"""
import hashlib
from pathlib import Path
import struct


def sign(path: Path):
    data = bytearray(path.read_bytes())
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xfeedfacf:
        raise ValueError("expected a little-endian 64-bit Mach-O")
    _, _, _, filetype, count, size, _, _ = struct.unpack_from("<8I", data)
    if filetype not in (2, 6, 7, 8) or size > len(data) - 32:
        raise ValueError("expected a native executable, dylib, interpreter or bundle")
    signature = linkedit = text = None
    cursor = 32
    for _ in range(count):
        if cursor + 8 > 32 + size:
            raise ValueError("truncated load command")
        command, length = struct.unpack_from("<II", data, cursor)
        if length < 8 or length % 8 or cursor + length > 32 + size:
            raise ValueError("invalid load command size")
        if command == 0x1d:
            if length != 16 or signature is not None:
                raise ValueError("invalid signature command")
            signature = cursor
        elif command == 0x19:
            if length < 72:
                raise ValueError("truncated segment command")
            name = bytes(data[cursor + 8:cursor + 24]).rstrip(b"\0")
            if name == b"__LINKEDIT":
                if linkedit is not None:
                    raise ValueError("duplicate link-edit segment")
                linkedit = cursor
            elif name == b"__TEXT":
                if text is not None:
                    raise ValueError("duplicate text segment")
                text = cursor
        cursor += length
    if cursor != 32 + size or None in (signature, linkedit, text):
        raise ValueError("missing or inconsistent signing metadata")
    offset, old_size = struct.unpack_from("<II", data, signature + 8)
    link_offset, link_size = struct.unpack_from("<QQ", data, linkedit + 40)
    if offset < 32 + size or offset % 16 or offset + old_size != len(data) or offset < link_offset:
        raise ValueError("signature must be the final aligned file region")
    del data[offset:]
    identifier = path.name.encode("utf-8") + b"\0"
    slots = (offset + 4095) // 4096
    hash_offset = 88 + len(identifier)
    directory_size = hash_offset + slots * 32
    signature_size = 20 + directory_size
    end = offset + signature_size
    struct.pack_into("<I", data, signature + 12, signature_size)
    struct.pack_into("<Q", data, linkedit + 48, end - link_offset)
    struct.pack_into("<Q", data, linkedit + 32, (end - link_offset + 16383) & ~16383)
    text_offset, text_size = struct.unpack_from("<QQ", data, text + 40)
    # Ad-hoc | hard | kill | linker-signed, with no privileged flags.
    directory = struct.pack(">9I4B4I4Q", 0xfade0c02, directory_size, 0x20400,
                            0x20302, hash_offset, 88, 0, slots, offset,
                            32, 2, 0, 12, 0, 0, 0, 0, 0,
                            text_offset, text_size, int(filetype == 2))
    hashes = b"".join(hashlib.sha256(data[start:min(start + 4096, offset)]).digest()
                      for start in range(0, offset, 4096))
    superblob = struct.pack(">5I", 0xfade0cc0, signature_size, 1, 0, 20)
    path.write_bytes(data + superblob + directory + identifier + hashes)
