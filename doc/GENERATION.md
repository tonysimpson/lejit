# Plan of work: generate the translated layer on 3.7

The phase is in `GOAL.md` ("What the current phase is") and its design in DECISIONS D67 and METACOMPILER.md "Versions". This is the order of work and where each step stands. What each step did is in its decision and commit; the step notes that were here are in git (`git show e404100:doc/GENERATION.md`).

## The check for every step

**Same code.** A generated function replaces a hand-written one only when Ni emits the same machine code with it, or the difference is recorded with a reason (`tools/samecode.py`, CODE_SIZE.md "Same code"). The behaviour tests and CPython's tests then run as usual.

## Steps

| # | Step | Status |
|---|---|---|
| 0 | **Inventory and same-code tool**: what mirrors CPython, function by function; `tools/samecode.py` compares the code two builds emit. | Done (`8b8ffea`, D70); the inventory is now computed by `donecheck.py` (INVENTORY.md). |
| 1 | **Generation at build time**: `setup.py` runs the generators against the source of the Python it builds for; no `*_gen.h` in git. | Done (`9eae61d`, D68). |
| 2 | **`ceval.c` reader**: 3.7's `TARGET(op)` cases read into `cases_generator`'s form; shared code and core cases listed, not translated. | Done (`7b21dea`, D69). |
| 3 | **Policy table and effect rules**: judgement as data keyed by CPython definitions with their token shape; effect flags computed from a call graph. | Done (`e7fd745`, D71). |
| 4 | **General translation (stage 2)**: an arbitrary C function, first on `abstract.c`. | Done (`1aa473c`, D74). |
| 5 | **Object families**: long, float, bool, math, unicode/bytes, list, tuple, dict, range, iterators, `bltinmodule`, `structmember`, `descrobject`. | Mostly done (D78, D84, D83, D86, D87, D89, D93); what is left is reasoned (below). |
| 6 | **Virtual objects**: constructors and forcing from the struct declarations plus "make virtual" entries. | Done (`a6a687b`, `603363c`, D80). |
| 7 | **Call path**: argument binding from `_PyEval_EvalCodeWithName`; inlining and closure decisions stay core. | Done (`e11f2a3`, D85). |
| 8 | **The rest of the opcodes**: stack effects, branches and jumps, builders, cells, imports, compares. | Mostly done (D73, D79, D81, D82, D88, D93): 75 of 119 opcodes generated; core opcodes stay core. |
| 9 | **Mechanism detectors**: detector and snippet machinery for entry, tracing and frames, tested on other versions' headers. | Done (`36b5beb`, D75; `f254b5d`, D77). |
| – | **Layouts** (D19's stage 0): struct fields from CPython's headers. | Done (`ee76274`, `9869ad9`, D76). |
| 10 | **Done check**: GOAL point 1 as a tool; then, on a quiet machine, `check.sh`, same code against the hand-written baseline (`19fa1df`) and a re-baseline (pyperformance, `compilebench.py`, `codesize.py`). | Point 1 is `tools/metac/donecheck.py` (`88d850c`, D90): no definition without a reason since `18c250d`. Points 2 to 4 not checked. |

A 3.8 dry run of generation is in PORT38.md.

## What is left

- **Reasoned but not done** (QUESTIONS 50): `donecheck.py`'s `policy`, `limit` and non-core `refused` definitions are still the phase's work by GOAL point 1, which wants what is left to be core with a reason. Their reasons are in `tools/metac/hand_table.py`, the policy table's items and METACOMPILER.md's failures log.
- **Opcodes**: `BUILD_CONST_KEY_MAP` (limit) and `IMPORT_NAME` (core: frame state) stay hand-written, reasons in `hand_table.py`; the block-stack, call and exception opcodes are core.
- **Same code** differs, with a reason, wherever a faithful translation fixes a hand-translation bug (KNOWN_BUGS) or a shortcut with no 3.7 C behind it becomes a policy entry or shows as a loss (D29).
- **GOAL points 2 to 4**: nothing hand-translated in the build, and on a quiet machine `check.sh`, same results as the hand-written baseline (`19fa1df`) and a re-baseline with no loss.

## Risks

- **Policy hidden in translations.** Hand translations carry Ni's own decisions inline (`SLOT_OR_GOTO`, `need_reference`, D59's forgetting); each must move to the policy table or the core, or the generated code lacks it.
- **Judgement shortcuts.** Some hand code is faster than a faithful translation (one-digit int paths, P26's float compare whitelist). Same code shows where; each becomes a policy entry or is accepted with its cost (D29).
- **`ceval.c` irregularity.** Shared labels, fall-through and macros between cases. The reader lists what it can't read rather than guessing (GOAL "breaks loudly").
- **Build-time generation** needs the matching CPython source at build time; a missing or different tarball must fail the build, not fall back.
