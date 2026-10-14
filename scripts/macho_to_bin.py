#!/usr/bin/env python3
"""Flatten a 64-bit Mach-O executable, including zero-filled memory."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

CPU_ARCHES = {0x01000007: "x86_64", 0x0100000C: "arm64"}
MAX_IMAGE_SIZE = 256 * 1024 * 1024


def parse_macho(data):
    if len(data) < 32:
        raise ValueError("truncated Mach-O header")
    magic, cpu, subtype, filetype, count, command_bytes, flags, reserved = struct.unpack_from(
        "<8I", data)
    if magic != 0xFEEDFACF or cpu not in CPU_ARCHES or filetype not in (2, 12):
        raise ValueError("expected an x86_64 or ARM64 Mach-O executable or fileset")
    command_end = 32 + command_bytes
    if command_end > len(data):
        raise ValueError("truncated load commands")
    offset = 32
    segments = []
    entry = None
    main_offset = None
    for _ in range(count):
        if offset + 8 > command_end:
            raise ValueError("truncated load command")
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or size % 8 or offset + size > command_end:
            raise ValueError("invalid load command size")
        if command == 0x19:
            if size < 72:
                raise ValueError("truncated segment command")
            name, address, memory_size, file_offset, file_size, maxprot, initprot, sections, segflags = (
                struct.unpack_from("<16s4Q4I", data, offset + 8))
            name = name.rstrip(b"\0").decode("ascii")
            if size < 72 + sections * 80:
                raise ValueError("truncated section records")
            if file_size > memory_size or file_offset + file_size > len(data):
                raise ValueError("segment exceeds its memory or file bounds")
            if address + memory_size > (1 << 64):
                raise ValueError("segment address overflow")
            if name != "__PAGEZERO" and memory_size:
                segments.append(dict(name=name, address=address, memory_size=memory_size,
                                     file_offset=file_offset, file_size=file_size,
                                     permissions=initprot))
        elif command == 0x5:  # LC_UNIXTHREAD
            if size < 16:
                raise ValueError("truncated thread command")
            flavor, words = struct.unpack_from("<II", data, offset + 8)
            expected_flavor, expected_words, pc_offset = (
                (6, 68, 256) if cpu == 0x0100000C else (4, 42, 128))
            if flavor != expected_flavor or words != expected_words or size < 16 + words * 4:
                raise ValueError("unsupported or truncated initial thread state")
            entry = struct.unpack_from("<Q", data, offset + 16 + pc_offset)[0]
        elif command == 0x80000028:  # LC_MAIN
            if size < 24:
                raise ValueError("truncated main command")
            main_offset = struct.unpack_from("<Q", data, offset + 8)[0]
        offset += size
    if offset != command_end:
        raise ValueError("load command count does not match their declared size")
    if not segments:
        raise ValueError("no loadable segments")
    segments.sort(key=lambda segment: segment["address"])
    for index, first in enumerate(segments):
        for second in segments[index + 1:]:
            if first["address"] + first["memory_size"] <= second["address"]:
                break
            if filetype != 12:
                raise ValueError("overlapping memory segments")
            # Filesets publish aggregate mappings over their constituent
            # segments. Initialized overlapping bytes must still agree.
            start = max(first["address"], second["address"])
            end = min(first["address"] + first["file_size"],
                      second["address"] + second["file_size"])
            if start < end:
                a = first["file_offset"] + start - first["address"]
                b = second["file_offset"] + start - second["address"]
                if data[a:a + end - start] != data[b:b + end - start]:
                    raise ValueError("conflicting initialized fileset segments")
    if entry is None and main_offset is not None:
        text = next((segment for segment in segments if segment["name"] == "__TEXT"), None)
        if text is None or not text["file_offset"] <= main_offset < text["file_offset"] + text["file_size"]:
            raise ValueError("main entry is outside the text file range")
        entry = text["address"] + main_offset - text["file_offset"]
    if entry is None or not any(
            segment["permissions"] & 4 and
            segment["address"] <= entry < segment["address"] + segment["file_size"]
            for segment in segments):
        raise ValueError("entry point is outside loaded executable bytes")
    base = segments[0]["address"]
    image_size = max(segment["address"] + segment["memory_size"] for segment in segments) - base
    if image_size > MAX_IMAGE_SIZE:
        raise ValueError("flat image exceeds the 256 MiB limit")
    return dict(architecture=CPU_ARCHES[cpu], cpu_type=cpu, cpu_subtype=subtype,
                virtual_base=base, virtual_entry=entry, image_size=image_size,
                segments=segments, input_sha256=hashlib.sha256(data).hexdigest())


def flatten(data, layout):
    image = bytearray(layout["image_size"])
    for segment in layout["segments"]:
        start = segment["address"] - layout["virtual_base"]
        file_offset, file_size = segment["file_offset"], segment["file_size"]
        image[start:start + file_size] = data[file_offset:file_offset + file_size]
    return image


def convert_macho_to_bin(input_file, output_file):
    data = Path(input_file).read_bytes()
    layout = parse_macho(data)
    Path(output_file).write_bytes(flatten(data, layout))
    return layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    addresses = parser.add_mutually_exclusive_group()
    addresses.add_argument("--load-address", type=lambda value: int(value, 0))
    addresses.add_argument("--virtual-bias", type=lambda value: int(value, 0))
    parser.add_argument("--layout", type=Path)
    parser.add_argument("--header", type=Path)
    args = parser.parse_args()
    if (args.layout or args.header) and args.load_address is None and args.virtual_bias is None:
        parser.error("--layout and --header require --load-address or --virtual-bias")
    data = args.input.read_bytes()
    layout = parse_macho(data)
    if args.virtual_bias is not None:
        if not 0 <= args.virtual_bias < (1 << 64):
            raise ValueError("invalid virtual bias")
        args.load_address = layout["virtual_base"] - args.virtual_bias
    if args.load_address is not None:
        if args.load_address < 0 or args.load_address + layout["image_size"] > (1 << 64):
            raise ValueError("physical image address overflow")
        layout["physical_base"] = args.load_address
        layout["physical_entry"] = args.load_address + layout["virtual_entry"] - layout["virtual_base"]
        layout["virtual_bias"] = layout["virtual_base"] - args.load_address
        if layout["virtual_bias"] < 0:
            raise ValueError("physical base exceeds the virtual base")
    args.output.write_bytes(flatten(data, layout))
    if args.layout:
        args.layout.write_text(json.dumps(layout, indent=2) + "\n")
    if args.header:
        definitions = {
            "KERNEL_CPU_TYPE": layout["cpu_type"],
            "KERNEL_LOAD_ADDRESS": layout["physical_base"],
            "KERNEL_ENTRY_ADDRESS": layout["physical_entry"],
            "KERNEL_VIRTUAL_BIAS": layout["virtual_bias"],
            "KERNEL_IMAGE_SIZE": layout["image_size"],
        }
        args.header.write_text("/* Generated from the linked Mach-O; do not edit. */\n"
                               "#pragma once\n" + "".join(
                                   f"#define {name} 0x{value:x}ULL\n"
                                   for name, value in definitions.items()))
    print(f'{layout["architecture"]}: {layout["image_size"]} bytes, '
          f'entry 0x{layout["virtual_entry"]:x}')


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        print(f"macho_to_bin: {error}", file=sys.stderr)
        sys.exit(1)
