#!/usr/bin/env bash
# Run a command with some uncommitted changes taken out, to show that a new
# test fails without the fix (D21). The build is forced: distutils compares
# whole seconds, so stashing just after a build can leave the fixed ni.so in
# place and make the check pass by mistake (it happened twice).
#
#   tools/without_fix.sh <file>... -- <command>
#   e.g. tools/without_fix.sh ni/mergepoints.c -- python3.7 tests/run.py -k framelocals
#
# The changes are restored and ni.so is removed afterwards, whatever happens.
# Exit status: that of the command (non-zero is what you want to see).
set -euo pipefail
cd "$(dirname "$0")/.."
PY37=${NI_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}

files=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do files+=("$1"); shift; done
[ $# -gt 1 ] && [ ${#files[@]} -gt 0 ] || { sed -n 2,11p "$0"; exit 2; }
shift

if git diff --quiet HEAD -- "${files[@]}"; then
    echo "without_fix: no uncommitted changes in ${files[*]}" >&2
    exit 2
fi
git stash push -q -- "${files[@]}"
trap 'git stash pop -q; rm -f ni*.so' EXIT

rm -f ni*.so
NI_TRACE=1 NI_ALL_CHECKS=1 "$PY37" setup.py build_ext --inplace > build/without-fix-build.log 2>&1 ||
    { echo "without_fix: build failed, see build/without-fix-build.log" >&2; exit 2; }
echo "-- without changes to ${files[*]}:"
set +e
"$@"
status=$?
set -e
exit $status
