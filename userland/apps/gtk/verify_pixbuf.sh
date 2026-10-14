#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_pixbuf_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_pixbuf_arch" \
    --root-image "bazel-bin/userland/apps/gtk/pixbuf-test-root.$native_pixbuf_arch.img" \
    --native-pixbuf-probe --native-init-probe --output-dir ".cache/qemu-pixbuf-$native_pixbuf_arch" "$@"
