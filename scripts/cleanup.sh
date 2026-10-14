#!/bin/bash
# Clean Bazel outputs. Runtime evidence in .cache/qemu is retained.
set -euo pipefail
cd "$(dirname "$0")/.."
exec bazel clean "$@"
