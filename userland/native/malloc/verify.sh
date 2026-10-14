#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_arch" \
  --root-image "bazel-bin/userland/native/malloc/malloc-test-root.$native_arch.img" \
  --native-malloc-probe --output-dir ".cache/qemu-malloc-$native_arch" "$@"
