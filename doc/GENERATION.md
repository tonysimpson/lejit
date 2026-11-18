---
written: 2026-11-17
status: plan; steps 1 (`9eae61d`) and 2 (`7b21dea`) done
---
# Plan of work: generate the translated layer on 3.7

The phase is in `GOAL.md` ("What the current phase is") and its design in DECISIONS D67 and METACOMPILER.md "Versions". This is the order of work, what each step proves, and how it is checked. Estimates are AI elapsed time and are rough; step 4 is the one that decides the rest.

## Where we start [read]

- **Hand-translated:** MAP's `translated` files are 3,353 lines; the `core + translated` and `translated + core` files another 12,004, of which some part is core. Generated so far: 949 lines (`plongobject_gen.h`, `pfloatobject_gen.h`, `pycompiler_gen.h`).
- **Generators:** `tools/metac/` (token reader, slot reader, `gen_long.py`, `gen_float.py`, `gen_ceval.py` with CPython's vendored `cases_generator`), 1,500 lines of Python; `bytecodes37.c` (282 lines, hand-written) describes 43 opcodes, 41 generated (INVENTORY.md).
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

## Step 0, the inventory: done (INVENTORY.md) [read]

`doc/INVENTORY.md` splits every in-scope file function by function (and `pycompiler.c` opcode by opcode): class, CPython 3.7 counterpart, patterns, the policy each carries. The tool half (`tools/samecode.py`) is separate. What it changes here:
- **Numbers.** 15,652 lines in scope, 14,346 without the compiled-out `pstringobject.*`: 3,811 translated, 3,298 mixed, 5,490 core, 1,747 glue. GOAL point 1 is about the first two; the core lines get named as core in MAP. `mergepoints.c`, `frames.c` and `pfuncobject.c` are almost all core; `pbltinmodule.c` (MAP: `translated`) is half mixed.
- **Step 3** starts from the policy items in the per-file tables: 72 effect-flag sets, 59 guards (nearly all D54's one rule), 60 virtual-object choices, 53 shortcuts, 25 promotion choices, 17 forgetting sites, 14 ownership choices, 7 megamorphic cut-offs, and the judgement that is code today (D53's `cannot_block_init`, the inlining limits, the opcode whitelist, field mutability).
- **Step 4** must give two outputs for some functions: the meta and the run-time copy D54 resumes in (`binary_op1`/`cimpl_binary_op1`, `binary_iop1`, `sequence_repeat`, the `PyNumber_*` sequence fallbacks).
- **Step 7** is mostly core: only `psyfunc.c`'s `fncall_*` argument binding is translation, and `_PyFunction_FastCall*` are in `Objects/call.c` in 3.7. The PsycoFunction proxy (about 300 lines, no caller) can go first.
- **Step 8**: 41 opcodes are generated today (not 39); the block-stack and `with` cases are core (Ni's finally states, not `WHY_*`); ten 3.7 opcodes are not compiled at all.
- **Same code** will differ, with a reason, wherever a faithful translation fixes a hand-translation bug: KNOWN_BUGS F97–F104, all found by the inventory and not yet fixed. Shortcuts with no 3.7 C behind them (one-digit bit operations, `divmod()`'s fast path, power-of-two divisors, `PsycoDict_NewPresized`'s small case) must become policy entries or show as a loss.

## Step 0, the tool half: done (`54afbce`, D70) [run]

`tools/samecode.py A B` compares the code two builds emit (revisions, directories or `.so` files; it builds revisions itself with `NI_CODESTATS=1`). Workload: `tests/cases` and the seven benchmarks, about 2 minutes. Baseline against itself: all same, four full runs. It found a deliberate swap in `plist_item` at list `FOR_ITER`/`BINARY_SUBSCR`, and C54 (`a599bd2`) at the four `isinstance` calls it changes. Use: build the generated function's revision against its parent and require "0 differ", or record each difference with a reason. Definition, method and limits (per instruction, not per meta function; inlined callees show the caller's state) in CODE_SIZE.md "Same code".

## Step 1: done (`9eae61d`, D68) [run]

`setup.py`'s `build_ext` runs `tools/metac/generate.py --python sys.executable --out build/temp.*/gen` before compiling; the three `*_gen.h` files are gone from git. How to build, the cache and `NI_CPYTHON_SRC` are in BUILD.md "Build".

**Interface, for the steps that add or change generators:**
- `generate.py` holds the list `GENERATORS`: (module, header it writes). Each module in `tools/metac/` provides `build(target) -> (text, report)`, where `report` is lines starting `generated` or `skipped`, and raises `cslots.GenError(reason)` when the source doesn't match what it relies on. Nothing else is called.
- `target` is `cslots.Target(src, version, python, pyconfig)`: the source tree (already version-checked), its `PY_VERSION`, the interpreter being built for (run it for anything only it knows, as `gen_ceval.stack_effects` does with `dis.stack_effect`) and its installed `pyconfig.h`.
- No generator writes files or reads `NI_*` variables; `generate.py` writes all headers, and only after every generator succeeded.
- Hand-written code names no version (D67): the version comes from `target.version`, the source from `target.src`. The only version-keyed data is `tools/metac/cpython_sha256.txt`.
- For step 2: `gen_ceval.py`'s internals (`read_defs`, `translate`, `run`, `check_37`, `header`) are unchanged except that `check_37` and `stack_effects` take the interpreter as an argument. Its `main` is now only `--try BYTECODES_C CPYTHON_SRC`, and `build(target)` is the entry: replacing `run(DEFS, target.src)` with the `ceval.c` reader is the whole change there.

**Checks** (`tools/check.sh`): the generators run twice and must give identical output; the build must then have compiled the same headers. The cross-checks of `gen_ceval` run in every generation, so in every build.

## Step 2: done (`7b21dea`, D69) [run]

`tools/metac/ceval_reader.py` reads 3.7's 119 `TARGET` cases; `gen_ceval.build` takes its definitions from it (or from `Python/bytecodes.c` when the source has one), and `bytecodes37.c` is gone. Details, the rules and the refusals by kind: METACOMPILER.md "`ceval.c` reader".
- **Read 51, generated 44, refused 68**: core 21, oparg 18, control flow 15, shared code 8, falls through 4, preprocessor 1, stack 1. The 7 read but not generated are vocabulary (`DELETE_SUBSCR`, `LOAD_CLOSURE`, `STORE_DEREF`, `COMPARE_OP`, `IMPORT_NAME`, `IMPORT_FROM`, `JUMP_FORWARD`).
- **The 41 regenerate the same** but for variable names (10 cases) and `DUP_TOP_TWO`'s incref order; case order is `ceval.c`'s. New: `LOAD_ATTR`, `STORE_ATTR`, `DELETE_ATTR`, with the same calls as the hand-written cases they replace (removed from `pycompiler.c`).
- **Checks:** every derived effect against `dis.stack_effect` (nine opargs) in every generation; a mutation test (wrong readers, wrong `ceval.c`) fails or refuses each time. Checked build: `tests/run.py` 747 pass, compile bounds, the seven `profile_bench.py` runs, CPython's default set 285 modules, 0 unexpected. Release build, instructions per warm iteration on the seven benchmarks against `b36a1ac`: within ±0.05% (STATUS).
- **For step 8:** the reader already gives definitions for the compares, imports, closures and jumps; what stops them is back-half vocabulary (failures log). The refused kinds core, shared code and falls through stay hand-written by design; oparg and control flow need array inputs and branches on run-time values.

## Risks

- **Step 4 is the method.** No generator has yet translated a function with general control flow and ownership; Psyco's hand translations make these choices by intuition. If token rules and then pycparser both fail on `abstract.c`, the phase needs re-planning there, not later.
- **Policy hidden in translations.** Hand translations carry Ni's own decisions inline (`SLOT_OR_GOTO`, `need_reference`, D59's forgetting). The inventory (step 0) must find each and move it to the policy table or the core, or the generated code will lack it.
- **Judgement shortcuts.** Some hand code is faster than a faithful translation (one-digit int paths, P26's float compare whitelist). Same code shows where; each becomes a policy entry or is accepted with its cost (D29).
- **`ceval.c` irregularity.** Shared labels, fall-through and macros between cases. The reader lists what it can't read rather than guessing (GOAL "breaks loudly").
- **Build-time generation** needs the matching CPython source at build time; a missing or different tarball must fail the build, not fall back.
