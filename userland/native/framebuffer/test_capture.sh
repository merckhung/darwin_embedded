#!/usr/bin/env bash
set -euo pipefail
cd "${TEST_SRCDIR:?}/${TEST_WORKSPACE:?}/userland/native/framebuffer"
exec python3 test_capture.py
