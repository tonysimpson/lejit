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
CPython source it is built for (`tools/features/features.py`), never a version
number.

## Use

    make PY=python3.10                        # needs gcc, libffi, libunwind
    python3.10 -c "import ni; ..."            # importing ni enables it for the process

`ni.build_info` gives the features and the version of the CPython source
they were detected in.

## Status

Research code, not for production. Build with `make`, test with `make check`
(`make check-all` for 3.7–3.10).
