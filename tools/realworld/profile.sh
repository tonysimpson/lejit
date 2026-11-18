#!/bin/bash
# profile.sh WORKLOAD PYTHON
# Runs a workload under perf (user-space cycles, DWARF call graphs, 199 Hz) and prints how its
# samples split between bytecode, GC, C library work and compiled code (split.py). Servers are
# profiled under 15 s of wrk load, the server process tree only.
set -u
W=$1; PY=$2
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/common.sh"
mkdir -p "$RW/work/$W"
DATA=$RW/work/$W/perf.data
export RW_PREFIX="perf record -q -e cycles:u -F 199 --call-graph dwarf,65528 --user-callchains -o $DATA --"
case " $SERVERS " in *" $W "*) export RW_LOAD=${RW_LOAD:-15} RW_REPEAT=${RW_REPEAT:-20} ;; esac
TIMEFORMAT="$W: %R s real, %U s user, %S s sys"
time "$HERE/run.sh" "$W" "$PY"
perf script --no-inline -F comm,ip,sym,dso -i "$DATA" 2>/dev/null | "$PY" -I "$HERE/split.py" | sed "s/^/$W: /"
rm -f "$DATA"
