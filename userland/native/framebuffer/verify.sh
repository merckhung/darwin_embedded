#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
exec python3 userland/native/framebuffer/verify.py --arch "$@"
