#!/bin/bash
# Build Ni (debug, all checks) and run every test suite. Stops at the first
# failure, so a broken build can never be followed by a test run against an
# old ni.so. Run from anywhere:  tools/check.sh
#
# NI_PY37 overrides the CPython 3.7 interpreter (NI_PY38 the 3.8 one the
# parse gate and the 3.8 dry run read), NI_GEN_PYTHON the Python
# that runs the generators, NI_CPYTHON_SRC the source tree they read (else
# the cached release tarball; doc/BUILD.md).
set -euo pipefail
cd "$(dirname "$0")/.."
PY37=${NI_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}
PY38=${NI_PY38:-$HOME/.local/share/uv/python/cpython-3.8.20-linux-x86_64-gnu/bin/python3.8}
GEN_PY=${NI_GEN_PYTHON:-python3}
LOG=build/check-build.log
mkdir -p build

step() { printf '\n== %s\n' "$*"; }

stale_sources() {   # sources newer than ni.so (find compares to the ns)
    local so; so=$(ls ni*.so 2>/dev/null | head -1)
    [ -z "$so" ] && { echo "(no ni.so)"; return; }
    find ni setup.py $(ls -d build/temp.*/gen 2>/dev/null) -newer "$so" \( -name '*.c' -o -name '*.h' -o -name setup.py \) | head -5
}

step "parse gate: every mirrored CPython file parses, on 3.7 and 3.8 (tests/parse_gate.py)"
"$GEN_PY" tests/parse_gate.py

step "effect snippets: small C in, the effect flags out (tests/effect_snippets.py)"
"$GEN_PY" tests/effect_snippets.py

step "translator snippets: small C in, the meta or refusal out (tests/gen2_snippets.py)"
NI_PY37="$PY37" "$GEN_PY" tests/gen2_snippets.py

step "generators run under two hash seeds, with the same output and the baseline's statuses (tools/metac/generate.py)"
# the cross-checks (gen_ceval against ceval.c and dis.stack_effect) and the
# comparison with tools/metac/expected/3.7.17.txt run in each; the build
# below generates again (seed 0) and must get the same headers
rm -rf build/gen-check
for n in 1 2; do
    PYTHONHASHSEED=$n "$GEN_PY" tools/metac/generate.py --python "$PY37" --out build/gen-check/$n
done
diff -r build/gen-check/1 build/gen-check/2
echo "same output"

step "3.8 dry run against its baseline (generate.py --past-policy, tools/metac/expected/3.8.20.txt)"
NI_CPYTHON_SRC= "$GEN_PY" tools/metac/generate.py --python "$PY38" --past-policy --out build/gen-check/3.8 > build/gen-check/3.8.log \
    || { tail -40 build/gen-check/3.8.log; exit 1; }
echo "same statuses"

step "done test, point 1: what mirrors CPython and is still hand-written (tools/metac/donecheck.py)"
# a definition with no reason (exit 1), a stale report or hand_table.py
# (exit 2) fails the check; transitional reasons only (exit 3) don't: the
# phase isn't done
rc=0
"$GEN_PY" tools/metac/donecheck.py --gen build/gen-check/1 || rc=$?
[ "$rc" -eq 2 ] && { echo "stale report, or hand_table.py does not match Ni or the docs"; exit 1; }
[ "$rc" -eq 1 ] && { echo "hand-written mirrors with no reason (listed above)"; exit 1; }
[ "$rc" -eq 3 ] && echo "not done: transitional reasons remain (above)"

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
diff -r build/gen-check/1 build/temp.*/gen
echo "built from the same generated headers"

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
