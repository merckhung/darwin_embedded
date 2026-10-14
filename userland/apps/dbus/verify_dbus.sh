#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_dbus_arch="$1"
shift
exec python3 scripts/verify_qemu_boot.py --arch "$native_dbus_arch" \
    --root-image "bazel-bin/userland/apps/dbus/test-root.$native_dbus_arch.img" \
    --native-dbus-probe --native-init-probe --output-dir ".cache/qemu-dbus-$native_dbus_arch" "$@"
