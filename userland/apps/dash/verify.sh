#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_shell_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_shell_arch" \
  --root-image "bazel-bin/userland/apps/dash/shell-test-root.$native_shell_arch.img" \
  --native-shell-probe --output-dir ".cache/qemu-shell-$native_shell_arch" "$@"
