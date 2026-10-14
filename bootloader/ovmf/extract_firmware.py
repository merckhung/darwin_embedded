#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Extract pinned publisher firmware and verify its bytes and copyright notice."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

def sha(data):
    return hashlib.sha256(data).hexdigest()

def members(data):
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Expected a Debian ar package")
    offset = 8
    while offset < len(data):
        header = data[offset:offset+60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise ValueError("Invalid ar member header")
        size = int(header[48:58])
        start = offset + 60
        end = start + size
        if end > len(data):
            raise ValueError("Truncated ar member")
        yield header[:16].decode().strip().rstrip("/"), data[start:end]
        offset = end + (size & 1)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("package", "manifest", "copyright", "code", "variables"):
        p.add_argument("--"+name, type=Path, required=True)
    a = p.parse_args()
    m = json.loads(a.manifest.read_text())
    data = a.package.read_bytes()
    if sha(data) != m["package_sha256"]:
        raise ValueError("Firmware package checksum mismatch")
    archives = dict(members(data))
    if archives.get("debian-binary") != b"2.0\n":
        raise ValueError("Unsupported package format")
    raw = subprocess.run(["zstd", "--decompress", "--quiet", "--stdout"],
                         input=archives["data.tar.zst"], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        for role, output in (("code", a.code), ("variables", a.variables), ("copyright", None)):
            record = m["files"][role]
            payload = archive.extractfile("./"+record["member"]).read()
            if sha(payload) != record["sha256"]:
                raise ValueError("Firmware member checksum mismatch: "+role)
            if role == "copyright":
                if payload != a.copyright.read_bytes():
                    raise ValueError("Firmware copyright notice must match the publisher package")
            else:
                output.write_bytes(payload)

if __name__ == "__main__":
    main()
