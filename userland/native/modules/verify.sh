#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_modules_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_modules_arch" \
  --root-image "bazel-bin/userland/native/modules/modules-test-root.$native_modules_arch.img" \
  --native-modules-probe --output-dir ".cache/qemu-modules-$native_modules_arch" "$@"
