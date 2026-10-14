#!/usr/bin/env python3
"""Link native libepoxy and retain every genuine accessibility/D-Bus program."""
import argparse
import json
from pathlib import Path
import tarfile
import tempfile

from link_accessibility import metadata_for as accessibility_metadata
from link_cairo import link_stack
from link_auth import image_metadata, verify_imports
from build_dependencies import digest
from build_clients import pack
from build_epoxy import VERSION


def metadata_for(root, names):
    metadata = accessibility_metadata(root, set(names) - {"libepoxy"})
    source = root / "src/Applications/widgets/libepoxy" / VERSION / "src/meson.build"
    if "darwin_versions = [1, '1.0']" not in source.read_text(): raise ValueError("libepoxy Darwin versions changed")
    metadata["libepoxy"] = {"major": "0", "compatibility": "1.0.0", "current": "1.0.0",
                            "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    return metadata


def retain_programs(args, report):
    previous = json.loads(args.services_report.read_text())
    if digest(args.services_runtime) != previous["bundle_sha256"] or previous["architecture"] != args.arch:
        raise ValueError("native services bundle/report mismatch")
    with tempfile.TemporaryDirectory(prefix=".native-services-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        tree, services = work / "runtime", work / "services"
        for source, destination in ((args.output, tree), (args.services_runtime, services)):
            with tarfile.open(source) as archive: archive.extractall(destination, filter="data")
        images = {"/" + str(path.relative_to(tree)): image_metadata(path.read_bytes()) for path in tree.rglob("*.dylib")}
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        records = {}
        for name, record in previous["programs"].items():
            path = record["install_path"]
            original, destination = services / path.lstrip("/"), tree / path.lstrip("/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(original.read_bytes())
            if digest(destination) != record["sha256"]: raise ValueError("native service digest mismatch")
            images[path] = image_metadata(destination.read_bytes())
            records[name] = {**record, "import_resolutions": verify_imports(path, images, loader, flat_namespace_root=path),
                             "retained_from_runtime_sha256": digest(args.services_runtime)}
        modules = {}
        for name, record in previous.get("modules", {}).items():
            path = record["install_path"]
            original, destination = services / path.lstrip("/"), tree / path.lstrip("/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(original.read_bytes())
            if digest(destination) != record["sha256"]: raise ValueError("native module digest mismatch")
            images[path] = image_metadata(destination.read_bytes())
            modules[name] = {**record, "import_resolutions": verify_imports(path, images, loader, flat_namespace_root=path),
                             "retained_from_runtime_sha256": digest(args.services_runtime)}
        pack(tree, args.output)
        report.update(programs=records, modules=modules, bundle_sha256=digest(args.output), services_runtime_sha256=digest(args.services_runtime), runtime_checked=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "services-runtime", "services-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    link_stack(args, metadata_for)
    report = json.loads(args.report.read_text())
    retain_programs(args, report)
    report["opengl_provider_supplied"] = False
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Native libepoxy {args.arch}: 77 signed dylibs and 12 retained native services/tools")


if __name__ == "__main__": main()
