#!/usr/bin/env bash
set -euo pipefail
cd "${BUILD_WORKSPACE_DIRECTORY:?Bazel workspace is required}"
native_check_arch="$1"
native_check_kind="$2"
shift 2
case "$native_check_arch" in arm64|x86_64) ;; *) exit 2 ;; esac
case "$native_check_kind" in
  dyld)
    exec python3 scripts/verify_qemu_boot.py --arch "$native_check_arch" \
      --root-image "bazel-bin/userland/native/dyld-test-root.$native_check_arch.img" \
      --native-dyld-probe "$@" ;;
  runtime)
    exec python3 scripts/verify_qemu_boot.py --arch "$native_check_arch" \
      --root-image "bazel-bin/userland/native/runtime-test-root.$native_check_arch.img" \
      --native-runtime-probe "$@" ;;
  signature)
    exec python3 scripts/verify_diagnostic_code_signing.py --arch "$native_check_arch" \
      --root-image "bazel-bin/userland/native/dyld-test-root.$native_check_arch.img" \
      --native-dylib "$@" ;;
  minimal-signature)
    exec python3 scripts/verify_diagnostic_code_signing.py --arch "$native_check_arch" \
      --root-image "bazel-bin/userland/diagnostic/root.$native_check_arch.img" "$@" ;;
  *) exit 2 ;;
esac
