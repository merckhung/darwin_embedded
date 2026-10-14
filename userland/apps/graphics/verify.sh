#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_graphics_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_graphics_arch" \
    --root-image "bazel-bin/userland/apps/graphics/test-root.$native_graphics_arch.img" \
    --native-graphics-probe --native-init-probe --output-dir ".cache/qemu-graphics-$native_graphics_arch" "$@"
