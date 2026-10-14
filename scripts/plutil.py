#!/usr/bin/env python3
"""Portable plist operations used by the XNU makefiles."""
import plistlib
import sys
from pathlib import Path


def main():
    args = [arg for arg in sys.argv[1:] if arg != "-s"]
    operation = args.pop(0)
    if operation == "-lint":
        for name in args:
            plistlib.loads(Path(name).read_bytes())
    elif operation == "-convert":
        kind, name = args
        formats = {"binary1": plistlib.FMT_BINARY, "xml1": plistlib.FMT_XML}
        path = Path(name)
        contents = plistlib.loads(path.read_bytes())
        path.write_bytes(plistlib.dumps(contents, fmt=formats[kind], sort_keys=False))
    elif operation == "-extract":
        key, kind, *rest = args
        contents = plistlib.loads(Path(rest[-1]).read_bytes())
        for component in key.split("."):
            contents = contents[component]
        if kind != "raw":
            raise ValueError("only raw extraction is supported")
        print(contents, end="" if "-n" in rest else "\n")
    else:
        raise ValueError(f"unsupported plist operation: {operation}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, IndexError, plistlib.InvalidFileException) as error:
        print(f"plutil: {error}", file=sys.stderr)
        sys.exit(1)
