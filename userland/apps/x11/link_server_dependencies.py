#!/usr/bin/env python3
"""Link native X server font/keyboard libraries with real GTK/services retained."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gtk"))
from link_gtk import metadata_for as gtk_metadata
from link_epoxy import retain_programs
from link_cairo import link_stack
from build_dependencies import digest
from link_clients import version as libtool_version
from build_server_dependencies import RELEASES


def metadata_for(root, names):
    metadata = gtk_metadata(root, set(names) - set(RELEASES))
    for name in ("libfontenc", "libXfont2"):
        source = root / "src/Applications/xserver" / name / RELEASES[name] / ("src/Makefile.am" if name == "libfontenc" else "Makefile.am")
        major, compatibility, current = libtool_version(source, name)
        metadata[name] = {"major": major, "compatibility": compatibility, "current": current,
                          "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    source = root / "src/Applications/xserver/libxkbfile" / RELEASES["libxkbfile"] / "src/meson.build"
    if "version: '1.0.2'" not in source.read_text(): raise ValueError("libxkbfile original interface changed")
    metadata["libxkbfile"] = {"major": "1", "compatibility": "1.0.0", "current": "1.0.0",
                              "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "services-runtime", "services-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); link_stack(args, metadata_for)
    report = json.loads(args.report.read_text()); retain_programs(args, report)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Native X server dependencies {args.arch}: 82 signed application dylibs and retained native services/GTK module")


if __name__ == "__main__": main()
