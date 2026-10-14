#!/usr/bin/env bash
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
userland/apps/x11/test-xorg-sha1-host
exec python3 userland/apps/x11/check_xorg_core.py
