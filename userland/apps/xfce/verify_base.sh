#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace required}"
xfce_arch="$1"
shift
exec env PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify_qemu_boot.py --arch "$xfce_arch" \
    --root-image "bazel-bin/userland/apps/xfce/xfce-base-root.$xfce_arch.img" \
    --root-report "bazel-bin/userland/apps/xfce/xfce-base-root.$xfce_arch.json" \
    --native-xfce-base-probe --native-init-probe --output-dir ".cache/qemu-xfce-base-$xfce_arch" "$@"
