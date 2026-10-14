#!/bin/bash
# Verify current XNU and native x86_64 dash; --interactive keeps the console.
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:-.}"
exec python3 scripts/verify_qemu_boot.py --arch x86_64 --timeout 180 "$@"
