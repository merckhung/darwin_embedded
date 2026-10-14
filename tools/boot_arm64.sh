#!/bin/bash
# Verify current XNU and native ARM64 dash; --interactive keeps the console.
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:-.}"
exec python3 scripts/verify_qemu_boot.py --arch arm64 \
  --root-image bazel-bin/userland/apps/init/root.arm64.img --timeout 180 "$@"
