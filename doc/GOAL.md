---
written: 2026-11-16
updated: 2026-11-17
---
# Goal and scope

## Thesis

Psyco (Armin Rigo et al., 2001–2010) was a proven, working JIT for CPython 2.x: a plug-in extension module, no interpreter changes, real speed-ups on real code. Its cost was maintenance: every CPython C function it optimised had to be written again by hand in a "compile-time" form (`ni/Objects/p*.c`, `ni/Python/pycompiler.c`), and those copies had to follow every CPython release.

Ni (this repo) is a fork that ported Psyco to x86-64 and part-way to CPython 3.7, and met that cost first-hand.

Psyco's authors went on to build PyPy, which derives its JIT from an interpreter written in RPython. Ni explores a different route: keep Psyco's design, and treat **CPython's own C source as the input to a meta-compiler** that generates the compile-time versions automatically. The idea is Armin's; he describes it in `ni/Python/pycompiler.h` (Meta functions section):

> The code of the meta function typically ressembles that of the original function enough that we might dream about a language in which we never have to write the two versions (where Psyco's version could be derived automatically from the standard version).

Ni's bet is that today's C tooling (doc/METACOMPILER.md) makes that route workable.

## What the current phase is

**Generate the translated layer on 3.7 (from 2026-11-17).** Everything in Ni that mirrors CPython is to be generated from CPython's own source, then the same generator is taken to 3.8, 3.9 and on, one version at a time, with every version kept (DECISIONS D67). This phase does the first step on 3.7, where the hand translations exist to check against. The plan of work is `doc/GENERATION.md`; the method is in `doc/METACOMPILER.md` ("Versions", stages, failures log).

What it builds:
- A reader for `ceval.c`'s cases that produces the same form CPython's parser gives for 3.12's `bytecodes.c`, replacing the hand-written `tools/metac/bytecodes37.c` (D67 revises D26).
- The generator's rules, kept in Ni and never in CPython's source (which is only read): the policy table (what to promote, make virtual, guard), representation rules, and effect rules (`CfPure`, `CfNoPython`, error conventions), computed from the source where possible.
- Generation at build time from the target version's source; generated output is not checked in (D67 replaces D24).
- Detectors and snippets for the mechanisms Ni must choose between (entry, tracing, frames), chosen by what the source offers, never by a version number.

Rules for the phase:
- No `#if` on CPython versions in hand-written code. Hand-written code is the core, the backend (with `llvm-mc` templates, D65) and the rules.
- A hand translation is replaced only when generated code does the same: the same calls in the same order, or a recorded reason why not (as for `gen_long.py` against `3702e3f`).
- A new JIT feature goes into the core or the rules, never into generated output, so it reaches every version.
- Each failure of the generator goes in METACOMPILER's failures log with the CPython construct that caused it, before a heavier tool is tried.

**When the phase is done.** All of:
1. Every definition in Ni that mirrors a CPython one is generated, or hand-written with a named reason: `tools/metac/donecheck.py` (D90) exits 0. Whether a reason that is only a policy item the translator doesn't implement is enough is D99.
2. Nothing in the build is hand-translated from CPython; `bytecodes37.c` is gone.
3. `tools/check.sh` passes, and the behaviour tests and CPython's tests give the same results as the hand-written baseline (`19fa1df`).
4. A re-baseline shows no loss against that baseline: pyperformance, `tools/compilebench.py` and `tools/codesize.py`, recorded in the commit message.

Then 3.8 (positional-only arguments, CPython's own vectorcall, a few opcodes) is the first test that versions differ only in the input.

**Previous phase, done 2026-11-17:** a robust 3.7 baseline with worthwhile performance. All four of its points were met at `cdd8a94`: pyperformance 2.19×, `tools/check.sh` passing, no expected failures. Its open items are in `doc/BACKLOG.md`; those moved to later versions are marked there (D64).

Hand-written improvements are translated-layer code too: each one is something the generator must reproduce. Record the pattern behind each in `doc/PATTERNS.md`.

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
- **A meta-compiler that consumes CPython's source.** It reads the source of each supported CPython from 3.7 on, one Ni source for all of them (D67), including the interpreter loop and the object implementations, and generates the whole Python-specific layer: the bytecode compiler, the compile-time object protocol, field descriptors, object layouts, refcount and error conventions.
- **Breaks loudly.** When a new CPython makes an incompatible change, generation or the build fails with a message that names the CPython construct and the assumption it broke. Generated code mustn't silently compile to something wrong. For example, it must check every struct offset and size it relies on (`static_assert` against the real headers), and refuse a C construct it doesn't understand rather than guessing.

The owner accepts this is a very high bar. Without it the code stays unmaintainable (DECISIONS D16).

## Out of scope — do not propose these

- Tracing JITs, meta-tracing (PyPy/RPython), method JITs built on a new IR (Cinder, Pyston), copy-and-patch (CPython 3.13+ `_Py_JIT`), or "just use Numba/Cython". These are known alternatives; the owner has deliberately chosen Psyco's approach. Mention them only where a specific technique helps the Psyco-style design.
- LLVM IR, or any compiler IR, as the meta-compiler's input or as a stage of Ni's code generation. The translated layer is generated from CPython's C source (D19, D26). LLVM is used only as an assembler (`llvm-mc`), at generation time, to turn hand-written machine-code templates into bytes and fixups (QUESTIONS 37, D65); Ni neither links nor runs it.
- Modifying CPython itself. Psyco is a plug-in; Ni uses the PEP 523 frame-eval hook. That constraint stays.
- Rewriting the core before the classification work is done.

## Success looks like

For this phase: the four points of "When the phase is done" above, with each performance change measured against speed, compile time and code size (D29). The classification phase before it is done: every file classified (`doc/MAP.md`), the patterns catalogued (`doc/PATTERNS.md`), the port's removals known (`doc/history/`), the core described (`doc/ARCHITECTURE.md`, `doc/GLOSSARY.md`).
