#!/usr/bin/env python3
"""Link complete native GTK3/GDK X11 libraries and retain real desktop services."""
import argparse
import json
from pathlib import Path
import re
import tarfile
import tempfile

from link_epoxy import metadata_for as epoxy_metadata, retain_programs
from link_cairo import link_stack
from build_dependencies import digest
from native_components import run
from sign_diagnostic_macho import sign
from link_auth import image_metadata, verify_imports
from build_clients import pack
from build_gtk import VERSION


def metadata_for(root, names):
    metadata = epoxy_metadata(root, set(names) - {"libgdk-3", "libgtk-3"})
    source = root / "src/Applications/widgets/gtk" / VERSION / "meson.build"
    text = source.read_text()
    if "osx_current = gtk_binary_age - gtk_interface_age + 1" not in text or \
       "gtk_osxversions = [osx_current, '@0@.@1@.0'.format(osx_current, gtk_interface_age)]" not in text:
        raise ValueError("GTK3 Darwin interface convention changed")
    _, minor, micro = map(int, VERSION.split("."))
    age = int(re.search(r"^gtk_interface_age\s*=\s*(\d+)$", text, re.M)[1])
    current = 100 * minor + micro - age + 1
    for name in ("libgdk-3", "libgtk-3"):
        metadata[name] = {"major": "0", "compatibility": f"{current}.0.0", "current": f"{current}.{age}.0",
                          "source": str(source.relative_to(root)), "source_sha256": digest(source)}
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "libraries", "build-report", "services-runtime", "services-report", "system", "dyld", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    link_stack(args, metadata_for)
    report = json.loads(args.report.read_text()); retain_programs(args, report)
    compiled = json.loads(args.build_report.read_text())
    with tempfile.TemporaryDirectory(prefix=".native-gtk-modules-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); libraries, tree = work / "libraries", work / "runtime"
        for source, destination in ((args.libraries, libraries), (args.output, tree)):
            with tarfile.open(source) as archive: archive.extractall(destination, filter="data")
        module = tree / "usr/lib/gtk-3.0/3.0.0/printbackends/libprintbackend-file.so"
        module.parent.mkdir(parents=True, exist_ok=True)
        archive = libraries / "usr/libexec/gtk/printbackend-file.a"
        run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-bundle", "-no_fixup_chains", "-dead_strip_dylibs",
             "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup", "-force_load", archive,
             *[tree / record["install_path"].lstrip("/") for record in report["libraries"].values()],
             "-L", tree / "usr/lib/system", "-o", module])
        sign(module)
        path = "/" + str(module.relative_to(tree))
        images = {"/" + str(p.relative_to(tree)): image_metadata(p.read_bytes()) for p in tree.rglob("*.dylib")}
        images[path] = image_metadata(module.read_bytes())
        loader = image_metadata(args.dyld.read_bytes())["exports"] | {"dyld_stub_binder"}
        report["modules"] = {"printbackend-file": {"install_path": path, "sha256": digest(module), "archive_sha256": digest(archive),
            "import_resolutions": verify_imports(path, images, loader, flat_namespace_root=path)}}
        if digest(archive) != compiled["modules"]["printbackend-file"]["archive_sha256"]: raise ValueError("GTK module archive mismatch")
        pack(tree, args.output); report["bundle_sha256"] = digest(args.output)
    report["build_report_sha256"] = digest(args.build_report)
    report.update(x11_backend=True, opengl_provider_supplied=False)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Native GTK3 {args.arch}: 79 signed libraries; original X11 widgets and native services")


if __name__ == "__main__": main()
