#!/bin/bash
# Build Ni (debug, all checks) and run every test suite. Stops at the first
# failure, so a broken build can never be followed by a test run against an
# old ni.so. Run from anywhere:  tools/check.sh
#
# NI_PY37 overrides the CPython 3.7 interpreter.
set -euo pipefail
cd "$(dirname "$0")/.."
PY37=${NI_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}
LOG=build/check-build.log
mkdir -p build

step() { printf '\n== %s\n' "$*"; }

step "build (NI_TRACE=1 NI_ALL_CHECKS=1)"
if ! NI_TRACE=1 NI_ALL_CHECKS=1 "$PY37" setup.py build_ext --inplace > "$LOG" 2>&1; then
    grep -E ": error|Error" "$LOG" | head -20
    echo "BUILD FAILED (full log: $LOG)"
    exit 1
fi
# belt and braces: the module must be newer than every source file
SO=$(ls ni*.so)
STALE=$(find ni setup.py -newer "$SO" \( -name '*.c' -o -name '*.h' -o -name setup.py \) | head -5)
if [ -n "$STALE" ]; then
    echo "STALE $SO: older than"; echo "$STALE"
    exit 1
fi
echo "ok ($SO)"

step "behaviour tests (tests/run.py)"
"$PY37" tests/run.py

step "encoder tests (tests/encoding/check.py)"
python3 tests/encoding/check.py

step "v_mem_mov execution tests"
NI_PY37="$PY37" bash tests/encoding/run_v_mem_mov.sh

step "stdlib smoke test (tests/smoke.py, with and without Ni)"
diff <("$PY37" tests/smoke.py plain 2>&1) <("$PY37" tests/smoke.py ni 2>&1)
echo "same output"

printf '\nALL CHECKS PASSED\n'
