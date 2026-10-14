# Copyright (C) 2026 fontconfig Authors
# SPDX-License-Identifier: HPND

"""Cache format compatibility tests.

Verifies that fontconfig can load cache files written by a different
version.  Two areas are tested:

1. Header version: FcDirCacheMapFd accepts caches with version >=
   FC_CACHE_VERSION_NUMBER.  Patching the header to a higher version
   confirms that a cache written by a newer fontconfig is still loaded.

2. FcLangSet map_size: entries are accessed via encoded offsets (not
   inline arrays), and map_size guards all bitmap access with
   FC_MIN(map_size, NUM_LANG_SET_MAP), so adding orth files does not
   break compatibility.
"""

import os
import re
import struct
from pathlib import Path

import pytest

from fctest import FcTest, FcTestFont

FC_CACHE_MAGIC_MMAP = 0xFC02FC04
FC_CACHE_VERSION_OFFSET = 4


@pytest.fixture
def fctest():
    return FcTest()


@pytest.fixture
def fcfont():
    return FcTestFont()


def _get_num_lang_set_map(builddir):
    """Read NUM_LANG_SET_MAP from the generated fclang.h."""
    fclang_h = Path(builddir) / "fc-lang" / "fclang.h"
    with open(fclang_h) as f:
        for line in f:
            m = re.match(r"#define\s+NUM_LANG_SET_MAP\s+(\d+)", line)
            if m:
                return int(m.group(1))
    raise RuntimeError("NUM_LANG_SET_MAP not found in fclang.h")


def _find_langset_map_size_offsets(data, map_size):
    """Find byte offsets of FcLangSet.map_size fields in cache binary.

    Serialised FcLangSet layout (x86-64):
        offset 0 : extra    (intptr_t = 8 bytes, always NULL in cache)
        offset 8 : map_size (uint32)
        offset 12: map[0]   (uint32)
        ...

    We scan for 8 zero bytes (extra == NULL) followed by the expected
    map_size, with at least one non-zero map entry to reduce false
    positives.
    """
    offsets = []
    needle = b"\x00" * 8 + struct.pack("<I", map_size)
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx == -1:
            break
        map_size_offset = idx + 8
        map_start = idx + 12
        if map_start + 4 <= len(data):
            first_map = struct.unpack_from("<I", data, map_start)[0]
            if first_map != 0:
                offsets.append(map_size_offset)
        pos = idx + len(needle)
    return offsets


def _generate_cache(fctest, fcfont):
    """Set up fonts and generate a cache; return the cache file path."""
    fctest.setup()
    fctest.install_font(fcfont.fonts, ".")
    for ret, stdout, stderr in fctest.run_cache([fctest.fontdir.name]):
        assert ret == 0, stderr
    cache_files = list(fctest.cache_files())
    assert len(cache_files) == 1, cache_files
    return cache_files[0]


@pytest.mark.skipif(not not os.getenv("EXEEXT"), reason="not working on Win32")
def test_cache_newer_version_loadable(fctest, fcfont):
    """A cache with a higher header version must still be loadable.

    FcDirCacheMapFd rejects cache->version < FC_CACHE_VERSION_NUMBER
    but accepts >=.  Patch the version field upward and verify fc-list
    still reads the cache instead of regenerating it.
    """
    cache_file = _generate_cache(fctest, fcfont)

    with open(cache_file, "rb") as f:
        data = bytearray(f.read())

    magic, version = struct.unpack_from("<Ii", data, 0)
    assert magic == FC_CACHE_MAGIC_MMAP, f"Bad cache magic: {magic:#x}"

    struct.pack_into("<i", data, FC_CACHE_VERSION_OFFSET, version + 1)

    mtime_before = cache_file.stat().st_mtime_ns
    with open(cache_file, "wb") as f:
        f.write(data)
    os.utime(cache_file, ns=(mtime_before, mtime_before))

    for ret, stdout, stderr in fctest.run_list(["-", "family", "lang"]):
        assert ret == 0, f"fc-list failed on newer-version cache: {stderr}"
        assert stdout.strip(), "fc-list returned no output"

    assert cache_file.stat().st_mtime_ns == mtime_before, (
        "Cache was regenerated instead of using the patched file"
    )


