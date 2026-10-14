#!/usr/bin/env python3
"""Link native libxml2 with its original Darwin versions and existing stack."""
import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dbus"))
from link_dbus import metadata_for as dbus_metadata
from link_cairo import link_stack
from build_dependencies import digest
from build_xml import VERSION


def metadata_for(root, names):
    metadata = dbus_metadata(root, set(names) - {"libxml2"})
    source = root / "src/Applications/xml/libxml2" / VERSION / "meson.build"
    text = source.read_text()
    if "darwin_compat = v_maj + v_min + 1" not in text or "darwin_versions: [ darwin_compat, f'@darwin_compat@.@v_mic@' ]" not in text:
        raise ValueError("original libxml2 Darwin interface convention changed")
    major, minor, micro = map(int, VERSION.split("."))
    minimum = int(re.search(r"^v_min_compat\s*=\s*(\d+)$", text, re.M)[1])
    metadata["libxml2"] = {"major": str(major + minimum), "compatibility": f"{major + minor + 1}.0.0", "current": f"{major + minor + 1}.{micro}.0",
                           "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    link_stack(parser.parse_args(), metadata_for)


if __name__ == "__main__": main()
