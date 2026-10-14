#!/usr/bin/env bash
set -euo pipefail
cd "${RUNFILES_DIR:?}/_main"
exec python3 userland/native/system/test_packaging.py
