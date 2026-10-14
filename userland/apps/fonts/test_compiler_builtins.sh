#!/usr/bin/env bash
set -euo pipefail
native_builtins_source=src/Toolchains/compiler-rt/20.1.8
for native_builtin_test in udivti3 udivmodti4; do
    clang-20 -std=gnu11 -O2 -I "$native_builtins_source/lib/builtins" \
        "$native_builtins_source/test/builtins/Unit/${native_builtin_test}_test.c" \
        "$native_builtins_source/lib/builtins/udivti3.c" \
        "$native_builtins_source/lib/builtins/udivmodti4.c" \
        -o "$TEST_TMPDIR/$native_builtin_test"
    "$TEST_TMPDIR/$native_builtin_test"
done
