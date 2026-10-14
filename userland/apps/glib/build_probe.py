#!/usr/bin/env python3
"""Build a native dependency consumer and a real init/dash HFS probe root."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "x11"))
from link_auth import image_metadata, verify_imports
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from build_diagnostic_userland import verify_installed_files
from sign_diagnostic_macho import sign


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def catalog():
    """Original test input in the documented little-endian GNU MO format."""
    messages = {
        b"": b"Content-Type: text/plain; charset=UTF-8\nLanguage: zz_ZZ\nPlural-Forms: nplurals=2; plural=(n != 1);\n",
        b"apple\0apples": b"native apple\0native apples",
        b"greeting": "Darwin λ".encode(),
        b"latin": "saluté".encode(),
        b"place\4greeting": "context λ".encode(),
    }
    count = len(messages)
    original_table, translated_table = 28, 28 + 8 * count
    offset = 28 + 16 * count
    originals, translations, contents = [], [], bytearray()
    for text in sorted(messages):
        originals.append((len(text), offset + len(contents)))
        contents.extend(text + b"\0")
    for text in sorted(messages):
        value = messages[text]
        translations.append((len(value), offset + len(contents)))
        contents.extend(value + b"\0")
    return (struct.pack("<7I", 0x950412de, 0, count, original_table, translated_table, 0, 0) +
            b"".join(struct.pack("<2I", *record) for record in originals + translations) + contents)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "report", "exports", "probe",
                 "runtime", "system", "base-root", "dyld", "hfs-tool", "module"):
        parser.add_argument("--" + name, type=Path, required=name in ("root", "sdk", "output", "report"))
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--core", action="store_true")
    mode.add_argument("--gio", action="store_true")
    parser.add_argument("--module-build", action="store_true")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-dependencies-test-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch:
            raise ValueError("dependency probe SDK architecture mismatch")
        if args.exports or args.module_build:
            dependencies = work / "dependencies"
            with tarfile.open(args.dependencies) as archive:
                archive.extractall(dependencies, filter="data")
            source = args.root.resolve() / "userland/apps/glib" / (
                "module_glib.c" if args.module_build else "test_gio.c" if args.gio else "test_glib.c" if args.core else "test_dependencies.c")
            resource = run(["clang-20", "-print-resource-dir"]).strip()
            flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                     "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1",
                     "-D_FORTIFY_SOURCE=0", "-Wall", "-Wextra", "-Werror", "-Wno-availability",
                     "-isystem", sdk / "usr/include", "-isystem", resource + "/include",
                     "-I", dependencies / "usr/include", "-I", dependencies / "usr/include/glib-2.0"]
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
            object_path = work / "module.o" if args.module_build else args.output
            run([*flags, "-c", source, "-o", object_path])
            magic, cpu, _, kind = struct.unpack_from("<4I", object_path.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("dependency probe is not the native Mach-O object")
            imports = symbols(object_path)[1]
            provided = set().union(*(symbols(path)[0] for path in (dependencies / "usr/lib").glob("*.a")))
            requests = imports - provided
            if args.module_build:
                module_runtime = work / "module-runtime"
                with tarfile.open(args.runtime) as archive:
                    archive.extractall(module_runtime, filter="data")
                run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-bundle", "-no_fixup_chains",
                     "-platform_version", "macos", "26.0", "26.5", "-adhoc_codesign", "-undefined", "dynamic_lookup",
                     "-o", args.output, object_path, module_runtime / "usr/lib/libgmodule-2.0.0.dylib",
                     module_runtime / "usr/lib/libglib-2.0.0.dylib", "-L", module_runtime / "usr/lib/system",
                     module_runtime / "usr/lib/libSystem.B.dylib"])
                sign(args.output)
            else:
                args.exports.write_text("\n".join(sorted(requests)) + "\n")
            args.report.write_text(json.dumps({"architecture": args.arch, "imports": sorted(imports),
                "system_requests": sorted(requests), "source_sha256": sha(source), "object_sha256": sha(object_path),
                "source_inputs": {str(path.relative_to(args.root.resolve())): sha(path)
                                  for path in ([source, source.with_name("test_gio_ipc.c"), source.with_name("test_gio_dns.c")]
                                               if args.gio else [source])},
                "module_sha256": sha(args.output) if args.module_build else None}, indent=2) + "\n")
            return
        tree = work / "runtime"
        for bundle in (args.runtime, args.system):
            with tarfile.open(bundle) as archive:
                archive.extractall(tree, filter="data")
        program = "native-gio-test" if args.gio else "native-glib-test" if args.core else "native-dependencies-test"
        executable = work / program
        libraries = sorted(path for path in (tree / "usr/lib").glob("*.dylib") if path.name != "libSystem.B.dylib")
        run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", args.arch, "-no_fixup_chains", "-dead_strip",
             "-platform_version", "macos", "26.0" if args.core else "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
             "-undefined", "dynamic_lookup", "-o", executable, args.probe.resolve(), *libraries,
             "-L", tree / "usr/lib/system", tree / "usr/lib/libSystem.B.dylib"])
        sign(executable)
        image = work / "root.img"
        shutil.copyfile(args.base_root, image)

        def edit(*arguments):
            return run([args.hfs_tool.resolve(), image, *arguments])

        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        mo = work / "native-deps.mo"
        mo.write_bytes(catalog())
        installed = {"/bin/" + program: executable, "/usr/lib/dyld": args.dyld,
                     "/usr/share/locale/zz_ZZ.UTF-8/LC_MESSAGES/native-deps.mo": mo}
        installed.update({"/" + str(path.relative_to(tree)): path for path in (tree / "usr").rglob("*") if path.is_file()})
        fixture_generators = {}
        if args.gio:
            payload = work / "payload.txt"
            payload.write_text("Native GIO compressed resource: \u20ac\n")
            xml = work / "native.gresource.xml"
            xml.write_text('<gresources><gresource prefix="/native"><file compressed="true">payload.txt</file></gresource></gresources>\n')
            schema = work / "org.native.Probe.gschema.xml"
            schema.write_text('<schemalist><schema id="org.native.Probe" path="/org/native/probe/"><key name="counter" type="u"><default>5</default></key></schema></schemalist>\n')
            for name, command in (
                ("glib-compile-resources", ["--sourcedir", work, "--target", work / "native.gresource", xml]),
                ("glib-compile-schemas", ["--strict", work]),
            ):
                tool = Path(shutil.which(name)).resolve()
                run([tool, *command])
                fixture_generators[name] = {"host_executable_sha256": sha(tool), "version": run([tool, "--version"]).strip()}
            installed.update({"/usr/share/native.gresource": work / "native.gresource",
                              "/usr/share/glib-2.0/schemas/gschemas.compiled": work / "gschemas.compiled"})
            configuration = work / "resolv.conf"
            configuration.write_text("nameserver 127.0.0.1\nlookup bind\n")
            installed["/etc/resolv.conf"] = configuration
        if args.core:
            installed["/usr/lib/native-glib-module.so"] = args.module
        directories = {str(parent) for name in installed for parent in Path(name).parents if parent != Path("/")}
        for name in sorted(directories, key=lambda name: (len(Path(name).parts), name)):
            if not (original / name.lstrip("/")).is_dir():
                edit("mkdir", name)
        for name, source in installed.items():
            binary = name == "/usr/lib/dyld" or name.startswith(("/bin/", "/usr/libexec/")) or name.endswith((".dylib", ".so"))
            edit("add", source, name)
            edit("chmod", "755" if binary else "644", name)
            expected[name] = source.read_bytes()
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        types = {name: 7 if name == "/usr/lib/dyld" else 6 if name.endswith(".dylib") else 8 if name.endswith(".so") else 2
                 for name in expected if name.startswith(("/bin/", "/sbin/", "/usr/libexec/")) or name == "/usr/lib/dyld" or name.endswith((".dylib", ".so"))}
        verify_installed_files(extracted, args.arch, expected, types)
        images = {name: image_metadata(expected[name]) for name in types}
        loader = images["/usr/lib/dyld"]["exports"] | {"dyld_stub_binder"}
        bindings = {name: verify_imports(name, images, loader, flat_namespace_root="/bin/" + program)
                    for name, kind in types.items() if kind != 7}
        run(["fsck.hfsplus", "-n", image])
        args.report.write_text(json.dumps({"architecture": args.arch, "root_sha256": sha(image),
            "executable_sha256": sha(executable), "probe_sha256": sha(args.probe),
            "runtime_sha256": sha(args.runtime), "system_sha256": sha(args.system), "base_root_sha256": sha(args.base_root),
            "catalog_sha256": sha(mo), "installed_sha256": {name: hashlib.sha256(data).hexdigest()
                                                              for name, data in sorted(expected.items())},
            "import_resolutions": bindings, "runtime_checked": False,
            "fixture_generators": fixture_generators,
            "validation": "Actual native init/dash; complete byte/CPU/type inventory and real loaded binding namespace; no ELF; fsck read-only PASS."}, indent=2) + "\n")
        image.replace(args.output)
        print(f"Native dependencies {args.arch}: signed HFS consumer and catalog, actual init/dash, complete strong binding closure")


if __name__ == "__main__":
    main()
