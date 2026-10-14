#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace required}"
native_cxx_arch="$1"
shift
exec env PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify_qemu_boot.py --arch "$native_cxx_arch" \
    --root-image "bazel-bin/userland/apps/cxx/runtime-root.$native_cxx_arch.img" \
    --root-report "bazel-bin/userland/apps/cxx/runtime-root.$native_cxx_arch.json" \
    --native-cxx-runtime-probe --output-dir ".cache/qemu-cxx-runtime-$native_cxx_arch" "$@"
