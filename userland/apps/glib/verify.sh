#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_dependency_arch="$1"
shift
if [[ "${1:-}" == "gio" ]]; then
    shift
    exec python3 scripts/verify_qemu_boot.py --arch "$native_dependency_arch" \
        --root-image "bazel-bin/userland/apps/glib/gio-test-root.$native_dependency_arch.img" \
        --native-gio-probe --native-init-probe --output-dir ".cache/qemu-gio-$native_dependency_arch" "$@"
fi
if [[ "${1:-}" == "core" ]]; then
    shift
    exec python3 scripts/verify_qemu_boot.py --arch "$native_dependency_arch" \
        --root-image "bazel-bin/userland/apps/glib/core-test-root.$native_dependency_arch.img" \
        --native-glib-probe --native-init-probe --output-dir ".cache/qemu-glib-$native_dependency_arch" "$@"
fi
exec python3 scripts/verify_qemu_boot.py --arch "$native_dependency_arch" \
    --root-image "bazel-bin/userland/apps/glib/test-root.$native_dependency_arch.img" \
    --native-dependencies-probe --native-init-probe --output-dir ".cache/qemu-dependencies-$native_dependency_arch" "$@"
