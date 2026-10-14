#!/usr/bin/env bash
set -euo pipefail
cd "${RUNFILES_DIR:?}/_main"
exec python3 userland/apps/x11/check_clients.py
