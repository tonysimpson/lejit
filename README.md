# Ni JIT

> **Status (2026):** dormant; being studied as the template for a maintainable
> Psyco-style JIT whose compile-time CPython layer is generated from CPython's
> C source. Start with `CLAUDE.md` and `doc/GOAL.md`.

Ni aims to provide a module that just makes Python faster.

Ni is based on a fork of Armin Rigo's Psyco, Armin and the other Psyco
developers moved onto PyPy so go check that out if you want a high performance
implementation of Python (http://pypy.org).

## Current state

* 64 bit port working ok but machine code is far from optimal
* Compiles against Python 3.7 and will compile simple function
* Many pysco limitation still remain, many Py2 behaviours will still remain.

Will compile very simple functions see mergepoints.c for supported opcodes.

Examples:

    def test(a, b):
        return a + b

Or

    def test(n):
        i = 0
        while i < n:
            i = i + 1
        return i

Like I say very simple but I suspect other opcodes could be re-enable with
very little effort. Any discoveries in this area would be very interesting.

I know I should really explain now to use nianalyser and gdb to debug things.

## Milestones

* Improve debugging and execution performance analysis - nianalyser - DONE
* Refactor code for maintainability and to CPython coding standards - On-going
* Make Ni work on all Python code - On-going
* Improve preformance and implement better optimisation decision making - Very on-going
* Python 3 port - on-going but progress made

I really need to make Trello for all this.
