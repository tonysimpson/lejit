#!/bin/sh
# Build and run v_mem_mov_test.c against the objects of an in-place Ni build
# (NI_TRACE=1 NI_ALL_CHECKS=1 python3.7 setup.py build_ext --inplace).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PY37=${NI_PY37:-$HOME/.pyenv/versions/3.7.17/bin/python3.7}
INC=$($PY37 -c "import sysconfig;print(sysconfig.get_paths()['include'])")
LIBDIR=$($PY37 -c "import sysconfig;print(sysconfig.get_config_var('LIBDIR'))")
OBJS=$(find "$ROOT/build" -name '*.o' -path '*temp*' | grep -v '/ni/ni.o$')
OUT=${TMPDIR:-/tmp}/v_mem_mov_test.$$
gcc -w -fcommon -O0 -g -DALL_STATIC=0 -DNI_TRACE=1 -DALL_CHECKS=1 \
    -I"$INC" -I"$ROOT/ni" -I"$ROOT/ni/x64" "$HERE/v_mem_mov_test.c" \
    $OBJS $(find "$ROOT/build" -name 'ni.o' -path '*temp*') \
    -L"$LIBDIR" -Wl,-rpath,"$LIBDIR" -lpython3.7m -lffi -lm -o "$OUT"
"$OUT" "$@"
status=$?
rm -f "$OUT"
exit $status
