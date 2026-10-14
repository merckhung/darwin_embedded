#!/usr/bin/env bash
set -euo pipefail
cd "${TEST_SRCDIR}/${TEST_WORKSPACE}"
exec env PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=userland/apps/xfce:userland/apps/x11:scripts python3 userland/apps/xfce/check_jpeg.py
