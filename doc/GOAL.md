---
written: 2026-11-16
updated: 2026-11-17
---
# Goal and scope

## Thesis

Psyco (Armin Rigo et al., 2001–2010) was a proven, working JIT for CPython 2.x: a plug-in extension module, no interpreter changes, real speed-ups on real code. It died because it was **unmaintainable**, not because the design was wrong. Every CPython C function it optimised had to be re-written by hand in a "compile-time" form (`ni/Objects/p*.c`, `ni/Python/pycompiler.c`), and those copies drifted with every CPython release.

Ni (this repo) is a fork that ported Psyco to x86-64 and part-way to CPython 3.7. It confirmed the maintenance problem first-hand.

The Psyco authors' answer was PyPy (write the interpreter in RPython and derive the JIT from it). **Ours is different:** keep Psyco's design, and treat **CPython's own C source as the input to a meta-compiler** that generates the compile-time versions automatically. Armin anticipated this in `ni/Python/pycompiler.h` (Meta functions section):

> The code of the meta function typically ressembles that of the original function enough that we might dream about a language in which we never have to write the two versions (where Psyco's version could be derived automatically from the standard version).

He went further on the mailing list. On 2002-11-18 he wrote: "I'm currently designing a simple C parser that can turn selected portions of the C core of the regular Python interpreter into a high-level Python version. Yes, that's a kind of C-to-Python translator!" By 2003-10-10 he had moved to PyPy, because the `Objects` files "could be generated automatically if CPython were written in a higher level language than C". The list never says why he dropped the parser. Ni's bet is that today's C tooling (doc/METACOMPILER.md) makes his first route workable. [read] (`../psyco-history/archive/psyco-devel/2002/2002-11-18_999647.md`, `2003/2003-10-10_999800.md`)

## What the current phase is

**A robust 3.7 baseline with worthwhile performance (from 2026-11-17).** The classification work below is done (`doc/MAP.md`, `doc/PATTERNS.md`, `doc/history/`). The phase now is making the 3.7 baseline correct and fast enough to be worth generating: fixing open bugs, and taking the performance improvements that measurement shows are worthwhile (D29: compile time and code size count too). The open items are in `doc/BACKLOG.md`. Then come the 3.12 port (`doc/PORT312.md`) and the meta-compiler, with this baseline as the specification and test oracle.

**When the phase is done** (owner, 2026-11-17). All of:
1. The work in flight is merged or dropped with a reason: inline stores within budget, the correctness batch (F10, F48, F50, F52, C25), C27/C28, specialisation step 3, C30/C31.
2. No known bug gives a wrong result or a crash in the default test runs (`tests/run.py`, `tests/cpython_tests.py`). Accepted differences are only frame visibility and tracing of already-running frames (C11, C29), each cited in `doc/KNOWN_BUGS.md`.
3. `tools/check.sh` prints ALL CHECKS PASSED on a quiet machine.
4. A re-baseline is recorded with no agents running: pyperformance against CPython 3.7, `tools/compilebench.py`, `tools/codesize.py`, in `doc/STATUS.md`.

Anything else open then (option D, the megamorphic table, strings, C11) moves to the 3.12 backlog unless it blocks one of these.

Hand-written improvements in this phase are translated-layer code too: each one is something the generator must later reproduce. Record the pattern behind each in `doc/PATTERNS.md`.

The classification still applies to everything in the repo:

| Class | Meaning | What happens to it |
|---|---|---|
| **core** | Specialiser internals: value representation (`vinfo_t`/`Source`), merge points and unification, snapshots, promotion, respawn, virtual objects, code buffers. | Kept as the design template; tidied and rewritten by hand. |
| **translated** | Hand-written compile-time copies of CPython functions. | Becomes **output** of the meta-compiler. The hand versions are the specification and the test oracle. |
| **backend** | x86-64 machine-code emission and register allocation. | Rewritten behind a clean interface. |
| **glue / tooling** | Module init, eval hook, build, nianalyser. | Kept or replaced as convenient. |
| **obsolete** | Py2-only or 32-bit-only, or features nobody needs. | Recorded and dropped. |

The key output for the translated layer is **`doc/PATTERNS.md`**: the catalogue of rewrites that turn a CPython C function into its compile-time form. It records which are **mechanical** (derivable from the C source) and which are **judgement** (heuristics, limits, extra knowledge). That catalogue is the meta-compiler's specification.

## Long-term aim (the bar every design choice is measured against)

- **A version-independent core.** The specialiser core (values and binding times, snapshots and merge points, promotion, virtual objects, code buffers, backend) knows nothing about Python. It doesn't change when CPython changes.
- **A meta-compiler that consumes CPython's source.** It reads the source of whichever CPython 3.8+ is targeted (3.7 tolerated for now), including the interpreter loop and the object implementations, and generates the whole Python-specific layer: the bytecode compiler, the compile-time object protocol, field descriptors, object layouts, refcount and error conventions.
- **Breaks loudly.** When a new CPython makes an incompatible change, generation or the build fails with a message that names the CPython construct and the assumption it broke. Generated code mustn't silently compile to something wrong. For example, it must check every struct offset and size it relies on (`static_assert` against the real headers), and refuse a C construct it doesn't understand rather than guessing.

The owner accepts this is a very high bar. Without it the code stays unmaintainable (DECISIONS D16).

## Out of scope — do not propose these

- Tracing JITs, meta-tracing (PyPy/RPython), method JITs built on a new IR (Cinder, Pyston), copy-and-patch (CPython 3.13+ `_Py_JIT`), or "just use Numba/Cython". These are known alternatives; the owner has deliberately chosen Psyco's approach. Mention them only where a specific technique helps the Psyco-style design.
- Modifying CPython itself. Psyco is a plug-in; Ni uses the PEP 523 frame-eval hook. That constraint stays.
- Rewriting the core before the classification work is done.

## Success looks like

For this phase: the backlog's bugs fixed or knowingly accepted, CPython's tests and pyperformance passing under Ni, and each performance change measured against speed, compile time and code size.

For the classification phase (done):

1. Every file and every significant function in this repo is classified (`doc/MAP.md`).
2. The translation patterns are catalogued with examples (`doc/PATTERNS.md`).
3. What was removed from Psyco during the port, and why, is known (`doc/history/`).
4. There's enough understanding of the core (`doc/ARCHITECTURE.md`, `doc/GLOSSARY.md`) to design a tidied core and a meta-compiler interface against it.
