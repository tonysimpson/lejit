# Sourced by setup.sh, run.sh and profile.sh. Expects PY (an interpreter) to be set.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RW=${RW_HOME:-$REPO/build/realworld}
LIMIT="prlimit --as=${RW_MEMLIMIT:-4000000000}"

# The suite, then the controls. name -> venv
declare -A ENV_OF=(
  [django]=django [flask]=flask [fastapi]=fastapi [botocore]=botocore [ansible]=ansible
  [pyyaml]=pyyaml [pytest]=pytest [pylint]=pylint [sphinx]=sphinx [sqlalchemy]=sqlalchemy
  [pandas_callbacks]=pandas [transformers_tiny]=transformers
  [numpy_maths]=numpy [transformers_distilgpt2]=transformers [pandas_vectorised]=pandas
)
WORKLOADS="django flask fastapi botocore ansible pyyaml pytest pylint sphinx sqlalchemy pandas_callbacks transformers_tiny numpy_maths transformers_distilgpt2 pandas_vectorised"
SERVERS="django flask fastapi"

tag_of() {  # e.g. cpython3.12
  "$1" -c 'import sys, platform; print(platform.python_implementation().lower() + "%d.%d" % sys.version_info[:2])'
}

venv_of() {  # venv directory for workload $1 under interpreter $PY
  echo "$RW/envs/${RW_TAG:-$(tag_of "$PY")}/${ENV_OF[$1]}"
}

# Offline after setup, single-threaded maths libraries, no bytecode cache writes into the venv.
export HF_HOME=$RW/hf HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 HF_HUB_DISABLE_TELEMETRY=1
export PIP_NO_INDEX=1 OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1
# A fixed hash seed makes set and dict-of-str orders, and so outputs such as pylint's
# duplicate-code report, the same from run to run. -s -P rather than -I, which would ignore it.
unset PYTHONPATH PYTHONHOME PYTHONSTARTUP PYTHONUSERBASE
export PYTHONHASHSEED=0
PYFLAGS="-s -P"
