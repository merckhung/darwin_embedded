#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_tls_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_tls_arch" \
    --root-image "bazel-bin/userland/native/tls/tls-test-root.$native_tls_arch.img" \
    --native-tls-probe --output-dir ".cache/qemu-tls-$native_tls_arch" "$@"
