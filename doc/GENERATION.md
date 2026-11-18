---
written: 2026-11-17
status: plan; step 1 done (`9eae61d`)
---
# Plan of work: generate the translated layer on 3.7

The phase is in `GOAL.md` ("What the current phase is") and its design in DECISIONS D67 and METACOMPILER.md "Versions". This is the order of work, what each step proves, and how it is checked. Estimates are AI elapsed time and are rough; step 4 is the one that decides the rest.

## Where we start [read]

- **Hand-translated:** MAP's `translated` files are 3,353 lines; the `core + translated` and `translated + core` files another 12,004, of which some part is core. Generated so far: 949 lines (`plongobject_gen.h`, `pfloatobject_gen.h`, `pycompiler_gen.h`).
- **Generators:** `tools/metac/` (token reader, slot reader, `gen_long.py`, `gen_float.py`, `gen_ceval.py` with CPython's vendored `cases_generator`), 1,500 lines of Python; `bytecodes37.c` (282 lines, hand-written) describes 43 opcodes, 39 generated.
- **Specification:** PATTERNS.md, 33 patterns: 12 mechanical, 13 judgement, the rest mixed. The judgement ones become entries in the policy table, not code.

## The check for every step

**Same code.** A generated function replaces a hand-written one only when Ni emits the same machine code with it, or the difference is recorded with a reason. Step 0 builds the tool for this; the behaviour tests and CPython's tests then run as usual.

## Steps

| # | Step | Needs | Proves | Estimate |
|---|---|---|---|---|
| 0 | **Inventory and same-code tool.** Split every mixed file in MAP, function by function, into core and translated, each translated function with its CPython counterpart and the patterns it uses. Write `tools/samecode.py`: build twice, compile the benchmarks and tests with each, compare the emitted code per code object. | – | What "all generated" means in numbers (GOAL point 1), and a check that can't be fooled by tests that pass anyway | 2–3 h, parallel by file family |
| 1 | **Generation at build time.** `setup.py` runs the generators against the source of the Python it builds for (`NI_CPYTHON_SRC`, or the release tarball for `sys.version`, checksum-checked and cached); the `*_gen.h` files leave git. `check.sh` follows. | – | D67 (2): nothing generated is checked in | 1–2 h |
| 2 | **`ceval.c` reader.** Read 3.7's `TARGET(op)` cases into the form `cases_generator` gives for `bytecodes.c`: inputs and outputs from `POP`/`TOP`/`PEEK`/`PUSH`/`SET_TOP`/`STACKADJ`, error exits from `goto error`, `DISPATCH`/`FAST_DISPATCH`, `PREDICT`. Check each against `dis.stack_effect`. Cases that jump into shared code (`fast_block_end`, `exception_unwind`) or are core (calls, the block stack) are listed, not translated. `bytecodes37.c` goes; the 39 opcodes regenerate identically. | 1 | D67 (5): the front end is shared by every version | 3–6 h |
| 3 | **Policy table and effect rules.** One data file in Ni, keyed by CPython function name, each entry with the shape of the function it was written for (a token hash, so a change in a later version stops the generator). It holds what PATTERNS calls judgement: what to promote (P03, P04), what to make virtual, where guards go (D54), shortcuts. Effect flags (`CfPure`, `CfNoPython`, error conventions) computed from a call graph of CPython's source, checked against today's hand-written flags. | 0 | Judgement is data, outside CPython's source | 3–5 h |
| 4 | **General translation (stage 2).** Translate an arbitrary C function: compile-time or run-time for each value, type reads (P01, P02), slot dispatch, branches on run-time values to condition codes (P05), ownership, error exits, restart safety. First on `abstract.c` (`PyObject_GetItem`, `PyNumber_*`, `PyObject_Size`…), which is mostly dispatch. Token rules probably stop here; the next tool (pycparser with stub headers) only after the failures log says so. | 3 | The method works beyond simple slots. If it doesn't, the phase is re-planned here | 1–2 days |
| 5 | **Object families.** With step 4's translator: the rest of `long` and `float`, `unicode`/`bytes`, `list`, `tuple`, `dict`, `range`, iterators, `bool`, `bltinmodule`, `structmember`, `descrobject`, `mathmodule`. One agent per family. | 4 | Coverage | 2–4 h each, parallel |
| 6 | **Virtual objects.** Constructors and forcing generated from the struct declarations plus "make virtual" entries in the policy table (PATTERNS E). | 3, 4 | Representation is rules, not code | 4–8 h |
| 7 | **Call path.** `psyfunc.c`'s argument binding from `ceval.c`'s `_PyEval_EvalCodeWithName`, and the calls in `pfuncobject.c`, `pmethodobject.c`, `pclassobject.c`. Inlining and closure decisions (D58) stay core; split them out first. | 4 | – | 4–8 h |
| 8 | **The rest of the opcodes.** Everything step 2 reads beyond the simple ones: attribute and global loads, compares, `BUILD_*`, `UNPACK_*`, `FOR_ITER`, jumps as condition codes. Core opcodes (calls, exceptions, block stack) stay core. | 2, 4 | `bytecodes37.c` and the hand-written cases in `pycompiler.c` are gone | 1 day |
| 9 | **Mechanism detectors.** The detector and snippet machinery for entry, tracing and frames. 3.7 has one mechanism for each, so test the detectors on other versions' headers too (3.8, 3.12), without building them. | 1 | D67 (4) is ready for 3.8 | 2–3 h |
| 10 | **Done check.** Quiet machine: `check.sh`, same code against the hand-written baseline (`19fa1df`), re-baseline (pyperformance, `compilebench.py`, `codesize.py`) in STATUS. | all | GOAL points 3 and 4 | 2 h |

Order: 0, 1 and 2 can run at once; then 3; then 4 alone; then 5, 6, 7 and 8 in parallel; 9 any time after 1. Total, if step 4 goes as hoped: about 4–7 days elapsed.

## Step 1: done (`9eae61d`, D68) [run]

`setup.py`'s `build_ext` runs `tools/metac/generate.py --python sys.executable --out build/temp.*/gen` before compiling; the three `*_gen.h` files are gone from git. How to build, the cache and `NI_CPYTHON_SRC` are in BUILD.md "Build".

**Interface, for the steps that add or change generators:**
- `generate.py` holds the list `GENERATORS`: (module, header it writes). Each module in `tools/metac/` provides `build(target) -> (text, report)`, where `report` is lines starting `generated` or `skipped`, and raises `cslots.GenError(reason)` when the source doesn't match what it relies on. Nothing else is called.
- `target` is `cslots.Target(src, version, python, pyconfig)`: the source tree (already version-checked), its `PY_VERSION`, the interpreter being built for (run it for anything only it knows, as `gen_ceval.stack_effects` does with `dis.stack_effect`) and its installed `pyconfig.h`.
- No generator writes files or reads `NI_*` variables; `generate.py` writes all headers, and only after every generator succeeded.
- Hand-written code names no version (D67): the version comes from `target.version`, the source from `target.src`. The only version-keyed data is `tools/metac/cpython_sha256.txt`.
- For step 2: `gen_ceval.py`'s internals (`read_defs`, `translate`, `run`, `check_37`, `header`) are unchanged except that `check_37` and `stack_effects` take the interpreter as an argument. Its `main` is now only `--try BYTECODES_C CPYTHON_SRC`, and `build(target)` is the entry: replacing `run(DEFS, target.src)` with the `ceval.c` reader is the whole change there.

**Checks** (`tools/check.sh`): the generators run twice and must give identical output; the build must then have compiled the same headers. The cross-checks of `gen_ceval` run in every generation, so in every build.

## Risks

- **Step 4 is the method.** No generator has yet translated a function with general control flow and ownership; Psyco's hand translations make these choices by intuition. If token rules and then pycparser both fail on `abstract.c`, the phase needs re-planning there, not later.
- **Policy hidden in translations.** Hand translations carry Ni's own decisions inline (`SLOT_OR_GOTO`, `need_reference`, D59's forgetting). The inventory (step 0) must find each and move it to the policy table or the core, or the generated code will lack it.
- **Judgement shortcuts.** Some hand code is faster than a faithful translation (one-digit int paths, P26's float compare whitelist). Same code shows where; each becomes a policy entry or is accepted with its cost (D29).
- **`ceval.c` irregularity.** Shared labels, fall-through and macros between cases. The reader lists what it can't read rather than guessing (GOAL "breaks loudly").
- **Build-time generation** needs the matching CPython source at build time; a missing or different tarball must fail the build, not fall back.
