#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_pango_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_pango_arch" \
    --root-image "bazel-bin/userland/apps/fonts/pango-test-root.$native_pango_arch.img" \
    --native-pango-probe --native-init-probe --output-dir ".cache/qemu-pango-$native_pango_arch" "$@"
