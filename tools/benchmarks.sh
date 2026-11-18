#!/bin/bash
# Build Ni for release and run the benchmark suites, printing the tables and
# keeping a copy in build/bench/<date>-<commit>.txt. Run from anywhere:
#
#   tools/benchmarks.sh                # bench, pyperf and compile
#   tools/benchmarks.sh pyperf         # one suite: bench, pyperf, compile
#                                      # or adversarial
#   tools/benchmarks.sh pyperf bench   # several
#   tools/benchmarks.sh -k richards    # only names containing 'richards'
#   tools/benchmarks.sh adversarial --long   # adversarial with its long runs
#
# Suites: bench   tools/bench.py        small kernels, Ni against CPython
#         pyperf  tools/pyperf37.py     pyperformance, Ni against CPython
#         compile tools/compilebench.py compile time, code size, break-even
#         adversarial tools/adversarial/run.py  programs aimed at Ni's
#                 weaknesses: every call timed in 5 processes per mode at
#                 two run lengths (--long adds a third), outputs compared,
#                 losses first; also writes <saved name>-adversarial.txt,
#                 one line per benchmark and measure (not run unless named)
#
# Leaves a release build behind; tools/check.sh rebuilds for debugging.
# NI_PY37 overrides the CPython 3.7 interpreter.
set -euo pipefail
cd "$(dirname "$0")/.."
PY37=${NI_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}
unset NI_TRACE NI_ALL_CHECKS

suites=()
filter=()
adv=()
while [ $# -gt 0 ]; do
    case "$1" in
        bench|pyperf|compile|adversarial) suites+=("$1") ;;
        -k) filter=(-k "$2"); shift ;;
        --long) adv+=(--long) ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (try --help)"; exit 2 ;;
    esac
    shift
done
[ ${#suites[@]} -eq 0 ] && suites=(bench pyperf compile)

# release build; distutils compares whole seconds, so remove a module that
# is older than any source ourselves (as tools/check.sh does)
so=$(ls ni*.so 2>/dev/null | head -1 || true)
if [ -n "$so" ] && [ -n "$(find ni setup.py -newer "$so" \( -name '*.c' -o -name '*.h' -o -name setup.py \) | head -1)" ]; then
    rm -f ni*.so
fi
mkdir -p build/bench
if ! "$PY37" setup.py build_ext --inplace > build/bench/build.log 2>&1; then
    grep -E ": error|Error" build/bench/build.log | head -20
    echo "BUILD FAILED (full log: build/bench/build.log)"
    exit 1
fi

commit=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain --untracked-files=no | head -1)
out=build/bench/$(date +%Y-%m-%d-%H%M)-$commit${dirty:+-dirty}.txt

{
    echo "Ni benchmarks: $(date '+%Y-%m-%d %H:%M'), commit $commit${dirty:+ (uncommitted changes)}"
    echo "release build, $("$PY37" --version 2>&1), $(nproc) CPUs"
    for s in "${suites[@]}"; do
        echo
        case $s in
            bench)   echo "== small kernels (tools/bench.py)"
                     "$PY37" tools/bench.py "${filter[@]}" || echo "(failed)" ;;
            pyperf)  echo "== pyperformance (tools/pyperf37.py)"
                     "$PY37" tools/pyperf37.py "${filter[@]}" || echo "(failed)" ;;
            compile) echo "== compile cost (tools/compilebench.py)"
                     "$PY37" tools/compilebench.py "${filter[@]}" || echo "(failed)" ;;
            adversarial) echo "== adversarial (tools/adversarial/run.py)"
                     "$PY37" tools/adversarial/run.py "${filter[@]}" "${adv[@]}" \
                         --data "${out%.txt}-adversarial.txt" || echo "(failed)" ;;
        esac
    done
} 2>&1 | tee "$out"
echo
echo "saved: $out"
