#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_resolver_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_resolver_arch" \
    --root-image "bazel-bin/userland/native/resolver/resolver-test-root.$native_resolver_arch.img" \
    --native-resolver-probe --output-dir ".cache/qemu-resolver-$native_resolver_arch" "$@"
