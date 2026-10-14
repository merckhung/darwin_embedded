#!/usr/bin/env python3
"""Link complete native D-Bus dylibs and actual daemon/helper/tool programs."""
import argparse
import json
from pathlib import Path
import re
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_extensions import metadata_for as extension_metadata
from link_cairo import link_stack
from link_auth import image_metadata, verify_imports
from build_dependencies import definitions, digest
from native_components import run, symbols
from sign_diagnostic_macho import sign
from build_clients import pack
from build_dbus import VERSION


def metadata_for(root, names):
    metadata = extension_metadata(root, set(names) - {"libdbus-1"})
    source = root / "src/Applications/ipc/dbus" / VERSION / "meson.build"
    text = source.read_text()
    current, revision, age = [int(re.search(r"^lt_" + name + r"\s*=\s*(\d+)$", text, re.M)[1])
                              for name in ("current", "revision", "age")]
    if age > current:
        raise ValueError("invalid original D-Bus interface age")
    metadata["libdbus-1"] = {"major": str(current - age), "compatibility": f"{current + 1}.0.0", "current": f"{current + 1}.{revision}.0",
                            "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "build-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    link_stack(args, metadata_for)
    report = json.loads(args.report.read_text())
    compiled = json.loads(args.build_report.read_text())
    with tempfile.TemporaryDirectory(prefix=".native-dbus-programs-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        libraries, tree = work / "libraries", work / "runtime"
        for source, destination in ((args.libraries, libraries), (args.output, tree)):
            with tarfile.open(source) as archive:
                archive.extractall(destination, filter="data")
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        providers = {record["install_path"]: set(record["exports"]) for record in report["libraries"].values() if record["install_path"] != "/usr/lib/libSystem.B.dylib"}
        program_records = {}
        for name in compiled["programs"]:
            archives = [libraries / "usr/libexec/dbus" / ("program-" + name + ".a")]
            if name == "dbus-daemon":
                archives += [libraries / "usr/libexec/dbus/daemon-internal.a", libraries / "usr/libexec/dbus/dbus-internal.a"]
            elif name == "dbus-daemon-launch-helper":
                archives += [libraries / "usr/libexec/dbus/launch-helper-internal.a", libraries / "usr/libexec/dbus/dbus-internal.a"]
            elif name == "dbus-run-session":
                archives.append(libraries / "usr/libexec/dbus/dbus-internal.a")
            defined = set().union(*(definitions(archive) for archive in archives))
            imported = set().union(*(symbols(archive)[1] for archive in archives)) - defined
            needed = set()
            for symbol in imported:
                candidates = {path for path, public in providers.items() if symbol in public}
                if len(candidates) > 1:
                    raise ValueError("ambiguous native D-Bus program provider: " + symbol)
                needed |= candidates
            destination = "/usr/libexec/" + name if name == "dbus-daemon-launch-helper" else "/usr/bin/" + name
            executable = tree / destination.lstrip("/")
            executable.parent.mkdir(parents=True, exist_ok=True)
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
                "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
                "-undefined", "dynamic_lookup", "-o", executable, *archives,
                *[tree / path.lstrip("/") for path in sorted(needed)], "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
            sign(executable)
            images[destination] = image_metadata(executable.read_bytes())
            resolutions = verify_imports(destination, images, loader, flat_namespace_root=destination)
            program_records[name] = {"install_path": destination, "sha256": digest(executable),
                "source_archives": {str(archive.relative_to(libraries)): digest(archive) for archive in archives},
                "linked_dylibs": sorted(needed | {"/usr/lib/libSystem.B.dylib"}), "import_resolutions": resolutions}
        pack(tree, args.output)
        report.update(programs=program_records, bundle_sha256=digest(args.output), build_report_sha256=digest(args.build_report),
                      runtime_checked=False, validation="Complete signed native library stack and genuine D-Bus daemon/helper/tools; actual Mach-O provider bindings audited for every image.")
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Native D-Bus {args.arch}: {len(report['libraries'])} signed dylibs and {len(program_records)} actual signed programs")


if __name__ == "__main__":
    main()
