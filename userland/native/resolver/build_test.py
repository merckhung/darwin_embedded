#!/usr/bin/env python3
"""Build a real native resolver consumer or install its signed HFS guest root."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols
from build_diagnostic_userland import PROGRAM_TYPES, verify_installed_files
from sign_diagnostic_macho import sign


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "output", "report", "exports", "probe", "system", "base-root", "dyld", "hfs-tool", "clients", "client-report", "headers", "native-requests"):
        p.add_argument("--" + name, type=Path, required=name in ("root", "sdk", "output", "report"))
    p.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    a = p.parse_args()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-resolver-test-", dir=a.output.parent) as directory:
        work = Path(directory).resolve()
        sdk, system = work / "sdk", work / "system"
        with tarfile.open(a.sdk) as archive:
            archive.extractall(sdk, filter="data")
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != a.arch:
            raise ValueError("resolver test SDK architecture mismatch")
        if a.exports:
            headers = work / "headers"
            with tarfile.open(a.headers) as archive:
                archive.extractall(headers, filter="data")
            client_report = json.loads(a.client_report.read_text())
            images = [(name, record["install_path"], next(symbol[1:] for symbol in record["exports"] if symbol != "___dso_handle"))
                      for name, record in sorted(client_report["libraries"].items())]
            (work / "native_x11_images.h").write_text("static const struct {const char *name, *path, *symbol;} native_x11_images[] = {\n" +
                "".join("{" + ",".join(json.dumps(field) for field in image) + "},\n" for image in images) + "};\n")
            source = a.root.resolve() / "userland/native/resolver/test_resolver.c"
            resource = run(["clang-20", "-print-resource-dir"]).strip()
            flags = ["clang-20", f"--target={a.arch}-apple-macos26.5", "-std=gnu11", "-O2",
                     "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fPIC", "-nostdinc",
                     "-D_DARWIN_C_SOURCE", "-D_FORTIFY_SOURCE=0", "-Werror", "-Wno-availability",
                     "-isystem", resource + "/include", "-isystem", sdk / "usr/include", "-I", headers / "usr/include", "-I", work]
            flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if a.arch == "arm64" else ["-march=penryn"]
            objects, records = [], []
            for path in (source, source.with_name("test_x11.c")):
                obj = work / (path.stem + ".o")
                run([*flags, "-c", path, "-o", obj])
                magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
                if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if a.arch == "arm64" else 0x1000007, 1):
                    raise ValueError("resolver probe is not native Mach-O")
                objects.append(obj)
                records.append({"source": path.name, "source_sha256": digest(path), "object_sha256": digest(obj)})
            run(["llvm-ar-20", "--format=darwin", "rcsD", a.output, *objects])
            public, imports, _ = symbols(a.output)
            imports -= public
            imports |= set(a.native_requests.read_text().splitlines())
            a.exports.write_text("\n".join(sorted(imports)) + "\n")
            a.report.write_text(json.dumps({"architecture": a.arch, "imports": sorted(imports),
                "sources": records, "archive_sha256": digest(a.output),
                "client_metadata_sha256": digest(a.client_report)}, indent=2) + "\n")
            return
        for path in (a.clients, a.system):
            with tarfile.open(path) as archive:
                archive.extractall(system, filter="data")
        executable = work / "native-resolver-test"
        run(["/usr/lib/llvm-20/bin/ld64.lld", "-arch", a.arch, "-no_fixup_chains", "-dead_strip",
             "-platform_version", "macos", "26.5", "26.5", "-adhoc_codesign", "-pie", "-e", "_main",
             "-undefined", "dynamic_lookup",
             "-o", executable, a.probe.resolve(), "-L", system / "usr/lib/system", system / "usr/lib/libSystem.B.dylib"])
        sign(executable)
        providers = symbols(a.dyld)[0] | {"dyld_stub_binder"}
        providers |= set().union(*(symbols(path)[0] for path in (system / "usr/lib").rglob("*.dylib")))
        if missing := symbols(executable)[1] - providers:
            raise ValueError("resolver probe has unresolved native APIs: " + ", ".join(sorted(missing)))
        image = work / "root.img"
        shutil.copyfile(a.base_root, image)
        def edit(*arguments):
            run([a.hfs_tool.resolve(), image, *arguments])
        original = work / "original"
        original.mkdir()
        edit("extractall", "/", original)
        expected = {"/" + str(path.relative_to(original)): path.read_bytes()
                    for path in original.rglob("*") if path.is_file()}
        installed = {"/usr/lib/dyld": a.dyld, "/bin/native-resolver-test": executable}
        for path in sorted((system / "usr").rglob("*")):
            if path.is_file(): installed["/" + str(path.relative_to(system))] = path
        fixtures = {
            "hosts": "127.0.0.1 localhost\n192.0.2.45 native-host.example native-host-alias\n2001:db8::45 native-v6.example native-v6-alias\n",
            "services": "native-service 4242/tcp native-service-alias\nnative-service 4243/udp native-dgram-alias\n",
            "resolv.conf": "nameserver 127.0.0.1\nlookup file bind\noptions ndots:1\n",
        }
        for name, content in fixtures.items():
            path = work / name
            path.write_text(content)
            installed["/etc/" + name] = path
        for name in sorted({str(Path(name).parent) for name in installed}, key=lambda value: len(Path(value).parts)):
            if not (original / name.lstrip("/")).is_dir(): edit("mkdir", name)
        types = dict(PROGRAM_TYPES)
        for name, path in installed.items():
            edit("add", path, name)
            edit("chmod", "644" if name.startswith("/etc/") else "755", name)
            expected[name] = path.read_bytes()
            if not name.startswith("/etc/"):
                types[name] = 7 if name == "/usr/lib/dyld" else 2 if name.startswith("/bin/") else 6
        extracted = work / "verified"
        extracted.mkdir()
        edit("extractall", "/", extracted)
        verify_installed_files(extracted, a.arch, expected, types)
        run(["fsck.hfsplus", "-n", image])
        a.report.write_text(json.dumps({"architecture": a.arch, "root_sha256": digest(image),
            "executable_sha256": digest(executable), "probe_sha256": digest(a.probe),
            "system_sha256": digest(a.system), "clients_sha256": digest(a.clients), "dyld_sha256": digest(a.dyld),
            "fixture_files": fixtures, "strong_import_closure": "complete", "runtime_checked": False}, indent=2) + "\n")
        image.replace(a.output)
        print(f"Native resolver {a.arch}: signed root with real DNS client and socket server probe")


if __name__ == "__main__":
    main()
