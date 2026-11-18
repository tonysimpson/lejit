#!/bin/bash
# Build Ni with AddressSanitizer and UBSan in build/asan (the in-place
# build is left alone) and run the behaviour tests, the compile bounds,
# CPython's own tests and the smoke test under it, then list the reports,
# deduplicated by stack (tools/sanitize_report.py). doc/BUILD.md.
#
#   tools/sanitize.sh [run] [bounds] [cpython | cpython-all] [smoke]   (default: all but cpython-all)
#
# NI_SANITIZE picks the sanitizers (default address,undefined);
# NI_SANITIZE_LEAKS=1 turns LeakSanitizer on (only leaks allocated from Ni
# are listed); NI_ASAN_OPTIONS adds ASan options. CPython isn't
# instrumented: libasan is preloaded, and PYTHONMALLOC=malloc sends its objects through ASan's allocator. Without an
# address-space limit (ASan reserves terabytes), resident memory is capped
# by a systemd scope (NI_SANITIZE_MEM, default 6G) where there is one.
set -uo pipefail
cd "$(dirname "$0")/.."
PY37=${NI_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}
SAN=${NI_SANITIZE:-address,undefined}
MEM=${NI_SANITIZE_MEM:-6G}
OUT=build/asan
STEPS=${*:-run bounds cpython smoke}

echo "== build ($SAN, NI_TRACE=1 NI_ALL_CHECKS=1) into $OUT"
mkdir -p "$OUT"
so=$(ls "$OUT"/ni*.so 2>/dev/null | head -1)
if [ -n "$so" ] && [ -n "$(find ni setup.py -newer "$so" \( -name '*.c' -o -name '*.h' -o -name setup.py \) | head -1)" ]; then
    rm -f "$OUT"/ni*.so    # distutils compares whole seconds
fi
if ! NI_SANITIZE="$SAN" NI_TRACE=1 NI_ALL_CHECKS=1 "$PY37" setup.py build_ext \
        --build-lib "$OUT" --build-temp "$OUT/temp" > "$OUT/build.log" 2>&1; then
    grep -E ": error|Error" "$OUT/build.log" | head -20
    echo "BUILD FAILED (full log: $OUT/build.log)"
    exit 1
fi
echo "ok ($(ls "$OUT"/ni*.so))"

ulimit -v unlimited 2>/dev/null || { echo "an address-space limit is set and can't be lifted: ASan needs none"; exit 1; }
LOGS=$OUT/logs/$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$LOGS"
export NI_MODULE_DIR=$PWD/$OUT NI_SANITIZE_LOGS=$PWD/$LOGS PYTHONMALLOC=malloc
preload=$(gcc -print-file-name=libasan.so)
case $SAN in *undefined*) preload+=" $(gcc -print-file-name=libubsan.so)";; esac
export LD_PRELOAD=$preload
# tests that ask for huge allocations expect MemoryError, not an abort
ASAN_BASE="allocator_may_return_null=1:detect_odr_violation=0"
if [ "${NI_SANITIZE_LEAKS:-0}" = 1 ]; then
    # a leak is listed, not a failure of the test that had it. The exit
    # code is shared by every report, so an abort is left to the signal
    # (CPython's tests check a fatal error's status) and an ASan error
    # exits 0: only the reports count in this mode
    ASAN_BASE+=":detect_leaks=1:handle_abort=0"
    export LSAN_OPTIONS=exitcode=0
else
    # handle_abort: the stack of a psyco_fatal_error
    ASAN_BASE+=":detect_leaks=0:handle_abort=1"
fi
ASAN_BASE+=${NI_ASAN_OPTIONS:+:$NI_ASAN_OPTIONS}    # more, e.g. detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1

if systemd-run --user --scope -q -p MemoryMax=1G true 2>/dev/null; then
    GUARD=(systemd-run --user --scope -q -p "MemoryMax=$MEM" -p MemorySwapMax=0 --)
else
    GUARD=()
    echo "(no systemd user scope: resident memory is not limited; watch ps)"
fi

FAILED=
step() {    # step NAME COMMAND...: the step's own reports go to NAME.*
    local name=$1 err; shift
    err=$LOGS/$name.ubsan.$$    # libubsan ignores log_path: keep stderr
    printf '\n== %s\n' "$name"
    ASAN_OPTIONS="$ASAN_BASE:log_path=$PWD/$LOGS/$name.asan" \
        timeout 4h "${GUARD[@]}" "$@" 2> >(tee "$err" >&2) || FAILED+=" $name"
    wait    # for tee
    grep -q 'runtime error:' "$err" || rm -f -- "$err"
}
export ASAN_OPTIONS=$ASAN_BASE
for s in $STEPS; do
    case $s in
    run) step behaviour "$PY37" tests/run.py ;;
    bounds) step bounds "$PY37" tests/compile_bounds.py ;;
    cpython) step cpython "$PY37" tests/cpython_tests.py -j 2 ;;
    cpython-all) step cpython "$PY37" tests/cpython_tests.py -j 2 --all ;;
    smoke) step smoke bash -c 'diff <("$0" tests/smoke.py plain) <("$0" tests/smoke.py ni) && echo "same output"' "$PY37" ;;
    *) echo "unknown step $s (run, bounds, cpython, cpython-all, smoke)"; exit 2 ;;
    esac
done

printf '\n== reports (%s)\n' "$LOGS"
env -u LD_PRELOAD python3 tools/sanitize_report.py "$LOGS"
rc=$?
[ -n "$FAILED" ] && echo "steps that failed:$FAILED"
[ $rc -eq 0 ] && [ -z "$FAILED" ]
