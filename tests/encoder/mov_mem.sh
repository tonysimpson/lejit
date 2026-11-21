#!/bin/sh
# Build and run mov_mem.c against the objects of an in-place Ni build
# (LE_TRACE=1 LE_ALL_CHECKS=1 python3.7 setup.py build_ext --inplace) for the
# interpreter LE_PYTHON (make's PY; else LE_PY37, else pyenv's 3.7.17).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PY=${LE_PYTHON:-${LE_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}}
INC=$($PY -c "import sysconfig;print(sysconfig.get_paths()['include'])")
LIBDIR=$($PY -c "import sysconfig;print(sysconfig.get_config_var('LIBDIR'))")
LDVERSION=$($PY -c "import sysconfig;print(sysconfig.get_config_var('LDVERSION'))")
TEMP=$($PY "$ROOT/tests/common.py")
OBJS=$(find "$TEMP" -name '*.o' | grep -v '/src/lejit.o$')
OUT=${TMPDIR:-/tmp}/mov_mem.$$
gcc -w -O0 -g -DNI_TRACE=1 -DALL_CHECKS=1 \
    -I"$INC" -I"$ROOT/src" -I"$ROOT/src/x64" -I"$TEMP/gen" "$HERE/mov_mem.c" \
    $OBJS $(find "$TEMP" -name 'lejit.o') \
    -L"$LIBDIR" -Wl,-rpath,"$LIBDIR" -lpython"$LDVERSION" -lffi -lunwind -lm -o "$OUT"
"$OUT" "$@"
status=$?
rm -f "$OUT"
exit $status
