#!/usr/bin/env python3
"""Link all required boot extensions with the matching kernel and KPI metadata."""
import argparse
from pathlib import Path
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builder", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--kext", type=Path, action="append", default=[])
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kc-base")
    args = parser.parse_args()
    args.workdir.mkdir(parents=True, exist_ok=True)
    sdk = args.workdir / "sdk"
    sdk.mkdir(exist_ok=True)
    with tarfile.open(args.sdk) as archive:
        archive.extractall(sdk, filter="data")
    command = [str(args.builder.resolve()), "-kernel", str(args.kernel.resolve())]
    if args.kc_base:
        command += ["-kc-base", args.kc_base]
    for index, path in enumerate(args.kext):
        destination = args.workdir / f"extension-{index}"
        destination.mkdir(exist_ok=True)
        with tarfile.open(path) as archive:
            archive.extractall(destination, filter="data")
        bundles = list(destination.glob("*.kext"))
        if len(bundles) != 1:
            parser.error(f"expected one extension bundle in {path}")
        command += ["-kext", str(bundles[0].resolve())]
    metadata = sorted((sdk / "System.kext").rglob("Info.plist"))
    if not metadata:
        parser.error("matching kernel KPI metadata is missing")
    for info in metadata:
        command += ["-codeless", str(info.parent.resolve())]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command += ["-o", str(args.output.resolve())]
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
