#!/usr/bin/env bash
set -euo pipefail
cd "${TEST_SRCDIR}/${TEST_WORKSPACE}"
exec env PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=userland/apps/cxx:userland/apps/xfce:userland/apps/x11:scripts python3 userland/apps/cxx/check_libraries.py
