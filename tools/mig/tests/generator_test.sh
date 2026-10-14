#!/bin/bash
set -euo pipefail

MIG_TEST_ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}/tools/mig"
export MIGCOM="${MIG_TEST_ROOT}/migcom"
export MIGCC
MIGCC="$(command -v clang-20 || command -v clang)"

cd "${TEST_TMPDIR}"
bash "${MIG_TEST_ROOT}/mig.sh" -mach_msg2 -arch x86_64 -target x86_64-apple-macos26.5 \
    -header modern.h -user modernUser.c -server modernServer.c \
    "${MIG_TEST_ROOT}/tests/smoke.defs"
[[ "$(<modernServer.c)" == *mig_kern_routine_t* ]]
[[ "$(<modern.h)" == *smoke* ]]

bash "${MIG_TEST_ROOT}/mig.sh" -arch x86_64 -target x86_64-apple-macos26.5 \
    -header classic.h -user classicUser.c -server classicServer.c \
    "${MIG_TEST_ROOT}/tests/smoke.defs"
[[ "$(<classicServer.c)" != *mig_kern_routine_t* ]]
[[ "$(<classicUser.c)" == *smoke* ]]

# A failed preprocessor must not be hidden by a successful empty migcom input.
cat > failing-cc <<'EOF'
#!/bin/bash
exit 1
EOF
chmod +x failing-cc
if MIGCC="${TEST_TMPDIR}/failing-cc" bash "${MIG_TEST_ROOT}/mig.sh" \
    -header fail.h -user failUser.c -server failServer.c \
    "${MIG_TEST_ROOT}/tests/smoke.defs"; then
    echo "mig accepted failed preprocessing" >&2
    exit 1
fi
