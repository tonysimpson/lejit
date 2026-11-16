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

stale_sources() {   # sources newer than ni.so (find compares to the ns)
    local so; so=$(ls ni*.so 2>/dev/null | head -1)
    [ -z "$so" ] && { echo "(no ni.so)"; return; }
    find ni setup.py -newer "$so" \( -name '*.c' -o -name '*.h' -o -name setup.py \) | head -5
}

step "build (NI_TRACE=1 NI_ALL_CHECKS=1)"
# distutils compares whole seconds: an edit in the same second as the last
# build looks up to date to it, so remove the module ourselves
[ -n "$(stale_sources)" ] && rm -f ni*.so
if ! NI_TRACE=1 NI_ALL_CHECKS=1 "$PY37" setup.py build_ext --inplace > "$LOG" 2>&1; then
    grep -E ": error|Error" "$LOG" | head -20
    echo "BUILD FAILED (full log: $LOG)"
    exit 1
fi
STALE=$(stale_sources)
if [ -n "$STALE" ]; then
    echo "STALE ni.so: older than"; echo "$STALE"
    exit 1
fi
echo "ok ($(ls ni*.so))"

step "behaviour tests (tests/run.py)"
"$PY37" tests/run.py

step "compile bounds (tests/compile_bounds.py)"
"$PY37" tests/compile_bounds.py

step "CPython's own tests, with and without Ni (tests/cpython_tests.py)"
"$PY37" tests/cpython_tests.py

step "encoder tests (tests/encoding/check.py)"
python3 tests/encoding/check.py

step "v_mem_mov execution tests"
NI_PY37="$PY37" bash tests/encoding/run_v_mem_mov.sh

step "stdlib smoke test (tests/smoke.py, with and without Ni)"
diff <("$PY37" tests/smoke.py plain 2>&1) <("$PY37" tests/smoke.py ni 2>&1)
echo "same output"

printf '\nALL CHECKS PASSED\n'
