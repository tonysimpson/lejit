#!/bin/bash
# Build and run the 3.12 entry prototype (doc/PORT312.md). Needs pyenv 3.12.9.
set -euo pipefail
cd "$(dirname "$0")"
PY=${NI_PY312:-$HOME/.pyenv/versions/3.12.9}
gcc -O2 -Wall -shared -fPIC -I"$PY/include/python3.12" \
    -I"$PY/include/python3.12/internal" proto.c \
    -o "proto$("$PY/bin/python3.12-config" --extension-suffix)"
"$PY/bin/python3.12" -I -c "import sys; sys.path.insert(0, '.'); exec(open('t.py').read())"
rm -f proto*.so
