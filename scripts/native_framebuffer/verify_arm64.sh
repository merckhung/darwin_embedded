#!/usr/bin/env bash
set -euo pipefail
workspace="${BUILD_WORKSPACE_DIRECTORY:-$(pwd)}"
exec python3 "$workspace/scripts/native_framebuffer/verify_framebuffer.py" "$@"