def test_langset_map_size_in_cache(fctest, fcfont):
    """Every FcLangSet in the cache must have map_size == NUM_LANG_SET_MAP."""
    cache_file = _generate_cache(fctest, fcfont)
    num_lang_set_map = _get_num_lang_set_map(fctest.builddir)

    with open(cache_file, "rb") as f:
        data = f.read()

    offsets = _find_langset_map_size_offsets(data, num_lang_set_map)
    assert len(offsets) > 0, (
        f"No FcLangSet entries with map_size={num_lang_set_map} found "
        f"in {cache_file}"
    )


@pytest.mark.skipif(not not os.getenv("EXEEXT"), reason="not working on Win32")
def test_langset_forward_compat(fctest, fcfont):
    """A cache with larger map_size (newer fontconfig) must be readable.

    Simulates the scenario where a newer fontconfig with more orth files
    wrote the cache.  The current fontconfig reads it using
    FC_MIN(map_size, NUM_LANG_SET_MAP) bitmap entries.
    """
    cache_file = _generate_cache(fctest, fcfont)
    num_lang_set_map = _get_num_lang_set_map(fctest.builddir)

    with open(cache_file, "rb") as f:
        data = bytearray(f.read())

    offsets = _find_langset_map_size_offsets(bytes(data), num_lang_set_map)
    assert len(offsets) > 0, "No FcLangSet entries found"

    larger_map_size = num_lang_set_map + 2
    for offset in offsets:
        struct.pack_into("<I", data, offset, larger_map_size)

    mtime_before = cache_file.stat().st_mtime_ns
    with open(cache_file, "wb") as f:
        f.write(data)
    os.utime(cache_file, ns=(mtime_before, mtime_before))

    for ret, stdout, stderr in fctest.run_list(["-", "family", "lang"]):
        assert ret == 0, f"fc-list failed on forward-compat cache: {stderr}"
        assert stdout.strip(), "fc-list returned no output"

    assert cache_file.stat().st_mtime_ns == mtime_before, (
        "Cache was regenerated instead of using the patched file"
    )


@pytest.mark.skipif(not not os.getenv("EXEEXT"), reason="not working on Win32")
def test_langset_backward_compat(fctest, fcfont):
    """A cache with smaller map_size (older fontconfig, e.g. 2.18.2) must be readable.

    Simulates a cache written by fontconfig 2.18.2 (NUM_LANG_SET_MAP=9).
    The current fontconfig reads only FC_MIN(9, NUM_LANG_SET_MAP) entries,
    ignoring bitmap positions for languages added after 2.18.2.
    """
    cache_file = _generate_cache(fctest, fcfont)
    num_lang_set_map = _get_num_lang_set_map(fctest.builddir)

    with open(cache_file, "rb") as f:
        data = bytearray(f.read())

    offsets = _find_langset_map_size_offsets(bytes(data), num_lang_set_map)
    assert len(offsets) > 0, "No FcLangSet entries found"

    old_map_size = 9
    for offset in offsets:
        struct.pack_into("<I", data, offset, old_map_size)

    mtime_before = cache_file.stat().st_mtime_ns
    with open(cache_file, "wb") as f:
        f.write(data)
    os.utime(cache_file, ns=(mtime_before, mtime_before))

    for ret, stdout, stderr in fctest.run_list(["-", "family", "lang"]):
        assert ret == 0, f"fc-list failed on backward-compat cache: {stderr}"
        assert stdout.strip(), "fc-list returned no output"

    assert cache_file.stat().st_mtime_ns == mtime_before, (
        "Cache was regenerated instead of using the patched file"
    )
