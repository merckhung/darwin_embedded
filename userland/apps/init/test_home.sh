#!/bin/dash
set -eu
/bin/native-home-test "$1"
exec /bin/dash /etc/native-shell-checks.sh "$1"
