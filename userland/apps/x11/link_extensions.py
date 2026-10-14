#!/usr/bin/env python3
"""Link complete native X11 extensions and their font/image/GIO dependencies."""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gtk"))
from link_pixbuf import metadata_for as pixbuf_metadata
from link_cairo import link_stack, x11_version
from build_dependencies import digest
from build_extensions import RELEASES, LIBRARIES


def metadata_for(root, names):
    metadata = pixbuf_metadata(root, set(names) - set(LIBRARIES.values()))
    for name, library in LIBRARIES.items():
        source = root / "src/Applications/desktop-x11" / name / RELEASES[name] / "src/Makefile.am"
        major, compatibility, current = x11_version(source, library)
        metadata[library] = {"major": major, "compatibility": compatibility, "current": current,
                             "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    if set(names) != set(metadata):
        raise ValueError("unexpected desktop X11 library inventory")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    link_stack(parser.parse_args(), metadata_for)


if __name__ == "__main__":
    main()
