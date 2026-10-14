#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_init_arch="$1"
native_init_kind="$2"
shift 2
native_init_checks=(--native-init-probe --native-tools-probe)
if [[ "$native_init_kind" == "test-root" ]]; then
    native_init_checks+=(--native-shell-probe)
fi
exec python3 scripts/verify_qemu_boot.py --arch "$native_init_arch" \
    --root-image "bazel-bin/userland/apps/init/$native_init_kind.$native_init_arch.img" \
    "${native_init_checks[@]}" --output-dir ".cache/qemu-init-$native_init_kind-$native_init_arch" "$@"
