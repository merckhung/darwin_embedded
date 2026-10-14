#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_font_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_font_arch" \
    --root-image "bazel-bin/userland/apps/fonts/test-root.$native_font_arch.img" \
    --native-fonts-probe --native-init-probe --output-dir ".cache/qemu-fonts-$native_font_arch" "$@"
