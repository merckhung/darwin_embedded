#!/usr/bin/env bash
set -euo pipefail
cd "${TEST_SRCDIR}/${TEST_WORKSPACE}"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH="${PWD}/userland/native/framebuffer${PYTHONPATH:+:${PYTHONPATH}}"
exec python3 userland/apps/x11/test_xorg_capture.py
