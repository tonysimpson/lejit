---
written: 2026-11-16
head_ref: e1fd808
---
# Goal and scope

## Thesis

Psyco (Armin Rigo et al., 2001–2010) was a proven, working JIT for CPython 2.x: a plug-in extension module, no interpreter changes, real speed-ups on real code. It died because it was **unmaintainable**, not because the design was wrong. Every CPython C function it optimised had to be re-written by hand in a "compile-time" form (`ni/Objects/p*.c`, `ni/Python/pycompiler.c`), and those copies drifted with every CPython release.

Ni (this repo) is a fork that ported Psyco to x86-64 and part-way to CPython 3.7. It confirmed the maintenance problem first-hand.

The Psyco authors' answer was PyPy (write the interpreter in RPython and derive the JIT from it). **Ours is different:** keep Psyco's design, and treat **CPython's own C source as the input to a meta-compiler** that generates the compile-time versions automatically. Armin anticipated this in `ni/Python/pycompiler.h` (Meta functions section):

> The code of the meta function typically ressembles that of the original function enough that we might dream about a language in which we never have to write the two versions (where Psyco's version could be derived automatically from the standard version).

## What the current phase is

Research and classification, not new code. For everything in this repo, decide which of these it is:

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

1. Every file and every significant function in this repo is classified (`doc/MAP.md`).
2. The translation patterns are catalogued with examples (`doc/PATTERNS.md`).
3. What was removed from Psyco during the port, and why, is known (`doc/history/`).
4. There's enough understanding of the core (`doc/ARCHITECTURE.md`, `doc/GLOSSARY.md`) to design a tidied core and a meta-compiler interface against it.
