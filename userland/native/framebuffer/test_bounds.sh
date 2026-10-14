#!/usr/bin/env bash
set -euo pipefail
exec "${TEST_SRCDIR:?}/${TEST_WORKSPACE:?}/userland/native/framebuffer/test-bounds"
