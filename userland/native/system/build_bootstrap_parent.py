#!/usr/bin/env python3
"""Build Apple's actual bootstrap-parent wrapper and source-generated Mach RPC."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "mach-headers", "migcom", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); args.output.parent.mkdir(parents=True, exist_ok=True)
    source = root / "src/Libraries/25.5.0/launchd"
    with tempfile.TemporaryDirectory(prefix=".native-bootstrap-parent-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); sdk = work / "sdk"
        for path in (args.sdk, args.mach_headers):
            with tarfile.open(path) as archive:
                archive.extractall(sdk, filter="data")
        original = source / "src/job.defs"; text = original.read_text(); index = []
        def select(match):
            index.append(match[1])
            return match[0] if match[1] == "parent" else "skip;"
        text = re.sub(r"\broutine\s+(\w+)\s*\(.*?\);", select, text, flags=re.S)
        if index.count("parent") != 1 or index.index("parent") != 6:
            raise ValueError("original launchd parent message ID changed")
        text = re.sub(r'^import "vproc(?:_priv|_internal)?\.h";\n', "", text, flags=re.M)
        text = text.replace('userprefix vproc_mig_;', 'uimport "native_bootstrap_types.h";\nuserprefix vproc_mig_;')
        definitions = work / "parent.defs"; definitions.write_text(text)
        declaration = re.search(r"^typedef mach_port_t vproc_mig_t;$", (source / "liblaunch/vproc_internal.h").read_text(), re.M)
        if not declaration:
            raise ValueError("original launchd client port type missing")
        (work / "native_bootstrap_types.h").write_text('#include <mach/mach.h>\n' + declaration[0] + '\n')
        client, header = work / "parentUser.c", work / "job.h"
        environment = dict(os.environ, MIGCC="clang-20", MIGCOM=str(args.migcom.resolve()))
        command = ["bash", root / "tools/mig/mig.sh", "-novouchers", "-target", f"{args.arch}-apple-macos26.5", "-arch", args.arch,
             "-nostdinc", "-I" + str(sdk / "usr/include"), "-I" + str(source / "src"), "-I" + str(work),
             "-header", header, "-user", client, "-server", "/dev/null", definitions]
        result = subprocess.run(list(map(str, command)), env=environment, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        original_wrapper = source / "liblaunch/libbootstrap.c"
        wrapper = re.search(r"kern_return_t\nbootstrap_parent\(.*?\n\}", original_wrapper.read_text(), re.S)
        if not wrapper or wrapper[0].count("vproc_mig_parent(bp, parent_port)") != 1:
            raise ValueError("original bootstrap_parent implementation changed")
        isolated = work / "bootstrap_parent.c"
        license = original_wrapper.read_text().split('#include "config.h"')[0]
        isolated.write_text(license + '#include <servers/bootstrap.h>\n#include "job.h"\n' + wrapper[0] + '\n')
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-O2", "-fPIC", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-Werror=implicit-function-declaration",
                 "-Werror=incompatible-pointer-types", "-Wno-availability", "-I", work,
                 "-isystem", sdk / "usr/include", "-isystem", sdk / "usr/local/include", "-isystem", resource + "/include"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        objects = []
        for path in (client, isolated):
            obj = path.with_suffix(".o"); run([*flags, "-c", path, "-o", obj])
            if struct.unpack_from("<4I", obj.read_bytes())[1::2] != (0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("foreign bootstrap RPC object")
            objects.append(obj)
        run(["llvm-ar-20", "--format=darwin", "rcsD", args.output, *objects])
        exports, imports, weak = symbols(args.output)
        if exports != {"_bootstrap_parent", "_vproc_mig_parent"}:
            raise ValueError("unexpected native bootstrap RPC exports: " + repr(exports))
        args.report.write_text(json.dumps({"architecture": args.arch, "archive_sha256": sha(args.output), "exports": sorted(exports),
            "imports": sorted(imports - exports), "message_id": 406, "original_sources": {str(path.relative_to(root)): sha(path) for path in
                (original, source / "src/job_types.defs", source / "liblaunch/vproc_internal.h", original_wrapper)},
            "generated_client_sha256": sha(client), "isolated_wrapper_sha256": sha(isolated), "runtime_checked": False,
            "scope": "Exact Apple parent RPC and wrapper; other original routine slots retained as MIG skips. No bootstrap server or fake success."}, indent=2) + "\n")
        print(f"Native bootstrap_parent {args.arch}: original Apple wrapper and MIG-generated Mach RPC 406")

if __name__ == "__main__":
    main()
