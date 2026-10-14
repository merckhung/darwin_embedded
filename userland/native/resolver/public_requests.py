#!/usr/bin/env python3
"""Separate the real fdopen helper's Libc-private dependency from X11 public APIs."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run
p = argparse.ArgumentParser(description=__doc__)
for name in ("input", "libc", "output", "report"):
    p.add_argument("--" + name, type=Path, required=True)
a = p.parse_args()
requests = set(a.input.read_text().splitlines())
private = "_libc_hooks"
listing = run(["llvm-nm-20", "-m", "-g", a.libc])
if private not in requests or not any("private external " + private in line for line in listing.splitlines()):
    raise ValueError("fdopen dependency is not the genuine private Libc hooks object")
requests.remove(private)
a.output.write_text("\n".join(sorted(requests)) + "\n")
a.report.write_text(json.dumps({"private_component_dependencies": {private: "libc"},
    "reason": "Actual fdopen source is appended to Libc and resolves this symbol internally; no private API is exported.",
    "original_requests_sha256": hashlib.sha256(a.input.read_bytes()).hexdigest(),
    "libc_archive_sha256": hashlib.sha256(a.libc.read_bytes()).hexdigest()}, indent=2) + "\n")
