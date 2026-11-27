# LeJIT

LeJIT is a JIT compiler for CPython: a fork of Armin Rigo's Psyco, ported to
x86-64 and CPython 3.7. Psyco's authors went on to build PyPy
(https://pypy.org).

On CPython 3.7, LeJIT runs pyperformance about 2.2× faster than plain CPython
and passes CPython's own test suite.

## Why

LeJIT is a research project. It keeps Psyco's design, with Psyco's
"compile-time CPython" (the layer that mirrors CPython's C code) translated
from CPython's own C source. One LeJIT source tree serves CPython 3.7 to 3.15:
where they differ, the code follows the features the build detects in the
installed CPython it is built for (`tools/features/installed.py`), never a
version number. A second detector reads CPython's source
(`tools/features/features.py`) and checks the first.

## Use

    make PY=python3.10                        # needs gcc, libffi, libunwind
    python3.10 -c "import lejit; lejit.start(); ..."   # importing alone starts nothing

`lejit.stop()` stops it. `lejit.check()` compares the features LeJIT was built with
(`lejit.build_features()`) against the running CPython's
(`lejit.runtime_features()`); a difference warns with a `RuntimeWarning`, or
with `error=True` raises one. `lejit.start(check=True)` checks first, as does
`LEJIT_START_CHECK=warning` (or `error`); `LEJIT_MODE=off` makes `lejit.start()` do
nothing.

Tools that read a thread's frames from outside Python (faulthandler, py-spy,
gdb, `_remote_debugging`) don't see a function LeJIT runs when C calls it,
such as a `sorted` key or a callback asyncio's event loop runs: where CPython
would show that function, they show the frame that called the C. Python's own
views of frames (tracebacks, `sys._getframe`, `inspect`) see it as CPython
does.

The package is `lejit/` (its API, in Python, is `lejit/__init__.py`) over the
compiled module `lejit._core`, built from the C sources in `src/`. On a Python
LeJIT can't run on (not CPython, not x86-64 Linux, a free-threaded build, or
3.16 and later) only the package is installed: `lejit.start()` warns and does
nothing.

## Status

Research code, not for production. Build with `make`, test with `make check`
(`make check-all` for 3.7–3.10).
`make extra-test` runs the slower tests and the fuzzers (nightly in CI).
