#!/usr/bin/env python3
"""Link complete native GdkPixbuf with its existing Pango/Cairo/GIO stack."""
import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "fonts"))
from link_cairo import link_stack
from link_pango import metadata_for as pango_metadata
from build_dependencies import digest


def metadata_for(root, names):
    metadata = pango_metadata(root, set(names) - {"libgdk_pixbuf-2.0"})
    source = root / "src/Applications/gtk/gdk-pixbuf/2.44.8/meson.build"
    text = source.read_text()
    if "darwin_versions = ['@0@'.format(current + 1), '@0@.@1@'.format(current + 1, gdk_pixbuf_interface_age)]" not in text:
        raise ValueError("GdkPixbuf Darwin version convention changed")
    _, minor, micro = map(int, re.search(r"version:\s*'([0-9.]+)'", text)[1].split("."))
    interface_age = 0 if minor % 2 else micro
    current = 100 * minor + micro - interface_age + 1
    metadata["libgdk_pixbuf-2.0"] = {"major": "0", "compatibility": f"{current}.0.0", "current": f"{current}.{interface_age}.0",
                                     "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    link_stack(parser.parse_args(), metadata_for)


if __name__ == "__main__":
    main()
