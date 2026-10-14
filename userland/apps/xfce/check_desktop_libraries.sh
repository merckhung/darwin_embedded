#!/bin/sh
set -eu
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH="userland/apps/xfce:userland/apps/x11:scripts${PYTHONPATH:+:$PYTHONPATH}"
exec python3 userland/apps/xfce/check_desktop_libraries.py
