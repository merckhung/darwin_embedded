#!/bin/dash
# All interpreters and external programs in this root are native Mach-O.
set -eu
nonce=$1
fail() { printf 'Native shell failure: %s\n' "$1" >&2; exit 1; }
[ "${#nonce}" -eq 48 ] || fail 'fresh nonce length'
case "$nonce" in *[!0-9a-f]*) fail 'fresh nonce format';; esac
[ "$(printf '\141:%s\n' "$nonce")" = "a:$nonce" ] || fail 'quoted escapes and format expansion'
set -- /etc/*passwd
[ "$#" -eq 2 ] && [ "$1" = /etc/master.passwd ] && [ "$2" = /etc/passwd ] || fail 'native directory globbing'

sum=0
for value in 1 2 3 4 5; do sum=$((sum + value)); done
[ "$sum" -eq 15 ] || fail 'arithmetic and loops'
choose() { case "$1" in native-*) printf '%s' "${1#native-}";; *) return 7;; esac; }
[ "$(choose native-darwin)" = darwin ] || fail 'function, case and substitution'
if choose other; then fail 'nonzero function status'; else [ "$?" -eq 7 ]; fi

path=/tmp/native-shell-$nonce
umask 077
printf 'native:%s\n' "$nonce" > "$path"
IFS= read -r value < "$path"
[ "$value" = "native:$nonce" ] || fail 'file redirection and read'
[ -f "$path" ] || fail 'native stat of redirected file'
piped=$(printf 'pipe:%s\n' "$nonce" | { IFS= read -r value; printf '%s' "$value"; })
[ "$piped" = "pipe:$nonce" ] || fail 'pipe and subshell'
original=$PWD
cd /etc
[ "$PWD" = /etc ] || fail 'native chdir and PWD'
cd "$original"

export NATIVE_SHELL_ENV=$nonce
executed=$(/bin/native-shell-test child "$nonce")
[ "$executed" = "exec:$nonce" ] || fail 'native fork, exec and exported environment'
unset NATIVE_SHELL_ENV
if /bin/dash -c 'exit 7'; then fail 'external nonzero exit'; else [ "$?" -eq 7 ]; fi

trap 'handled=yes' USR1
handled=no
kill -USR1 "$$"
[ "$handled" = yes ] || fail 'native signal trampoline and trap'
trap - USR1
/bin/dash -c 'exit 0' &
child=$!
wait "$child" || fail 'native background wait'

arch=$(/bin/uname -m)
version=$(/bin/uname -r)
case "$arch" in arm64|x86_64) ;; *) fail 'native kernel CPU identity';; esac
[ "$version" = 25.5.0 ] || fail 'native kernel release'
printf 'NATIVE_SHELL_OK:%s:%s:%s\n' "$nonce" "$version" "$arch"
