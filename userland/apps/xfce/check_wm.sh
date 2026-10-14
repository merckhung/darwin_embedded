#!/usr/bin/env bash
set -euo pipefail
cd "${TEST_SRCDIR}/${TEST_WORKSPACE}"
exec env PYTHONDONTWRITEBYTECODE=1 python3 userland/apps/xfce/check_wm.py
