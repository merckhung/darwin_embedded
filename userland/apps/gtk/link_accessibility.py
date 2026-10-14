#!/usr/bin/env python3
"""Link native accessibility libraries/services and retain the D-Bus programs."""
import argparse
import json
from pathlib import Path
import re
import tarfile
import tempfile

from link_xml import metadata_for as xml_metadata
from link_cairo import link_stack
from link_auth import image_metadata, verify_imports
from build_dependencies import definitions, digest
from native_components import run, symbols
from sign_diagnostic_macho import sign
from build_clients import pack
from build_accessibility import VERSION


def metadata_for(root, names):
    additions = {"libatk-1.0", "libatspi", "libatk-bridge-2.0"}
    metadata = xml_metadata(root, set(names) - additions)
    source = root / "src/Applications/accessibility/at-spi2-core" / VERSION / "meson.build"
    text = source.read_text()
    if "atk_binary_age = 10000 * atspi_major_version + 100 * atspi_minor_version + 10 + atspi_micro_version" not in text or \
       "darwin_versions = ['@0@'.format(current + 1), '@0@.@1@'.format(current + 1, atk_interface_age)]" not in text:
        raise ValueError("original ATK Darwin interface convention changed")
    major, minor, micro = map(int, VERSION.split("."))
    age = int(re.search(r"^atk_interface_age\s*=\s*(\d+)$", text, re.M)[1])
    current = 10000 * major + 100 * minor + 10 + micro - age
    metadata["libatk-1.0"] = {"major": "0", "compatibility": f"{current + 1}.0.0", "current": f"{current + 1}.{age}.0",
                              "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    # These two original library() declarations omit darwin_versions and
    # ATK's common_ldflags. Meson defaults both Darwin versions to soversion.
    for name, directory in (("libatspi", "atspi"), ("libatk-bridge-2.0", "atk-adaptor")):
        recipe = source.parent / directory / "meson.build"
        metadata[name] = {"major": "0", "compatibility": "0.0.0", "current": "0.0.0",
                          "source": str(recipe.relative_to(root)), "source_sha256": digest(recipe)}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "build-report", "dbus-runtime", "dbus-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    link_stack(args, metadata_for)
    report = json.loads(args.report.read_text())
    compiled = json.loads(args.build_report.read_text())
    dbus_report = json.loads(args.dbus_report.read_text())
    if digest(args.dbus_runtime) != dbus_report["bundle_sha256"] or dbus_report["architecture"] != args.arch:
        raise ValueError("D-Bus program bundle/report mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-accessibility-services-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        libraries, tree, dbus = work / "libraries", work / "runtime", work / "dbus"
        for source, destination in ((args.libraries, libraries), (args.output, tree), (args.dbus_runtime, dbus)):
            with tarfile.open(source) as archive: archive.extractall(destination, filter="data")
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        providers = {record["install_path"]: set(record["exports"]) for record in report["libraries"].values()}
        program_records = {}
        for name, record in dbus_report["programs"].items():
            destination = record["install_path"]
            original, executable = dbus / destination.lstrip("/"), tree / destination.lstrip("/")
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_bytes(original.read_bytes())
            if digest(executable) != record["sha256"]: raise ValueError("D-Bus program digest mismatch")
            images[destination] = image_metadata(executable.read_bytes())
            program_records[name] = {**record, "import_resolutions": verify_imports(destination, images, loader, flat_namespace_root=destination),
                                     "retained_from_runtime_sha256": digest(args.dbus_runtime)}
        for name, record in compiled["programs"].items():
            archive = libraries / "usr/libexec/accessibility" / (name + ".a")
            imported = symbols(archive)[1] - definitions(archive)
            needed = set()
            for symbol in imported:
                candidates = {path for path, public in providers.items() if symbol in public}
                if len(candidates) > 1: raise ValueError("ambiguous accessibility program provider: " + symbol)
                needed |= candidates
            destination = "/usr/libexec/" + name
            executable = tree / destination.lstrip("/")
            run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip", "-dead_strip_dylibs",
                 "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main", "-undefined", "dynamic_lookup",
                 "-o", executable, archive, *[tree / path.lstrip("/") for path in sorted(needed)],
                 "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
            sign(executable)
            images[destination] = image_metadata(executable.read_bytes())
            program_records[name] = {"install_path": destination, "sha256": digest(executable), "archive_sha256": digest(archive),
                "linked_dylibs": sorted(needed | {"/usr/lib/libSystem.B.dylib"}),
                "import_resolutions": verify_imports(destination, images, loader, flat_namespace_root=destination)}
        pack(tree, args.output)
        report.update(programs=program_records, bundle_sha256=digest(args.output), build_report_sha256=digest(args.build_report),
                      dbus_runtime_sha256=digest(args.dbus_runtime), runtime_checked=False)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Native accessibility {args.arch}: {len(report['libraries'])} signed dylibs, two accessibility services and ten retained D-Bus programs")


if __name__ == "__main__": main()
