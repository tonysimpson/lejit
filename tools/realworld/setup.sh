#!/bin/bash
# setup.sh PYTHON [WORKLOAD...]
# Creates one venv per environment under build/realworld/envs/<tag>/ from the hashed locks
# in reqs/, then fetches the inputs and models. The only step that needs the network.
set -eu
PY=$1; shift
source "$(dirname "$0")/common.sh"
unset PIP_NO_INDEX HF_HUB_OFFLINE TRANSFORMERS_OFFLINE
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 12))' ||
  { echo "the pinned releases need Python 3.12 or later" >&2; exit 1; }
WANT=${*:-$WORKLOADS}
declare -A DONE=()
for w in $WANT; do
  e=${ENV_OF[$w]:?unknown workload $w}
  [ -n "${DONE[$e]:-}" ] && continue
  DONE[$e]=1
  V=$(venv_of "$w")
  if [ ! -x "$V/bin/python" ]; then
    echo "== venv $V"
    $LIMIT "$PY" -m venv "$V"
    $LIMIT "$V/bin/python" -m pip install -q --disable-pip-version-check --require-hashes \
      --only-binary :all: --no-deps -r "$HERE/reqs/$e.txt"
  fi
done
mkdir -p "$RW/inputs"
$LIMIT "$PY" -I "$HERE/fetch.py" "$RW" inputs
case " $WANT " in *" transformers_"*)
  $LIMIT "$(venv_of transformers_tiny)/bin/python" -I "$HERE/fetch.py" "$RW" models ;;
esac
echo "setup done: $RW"
