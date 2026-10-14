#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace required}"
native_pty_arch="$1"
shift
exec env PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify_qemu_boot.py --arch "$native_pty_arch" \
    --root-image "bazel-bin/userland/native/pty/pty-root.$native_pty_arch.img" \
    --root-report "bazel-bin/userland/native/pty/pty-root.$native_pty_arch.json" \
    --native-pty-probe --output-dir ".cache/qemu-pty-$native_pty_arch" "$@"
