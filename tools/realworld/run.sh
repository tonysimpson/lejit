#!/bin/bash
# run.sh WORKLOAD PYTHON
# Runs one workload offline in its venv and prints "<workload> digest <sha256>", the digest of
# its output, to compare across interpreters. The output itself is kept in
# build/realworld/work/<workload>/output.txt.
#   RW_PREFIX     command put in front of the measured process (a profiler, say)
#   RW_REPEAT     servers: passes over the request list for the digest (default 300)
#   RW_LOAD       servers: then seconds of wrk load on the first request (default 0)
#   RW_ANSIBLE_ARGS  extra ansible-playbook arguments ("--skip-tags modules": controller only)
set -u
W=$1; PY=$2
source "$(dirname "$0")/common.sh"
[ -n "${ENV_OF[$W]:-}" ] || { echo "unknown workload $W; one of: $WORKLOADS" >&2; exit 2; }
V=$(venv_of "$W"); VPY=$V/bin/python
[ -x "$VPY" ] || { echo "no venv $V: run setup.sh first" >&2; exit 2; }
WORK=$RW/work/$W; mkdir -p "$WORK"; cd "$WORK" || exit 1
IN=$RW/inputs; WL=$HERE/wl
PRE=${RW_PREFIX:-}
OUT=$WORK/output.txt

run() { $PRE $LIMIT "$@" > "$OUT"; }

serve() {  # serve PORT server-command...
  local port=$1; shift
  $PRE $LIMIT "$@" > "$WORK/server.log" 2>&1 &
  local bg=$!
  local base=http://127.0.0.1:$port
  curl -s -o /dev/null --retry-connrefused --retry 60 --retry-delay 1 "$base/"
  local args=() i m p b
  for ((i = 0; i < ${RW_REPEAT:-300}; i++)); do
    while read -r m p b; do
      if [ "$m" = POST ]; then args+=(--next -s -X POST -H 'content-type: application/json' --data "$b" "$base$p")
      else args+=(--next -s "$base$p"); fi
    done < "$WL/$W.requests"
  done
  curl "${args[@]:1}" > "$OUT"
  if [ "${RW_LOAD:-0}" != 0 ]; then
    read -r m p b < "$WL/$W.requests"
    wrk -t1 -c1 -d"${RW_LOAD}s" "$base$p" > "$WORK/wrk.txt"
  fi
  # Stop the server: the lowest pid listening on the port is gunicorn's arbiter or uvicorn.
  local pid
  pid=$(ss -ltnpH "sport = :$port" | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -n | head -n 1)
  [ -n "$pid" ] && kill -INT "$pid"
  wait "$bg"
}

case $W in
  django)  serve 8761 "$VPY" $PYFLAGS "$V/bin/gunicorn" --chdir "$WL" -w 1 --bind 127.0.0.1:8761 srv_django:application ;;
  flask)   serve 8762 "$VPY" $PYFLAGS "$V/bin/gunicorn" --chdir "$WL" -w 1 --bind 127.0.0.1:8762 srv_flask:app ;;
  fastapi) serve 8763 "$VPY" $PYFLAGS "$V/bin/uvicorn" --app-dir "$WL" --host 127.0.0.1 --port 8763 \
             --log-level warning --no-access-log srv_fastapi:app ;;
  botocore)   run "$VPY" $PYFLAGS "$WL/botocore_calls.py" ;;
  ansible)
    rm -rf "${WORK:?}/target"; mkdir -p "$WORK/target" "$WORK/tmp"
    ANSIBLE_LOCAL_TEMP=$WORK/tmp ANSIBLE_HOME=$WORK/home ANSIBLE_FORKS=1 ANSIBLE_NOCOLOR=1 \
    ANSIBLE_LOCALHOST_WARNING=False ANSIBLE_INVENTORY_UNPARSED_WARNING=False \
      run "$VPY" $PYFLAGS "$V/bin/ansible-playbook" -i localhost, -e "ansible_python_interpreter=$VPY" \
        -e "target=$WORK/target" ${RW_ANSIBLE_ARGS:-} "$WL/ansible/play.yml" ;;
  pyyaml)     run "$VPY" $PYFLAGS "$WL/pyyaml_manifests.py" ;;
  pytest)     run "$VPY" $PYFLAGS "$WL/pytest_packaging.py" "$IN/packaging-26.3/tests" ;;
  pylint)     run "$VPY" $PYFLAGS "$WL/pylint_requests.py" "$IN/requests-2.34.2/src" ;;
  sphinx)     rm -rf "${WORK:?}/html"; run "$VPY" $PYFLAGS "$WL/sphinx_flask.py" "$IN/flask-3.1.3/docs" "$WORK/html" ;;
  sqlalchemy) run "$VPY" $PYFLAGS "$WL/sqlalchemy_orm.py" ;;
  pandas_callbacks)  run "$VPY" $PYFLAGS "$WL/pandas_callbacks.py" ;;
  pandas_vectorised) run "$VPY" $PYFLAGS "$WL/pandas_vectorised.py" ;;
  numpy_maths)       run "$VPY" $PYFLAGS "$WL/numpy_maths.py" ;;
  transformers_tiny) run "$VPY" $PYFLAGS "$WL/transformers_generate.py" hf-internal-testing/tiny-random-gpt2 \
                       71034c5d8bde858ff824298bdedc65515b97d2b9 60 ;;
  transformers_distilgpt2) run "$VPY" $PYFLAGS "$WL/transformers_generate.py" distilbert/distilgpt2 \
                       2290a62682d06624634c1f46a6ad5be0f47f38aa 12 ;;
esac
rc=$?
[ $rc = 0 ] || echo "$W: exit status $rc" >&2
echo "$W digest $(sha256sum < "$OUT" | cut -d' ' -f1)"
exit $rc
