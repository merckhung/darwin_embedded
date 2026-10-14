#!/usr/bin/env bash
set -euo pipefail
cd "${TEST_SRCDIR}/${TEST_WORKSPACE}"
exec env PYTHONDONTWRITEBYTECODE=1 python3 userland/apps/xfce/test_ui_capture.py
