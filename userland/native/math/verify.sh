#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_math_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_math_arch" \
  --root-image "bazel-bin/userland/native/math/math-test-root.$native_math_arch.img" \
  --native-math-probe --output-dir ".cache/qemu-math-$native_math_arch" "$@"
