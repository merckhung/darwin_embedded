#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
export PYTHONPATH="$PWD/scripts${PYTHONPATH:+:$PYTHONPATH}"
exec python3 -m unittest test_native_rootfs test_native_dyld test_diagnostic_macho \
  test_verify_qemu_boot test_commpage_population test_macho_to_bin \
  test_kalloc_budget test_kc_builder test_embedded_aes test_embedded_entitlements -v "$@"
