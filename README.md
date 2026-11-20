# Ni

Ni is a JIT compiler for CPython: a fork of Armin Rigo's Psyco, ported to
x86-64 and CPython 3.7. Psyco's authors went on to build PyPy
(https://pypy.org).

On CPython 3.7, Ni runs pyperformance about 2.2× faster than plain CPython
and passes CPython's own test suite.

## Why

Ni is a research project. It keeps Psyco's design, with Psyco's
"compile-time CPython" (the layer that mirrors CPython's C code) translated
from CPython's own C source. One Ni source tree serves CPython 3.7 to 3.10:
where they differ, the code follows the features the build detects in the
installed CPython it is built for (`tools/features/installed.py`), never a
version number. A second detector reads CPython's source
(`tools/features/features.py`) and checks the first.

## Use

    make PY=python3.10                        # needs gcc, libffi, libunwind
    python3.10 -c "import ni; ni.start(); ..."   # importing alone starts nothing

`ni.stop()` stops it. `ni.check()` compares the features Ni was built with
(`ni.build_features()`) against the running CPython's
(`ni.runtime_features()`); a difference warns with a `RuntimeWarning`, or
with `error=True` raises one. `ni.start(check=True)` checks first, as does
`NI_START_CHECK=warning` (or `error`); `NI_MODE=off` makes `ni.start()` do
nothing.

The package is `ni/` (its API, in Python, is `ni/__init__.py`) over the
compiled module `ni._core`, built from the C sources in `src/`. On a Python
Ni can't run on (not CPython, not x86-64 Linux, or 3.11 and later) only the
package is installed: `ni.start()` warns and does nothing.

## Status

Research code, not for production. Build with `make`, test with `make check`
(`make check-all` for 3.7–3.10).
