#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace required}"
exec env PYTHONDONTWRITEBYTECODE=1 python3 userland/apps/xfce/verify_ui.py --arch "$@"
