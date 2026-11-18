---
written: 2026-11-17
status: plan; steps 1 (`9eae61d`), 2 (`7b21dea`), 3 (`e7fd745`) and 9 (`36b5beb`) done, and D19's stage 0, layouts (`ee76274`, D76); step 8 begun (front end, `99d9b26`); 3.8 dry run (`2a71def`, PORT38.md)
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
- **Step 7** is mostly core: only `psyfunc.c`'s `fncall_*` argument binding is translation, and `_PyFunction_FastCall*` are in `Objects/call.c` in 3.7. The PsycoFunction proxy (about 300 lines, no caller) is gone (`896dfaf`).
- **Step 8**: 41 opcodes are generated today (not 39); the block-stack and `with` cases are core (Ni's finally states, not `WHY_*`); ten 3.7 opcodes are not compiled at all.
- **Same code** will differ, with a reason, wherever a faithful translation fixes a hand-translation bug: KNOWN_BUGS F97–F104, all found by the inventory and not yet fixed. Shortcuts with no 3.7 C behind them (one-digit bit operations, `divmod()`'s fast path, power-of-two divisors, `PsycoDict_NewPresized`'s small case) must become policy entries or show as a loss.

## Step 0, the tool half: done (`8b8ffea`, D70) [run]

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

## Step 3: done (`e7fd745`, D71) [run]

The policy table is `tools/metac/policy_table.py`, checked by `policy.py` in every generation; the effect rules are `tools/metac/effects.py`. Details: METACOMPILER.md "Policy table" and "Effect rules".
- **Table:** 250 entries keyed by CPython definitions (55 `ceval.c` cases, 44 in `abstract.c`, 22 in `longobject.c`…), 93 with policy items (186 items), 11 rules, 9 limits, 22 light opcodes, 12 types, 38 cannot-block functions, the two type whitelists, and the effect rules' 25 seeds, 28 external functions and 6 assumptions.
- **Moved:** `gen_float.py`'s `LIBM` and `ERRORS` come from the table; the generated headers are byte-identical. Nothing else in the generators is policy.
- **Effects against Ni's 225 call sites:** no Python 182 agree, 28 cautious, 2 disagree; pure 197, 6, 2, plus 7 allocators; error checking 143, 6, 8; `CfNewRef` 33 of 33; `CfNoBlock` 74, 82, 1; D53's list 37 of 38. Of the 13 disagreements, two are flag bugs (F104, known; F105, new), eleven are limits (failures log).
- **Checks:** a changed function in a copy of the source fails generation and names it; an unknown key, value, Ni name, limit or field fails. Checked build: `tests/run.py` 747 pass, compile bounds 7, CPython's default set 285 modules, 0 unexpected.

**For the steps that follow:**
- **Reading the table:** `import policy_table`; `ENTRIES[key]['policy']` is a list of `(kind, value[, note])`; `RULES[name]` says what is one rule. `generate.py` runs `policy.check` before any generator, so a generator can trust every key and shape.
- **Step 4** needs the `promote`, `mega`, `likely`, `restype` and `ref` items of `abstract.c`'s 44 entries, the `slot_guard` and `restart` rules, and flags from `effects.py`: make it a library call (`effects.analyse(src)`) from the generator rather than a table, and add per-site assumptions to `ASSUMPTIONS`.
- **Step 5:** `shortcut`/`extra` items per family (`one_digit`, `power_of_two`, `small_presized`, `type_whitelist`); an `extra` item is code the generator must write from the table, as there is no C for it.
- **Step 6:** `TYPES` (fields, mutability, weights) and the `virtual` items.
- **Step 7:** the `limit` items (`max_args`, `depth`, `max_frees`) and `LIMITS`; `forget` items at the call sites.
- **Step 8:** the `ceval.c` entries' `guard`, `promote` and `other` items (`LOAD_GLOBAL`'s dict-version guard, `ends_block`), and `LIGHT_OPCODES`.
- When code moves from hand-written to generated, the checks that compare the table with Ni's code (limits, `cannot_block_init`, whitelists) should be replaced by the generator reading the table.

## Step 4: done (`1aa473c`, D74) [run]

General translation works on `abstract.c`: `tools/metac/gen_abstract.py` (with `tools/metac/cparse.py`) writes `pabstract_gen.h`. METACOMPILER.md "General translation" has the method, what was generated and why each difference in the code is there.
- **Generated:** 49 functions (every `abstract.c` function Ni declares a meta for, but four kept by policy items and `PySequence_Tuple`, plus their static helpers), with 9 run-time copies. `pabstract.c` lost 52 definitions, 1,106 → 297 lines. `DELETE_SUBSCR` is now a generated opcode.
- **Same code:** `tools/samecode.py d71b0f6 a8c06a9`: 59 units same, 24 differ (3,991 of 4,075 code objects). Every difference is recorded with a reason: two bug fixes (F101; F102 for `seq * int` and the item functions), the C's own order in `PySequence_GetItem`/`SetItem`, `**=` translated instead of residual, a different run-time copy for `>>`'s hint, `_PySequence_IterSearch`'s real return type, register names, and a respawn count. Release instructions per warm iteration on the seven benchmarks: no change (within 0.2%).
- **Token rules didn't stop here.** A statement reader over ctokens plus the preprocessor's macro list and declarations was enough; pycparser was not needed.
- **Limits for step 5:** no loops (`PySequence_Tuple`, `long_compare`), an `int` result's convention needs a Ni declaration, only parameters are promoted, a resume inside a statement that already called is refused, `#`/`##` macros aren't expanded. Each refuses with a reason rather than guessing.
- **Verdict:** the method holds for dispatch code. The policy table carried only `likely` and `not_translation`; everything else came from the C, Ni's declarations and the effect rules. Step 5's families have loops and representation (virtual objects), which this translator doesn't do yet: loops are the first thing to add.

## Step 5, first part: loops, and the first object file (`83131e1`, D78) [run]

The translator now has a loop rule (METACOMPILER.md "Loops"): a counted loop with a compile-time bound is unrolled by the meta, a tuple's length is asked of `PsycoTuple_Load` with CPython's function as the fallback, any other loop resumes CPython. `loop_rule` is shared with `gen_ceval.py` for the builder opcodes (`BUILD_SET`, `BUILD_MAP`, `BUILD_CONST_KEY_MAP`, `BUILD_*_UNPACK`), whose loops count `oparg` down.
- **Generated:** `tupleobject.c:tuplecontains` into `ptupleobject_gen.h` (the first object-family file: `--unit tuple`, slots from `policy_table.TRANSLATED_SLOTS`); `abstract.c:PySequence_Tuple` (hand meta removed), `PySequence_List`, `_PySequence_IterSearch`. F102 fixed for list and tuple `[int]`.
- **Found:** a core respawn bug, KNOWN_BUGS F106 (the flags' condition not resolved on a replay or on the branch taken), fixed in `dispatcher.c`.
- **Same code:** `tools/samecode.py baseline-3.7 69a0bdd` (this code before its rebase onto `9ee0da7`, which changed only generator files whose 3.7 output is the same): 1 unit same, 83 differ (3,610 of 4,126 code objects same); each first difference is F102's folded index, the unrolled `in`, or follows from them. Release instructions per warm iteration: none slower (hexiom −2.1%, go −0.6%).
- **Stopped, for step 6 or later:** `tupleconcat`/`tuplerepeat` (virtual objects; done in step 6), `tuplerichcompare` (a type field, not a table slot; returns a compare's object), `list_contains` (a list's length is mutable), `PySequence_Fast` (handles a failed call's error).

## Step 5, second part: the container and string families (`5169c24`, D84) [run]

Four rules, each a function of its own in `gen_abstract.py`, and representation as data (METACOMPILER.md "Container slots"): slots that are fields of the type (`tp_richcompare`), with their binding times from `policy_table.SLOT_ARGS`; compile-time `switch` and header statement macros; local aliases of parameters; fields of run-time objects through `TYPES` `descriptors`.
- **Generated:** `tupleobject.c:tuplerichcompare` (new), `listobject.c:list_length`, `bytesobject.c:bytes_length`, `dictobject.c:dict_length` (units `list`, `dict`, `bytes`). Hand code: `psyco_dict_length` and three registrations removed.
- **Found:** F109 (no identity shortcut in `PsycoObject_RichCompareBool`: `nan in (nan,)` False since D78), fixed; a `cparse.Decls` bug (a function head read as a struct), fixed.
- **Kept, each with its reason in the failures log:** str (the unit doesn't read; D37; PEP 393), bytes subscript (D56), list items and slices (D80, D56), range and its iterator (virtual words), `iter_iternext` (F48), `member_get`/`method_get`/`PyMember_GetOne` (not attempted). Most of what remains is representation the C doesn't state; the rest would resume at once and gain nothing over the slot call.
- **Same code and instructions:** METACOMPILER.md "Container slots": 51 units same, 36 differ, each first difference the new meta, F109's fix, or promotion-stub bytes; instructions per warm iteration: nothing slower (deltablue's first +1.0% was noise: alternate runs agree, 26.55 against 26.55).

## Step 5, numbers: long, float, bool, math (`a0c8ea9`, `28ff76f`, D83) [run]

An int's one-word form is data (`TYPES`' `word` section), and `gen_abstract.py`'s rules read it (METACOMPILER.md "Representation rules"). `gen_long.py` is retired.
- **Generated:** `long_add`, `long_sub`, `long_mul`, `long_neg`, `long_invert`, `long_bool`, `long_long` (unary +), `long_abs`, `long_true_divide`, `long_pow` (`plongobject_gen.h`, 11 functions with `_PsycoLong_Copy`); `float_float` (`pfloatslots_gen.h`). Hand code removed: `plong_bool`, `plong_pos`, `plong_abs`, `plong_true_divide`, `plong_pow`, `plong_fallback1`, `pfloat_pos`, the `RETLONG`/`RETFLOAT` and `CONVERT_TO_DOUBLE` macros.
- **Same code:** `tools/samecode.py c96c8a0 d400689`: 1 unit same, 88 differ (3,145 of 4,319 code objects same). Every first difference has a reason: a zero test of an int is `test r, r` where `plong_bool` emitted `cmp r, 0` (`integer_non_null`; nearly all, the `POP_JUMP_IF_*` on ints); F104's NULL test after the residual `float_richcompare` (99 `COMPARE_OP`s, float against int); `abs(int)` a sign test with two continuations instead of a call; `+int` the int itself (a subclass: `_PyLong_Copy`); and, after those, register renamings and data words in promotion code that hold heap addresses. A test build with the zero test made `cmp` again, against `0a38681` (before F104 and D84): 65 units same, 21 differ (4,166 of 4,231), each first difference `abs`, `+`, or data words and renamings after them.
- **Instructions per warm iteration** (release, `profile_bench.py` 1 s and 4 s, raytrace 2 s and 8 s, two runs each): `db9c68f` → `e1c85b9` (this branch before its rebase onto D85's `c96c8a0`, which changed only argument binding; the same number code): richards 168.29, 168.31 → 168.29, 168.31 M; deltablue 26.49, 27.16 → 27.12, 27.15 (its usual spread); raytrace 3,091.9, 3,139.7 → 3,082.3, 3,093.4 (noise, ±1%); go 1,063.16 → 1,063.17; hexiom 40.22, 40.33 → 40.22, 40.22; chaos 460.59, 460.61 → 461.17, 461.17 (+0.12%: F104's NULL test after each float-against-int `float_richcompare` in its hot loops, a bug fix); spectral_norm 240.32 → 240.32; nbody 252.08 → 252.08; float 662.9, 664.4 → 662.5, 665.5 (noise); scimark 1,872.03 → 1,872.03. None slower except chaos, by the fix.
- **Checked build:** generators twice, same output, the build used it; `tests/run.py` 855 pass (`long_generated.py`, 8 cases: one-digit edges, signs, zero, subclasses with and without `__abs__`/`__bool__`, errors, overflow past a word, float `+`); `compile_bounds.py` 7 checks; `tests/cpython_tests.py -j 2` 285 modules, 0 unexpected, and `--all` 371 modules, 0 unexpected.
- **Left:** `gen_float.py`'s slots, `long_richcompare`, `long_hash`, the bit operations, `// % divmod()`, bool's slots, math's `FUNC1`; each an item or a failures-log row.

## Step 6: virtual objects (`a6a687b`, `603363c`, D80) [run]

Tuple, slice and bound-method support (descriptors, forcing, direct computation, construction) is generated by `tools/metac/gen_virtual.py` from CPython's constructors and `policy_table.TYPES`; a constructor call in translated C is the virtual (METACOMPILER.md "Virtual objects").
- **Generated:** `PyTuple_New`'s, `PySlice_New`'s and `PyMethod_New`'s support; `tupleobject.c:tupleconcat` (hand `PsycoTuple_Concat` removed) and `tuplerepeat` (new). Hand code: 293 lines removed, 39 added.
- **Kept:** the list (mutable, a different representation), `PsycoTuple_Load`, `ptuple_item`.
- **Found:** F107 (codestats use after free), F108 (hand concat in a loop ran out of memory).
- **Same code:** `tools/samecode.py` A = `e0ae987` with F107's fix (`scratch` build; the plain baseline's codestats build reads freed memory), B = `603363c`: 54 units same, 31 differ (3,993 of 4,136 code objects same); `virtual_objects` runs only on B (A segfaults in `concat_in_loop`, F108). The first differences are: a constant slice now a constant (`movabs obj:slice` where A built it: `BINARY_SUBSCR` in `zipfile`, `sre_compile._mk_bitmap`, `containers.slicing`, `heap_slots`, about 25 code objects); `tuplerepeat` and `tupleconcat` generated (`argparse.format`'s `(result,) * tuple_size` is a constant tuple; `subscript_concat.concat_types`' `a + ()` with `a` unknown is the slot call where the hand code promoted and returned `a`); the rest are register and stack-offset renamings and different addresses in promotion stubs after those (`sre_parse`'s `LOAD_ATTR` copies, as part A alone showed against `031b0a6`) [inferred: read by eye in the first differences samecode prints, not classified by a tool].
- Release builds, instructions per warm iteration (`perf stat -e instructions:u`, `profile_bench.py` 1 s and 4 s, raytrace 2 s and 8 s, PYTHONHASHSEED=0), `e0ae987` → `603363c`: richards 168.31 → 168.29 M, deltablue 27.17 → 27.18, raytrace 3,120.1 → 3,110.5, go 1,064.0 → 1,064.4, hexiom 39.73 → 39.70, chaos 464.77 → 461.01 (−0.8%), pyflate 2,003.1 → 2,003.0: nothing slower beyond noise (≤ 0.04%).

## Step 7: the call path's argument binding (`e11f2a3`, D85) [run]

`ceval.c:_PyEval_EvalCodeWithName`'s binding is generated by `tools/metac/gen_call.py` into `psyfunc_gen.h`, a function the compiler runs when it compiles a call; anything it has no form for is the real call (METACOMPILER.md "Argument binding").
- **Generated:** positional arguments, defaults, `*args` (virtual, up to 15), the keyword loop, the count checks, kw-only and cells as real calls, free variables. `fncall_collect_arguments` and the translated half of `fncall_store_arguments` removed.
- **Kept core:** `fncall_init`'s refusals, `COMPUTE_DEFCOUNT`, keyword calls, inlining, recursion, tracing, genexprs, and `pmethodobject.c`'s dispatch (reasons in D85 (5)).
- **Same code:** `tools/samecode.py db9c68f e11f2a3`: 78 units same, 10 differ (4,261 of 4,291 code objects same). One difference is code, accepted as D85 (6): `arg_binding.many_extras` calls `star` with 20 extra arguments, which A compiled with a 20-item virtual tuple and B makes the real call. Every other first difference is inline promotion-stub data (heap addresses disassembled as instructions; `LOAD_METHOD`, `LOAD_ATTR` and the like) and the register choices after it [read: by eye in samecode's output, and no other copy count changes]. The binder's arrays have new sizes (`nlocals` where they were the argument count) and `PyMem_MALLOC` is pymalloc, whose pools Python objects share, so objects land at other addresses [inferred]. A check of that: the baseline built with only a heap shift (`malloc(100000)` and 300 small blocks at module init) against itself gives 59 units same, 28 differ, all promotion data of the same kind. Against `0a38681` before the rebase, two loop copies (`sre_parse._parse`, `bm_raytrace` `<listcomp>`, the latter empty) also differed by one promotion entry; after the rebase they are the same.
- Release builds, instructions per warm iteration (`perf stat -e instructions:u`, `profile_bench.py` 1 s and 4 s, raytrace 2 s and 8 s, PYTHONHASHSEED=0), `db9c68f` → `e11f2a3`: richards 168.29 → 168.29 M, deltablue 27.12 → 27.12, raytrace 3,094.3 → 3,089.7, go 1,064.5 → 1,065.1 (five runs each, alternated: means 1,065.44 → 1,065.42), hexiom 39.82 → 39.82, chaos 461.25 → 460.26, pyflate 2,002.93 → 2,002.93: nothing slower.
- **Checked build:** generation twice, same output, and the build used it; `tests/run.py` 847 pass (`arg_binding.py`, 8 new cases: defaults, `*args` empty, short and past 15 items, keyword-only defaults, wrong counts with CPython's messages, methods, closures, builtins of each `ml_flags` kind and their wrong-count errors); `compile_bounds.py` 7 checks; the seven `profile_bench.py` runs; `tests/cpython_tests.py -j 2` 285 modules, 0 unexpected, and `--all` 371 modules, 0 unexpected.
- **3.8.20** (PORT38 "Rerunning", not built): generated. The input carries 3.8's change: the keyword loop starts at `co->co_posonlyargcount` and `positional_only_passed_as_keyword` is a new real-call exit; error calls are `_PyErr_*`, `PyDict_GetItemWithError`. `*args` is the real call on 3.8: `_PyTuple_FromArray` (3.8's replacement for `PyTuple_New` and the fill loop) has no form, so every call to a `*args` function would be residual until a rule reads it (failures log). Needed one rule: a local only ever assigned what has no form (3.8's `tstate = _PyThreadState_GET()`, a GNU statement expression, read only by error calls) is not declared and reading it ends the binding; 3.7's output is byte-identical.

## Step 8, front end: oparg-dependent stack effects (`99d9b26`, D73) [run]

The reader expresses stack effects that depend on oparg as 3.12's DSL does (`values[oparg]`, `step if (oparg == 3)`, `unused[oparg - 1]`), derived from the C; METACOMPILER.md "`ceval.c` reader".
- **Of the 18 opcodes step 2 refused for oparg: 17 read, 5 generated** (`LIST_APPEND`, `MAP_ADD`, `BUILD_TUPLE`, `BUILD_LIST`, `BUILD_SLICE`; their hand-written cases left `pycompiler.c`, same calls in the same order, and `tools/samecode.py` finds the same machine code everywhere). `CALL_FUNCTION_EX` is refused: CPython 3.7 leaks `kwargs` on one error path. The 12 read but not generated need a residual-call rule with effect flags (step 3: `SET_ADD`, `BUILD_SET`, `BUILD_STRING`), loops with error exits in the back half (`BUILD_MAP`, `BUILD_CONST_KEY_MAP`, the four `BUILD_*_UNPACK`), `ceval.c` helpers translated (`UNPACK_*`'s `unpack_iterable`), or a `switch` on oparg folded (`FORMAT_VALUE`); each is in the failures log.
- **Calls read, not generated:** `CALL_FUNCTION`, `CALL_FUNCTION_KW`, `CALL_METHOD`, their effects from `call_function`'s own body. They are core (step 7).
- **Totals:** read 74 of 119 (was 51), generated 49 (was 44); refused 45: core 18, control flow 9, shared code 8, stack 5, falls through 4, preprocessor 1.
- **Policy table:** the five entries' `ni` now name `gen_ceval.py:translate` (`policy.py` fails the build while they name the removed hand cases). Their `virtual` items (`BUILD_TUPLE`, `BUILD_LIST`, `BUILD_SLICE`) are not read: the generator finds the constructor by PATTERNS E's rule, and `BUILD_LIST`'s length limit is inside `PsycoList_New`. Step 6 should read those items or drop them, so the table and the rule can't disagree unnoticed.
- **Left for step 8's back half:** branches on run-time values as condition codes (`UNARY_NOT`, `POP_JUMP_IF_*`), loops with error exits, `cmp_outcome`'s switch, jumps.

## Step 8, back half: branches and jumps (`a907150`, D79) [run]

The control-flow cases are generated with step 4's rules: METACOMPILER.md "`ceval.c` reader" (the restructuring rules, the refusals table, the back half's additions) and the failures log.
- **Generated: 9 more, 59 of 119** (`UNARY_NOT`, `POP_JUMP_IF_FALSE`, `POP_JUMP_IF_TRUE`, `JUMP_IF_FALSE_OR_POP`, `JUMP_IF_TRUE_OR_POP`, `JUMP_FORWARD`, `JUMP_ABSOLUTE`, `DELETE_FAST`, `SET_ADD`); their hand cases left `pycompiler.c` (104 lines out, 37 in: the two core macros). Read 82 (was 74); refused 37: core 18, shared code 8, stack 5, falls through 4, control flow 2 (`GET_ANEXT`, `WITH_CLEANUP_FINISH`); `DELETE_DEREF` is read but needs cells in the vocabulary.
- **Reused from step 4:** `gen_abstract.int_condition` (P05, now shared; gen_abstract's output unchanged) and `Translator.flags` for residual calls. Core: `JUMP_IF_CC` and `JUMPED()` in `pycompiler.c`. Policy: rules `singleton_test_generic_path` and `bool_select`, and `forget before_branch` on all four conditional jumps.
- **Same code:** `tools/samecode.py baseline-3.7 a907150`: 83 units same, 0 differ (4,107 code objects). Release instructions per warm iteration on the seven benchmarks: no change (within 0.4%, the same machine code).
- **Left:** `COMPARE_OP` (needs `cmp_outcome` translated: a compile-time `switch`, an object identity compare, a loop), `BUILD_SET`, `BUILD_MAP`, `BUILD_CONST_KEY_MAP`, the four `BUILD_*_UNPACK` (loops with error exits: the loop rule being added to gen_abstract), `BUILD_STRING` (an array argument to a residual call), `FORMAT_VALUE` (the hand case is one residual call, judgement), `UNPACK_*` (`unpack_iterable`), cells (`LOAD_CLOSURE`, `LOAD_DEREF`, `STORE_DEREF`, `DELETE_DEREF`), imports.

## Step 8, builders: loops with error exits (`5c8a4fb`, D81) [run]

The back half translates C `for` loops by `gen_abstract.loop_rule` and keeps C ints of the compiler (METACOMPILER.md "`ceval.c` reader", failures log).
- **Generated: 5 more, 64 of 119** (`BUILD_SET`, `BUILD_MAP`, `BUILD_LIST_UNPACK`, `BUILD_SET_UNPACK`, `LOAD_CLOSURE`); their hand cases and `cimpl_build_set_unpack` left `pycompiler.c`. Read still 82.
- **Same code:** `tools/samecode.py baseline-3.7 5c8a4fb`: 80 units same, 4 differ (4,136 of 4,150 code objects); every first difference is a `BUILD_LIST_UNPACK` (8) or `BUILD_SET_UNPACK` (6), one residual call per part where the hand case made one call of `cimpl_build_unpack`/`cimpl_build_set_unpack` over a virtual tuple of the parts. All seven benchmarks are the same code.
- **Instructions per warm iteration:** not measured; the seven benchmarks emit the same machine code (samecode above), so they can't have changed.
- **Checked build:** generators twice, same output, and the build used it; `tests/run.py` 772 pass (two new cases in `generated_opcodes.py`: each builder at several opargs with result types, and error exits at the first, middle and last item); `tests/compile_bounds.py` 7 checks; the seven `profile_bench.py` runs; `tests/cpython_tests.py -j 2` 285 modules, 0 unexpected.
- **3.8.20** (PORT38 "Rerunning", not built): 65 generated; the five new cases' text is identical to 3.7's.
- **Left:** `COMPARE_OP`, `BUILD_CONST_KEY_MAP`, `BUILD_MAP_UNPACK*`, `LOAD_DEREF`, `STORE_DEREF`, `DELETE_DEREF`, `BUILD_STRING`, `FORMAT_VALUE`, `BUILD_TUPLE_UNPACK*` (P22's shortcut); reasons in the failures log.

## Step 8, helpers and cells: `COMPARE_OP`, `LOAD_DEREF` (`4c57d97`, D82) [run]

`ceval.c`'s static helpers are inlined by `ceval_inline.py`; type tests use only what Ni knows and resume in a generated run-time copy of the arm; cell fields are read through Ni's descriptors (METACOMPILER.md "`ceval.c` reader", failures log).
- **Generated: 2 more, 66 of 119** (`COMPARE_OP`, `LOAD_DEREF`); their hand cases and `cimpl_exc_match` left `pycompiler.c`, which now includes `pycompiler_copies_gen.h` (`cimpl_cmp_outcome_PyCmp_EXC_MATCH`). Read still 82.
- **Same code:** `tools/samecode.py baseline-3.7 4c57d97`, in two steps: COMPARE_OP alone, 1 unit same, 85 differ, 3,766 of 4,241 code objects same; every first difference has a reason: 421 an `except K:` on a known class calling `PyErr_GivenExceptionMatches` after the compile-time check (was `cimpl_exc_match`), the run-time copy's new name where Ni knows too little, `except 42:` raising at compile time (`compare_ops.py`), and the rest shifted jump offsets and inline data after shorter code. Then LOAD_DEREF on top: 3,905 of 4,241 same, 330 first differences each `test r, r` for `cmp r, 0` in the unbound test, the rest knock-ons.
- **Instructions per warm iteration** (release): `77bde48` → `4c57d97`: richards 168.31 → 168.29 M, deltablue 26.69 → 26.59, raytrace 3,094.5 → 3,103.1 (four runs each: 3,087–3,094 → 3,090–3,103, +0.1% on the means, within raytrace's spread; its emitted code differs only in addresses and `test` for `cmp`, the same instructions), go 1,064.7 → 1,063.6, hexiom 39.67 → 39.67, chaos 461.07 → 460.71, pyflate 2,003.05 → 2,003.05: nothing slower beyond noise.
- **Checked build:** generators twice, same output, the build used it; `tests/run.py` 825 pass (`compare_ops.py`, 19 cases); `compile_bounds.py` 7 checks; `tests/cpython_tests.py -j 2` 285 modules, 0 unexpected; the seven `profile_bench.py` runs.
- **3.8.20** (not built): 67 generated; the two new cases are 3.7's text but one effect flag (PORT38 item 4).
- **Left:** `STORE_DEREF`, `DELETE_DEREF`, `IMPORT_NAME`, `IMPORT_FROM`, `BUILD_CONST_KEY_MAP`, `BUILD_MAP_UNPACK*`, `BUILD_STRING`, `FORMAT_VALUE`, `BUILD_TUPLE_UNPACK*`; reasons in the failures log.

## Step 9: mechanism detectors (`36b5beb`, D75) [run]

`tools/metac/mechanisms.py` answers capability questions from the target's source and maps the answers to one snippet per mechanism in `ni/mechanisms/`; `generate.py` writes `mechanism_{entry,tracing,frames}_gen.h`. Details, the interfaces and the detector table: METACOMPILER.md "Mechanisms".
- **3.7:** entry `frame_eval_hook`, tracing `tstate_use_tracing`, frames `frame_object`. Each needed a snippet: the hook and its installation; the `use_tracing` test at run time and in emitted code; `ni_frame_t` and eight accessors for the frame chain (field accesses on 3.7, a different structure on 3.11+). The core (`eval_hook.c:ni_entry_point`, `NiCode_Run`, `frames.c`, `pycompiler.c:cimpl_new_frame`, `psyfunc.c:tracing_on`) calls only those.
- **Other versions, detectors only** (`tests/mechanisms.py`): 3.8.20 gives 3.7's answers; 3.11.11 a hook given `_PyInterpreterFrame` (vectorcall unusable: specialised calls would bypass it), CFrame tracing, CFrame frames; 3.12.9 vectorcall with watchers, `sys.monitoring`, CFrame frames; 3.13.2 the same but the innermost frame in `tstate->current_frame`. Those snippets are stubs that `#error` "not written yet".
- **Checks:** generation twice, same output, and the build used it; answers not in `VARIANTS` fail generation and remove the headers (`tests/mechanisms.py`); `tools/samecode.py 2d2d3d5 36b5beb` 83 units same, 0 differ (4,089 code objects); checked build `tests/run.py` 749 pass (`tracing` 9), `compile_bounds.py` 7, `cpython_tests.py -j 2` 285 modules 0 unexpected, `smoke.py` same, `v_mem_mov` 90,144 cases.
- **For 3.8:** no entry, tracing or frames work. Since D77 two mechanism stubs stop a 3.8 build (below), with the policy table's shapes [inferred, not built].

## Step 9, the rest of the mechanisms (`f254b5d`, D77) [run]

Five more mechanisms go through step 9's machinery: where eval_breaker is and what ceval does on it, the current thread state, the recursion count, the exception state, the inline store's GC test. Seven `VARIANTS` entries (two pairs whose code is half in `pyruntime.c`); detector table and interfaces in METACOMPILER.md "Mechanisms".
- **3.7:** each mechanism's code moved into a snippet behind a narrow interface, and the layout entries it read (`PyThreadState`'s three fields, `PyGC_Head`, both `RUNTIME` fields) moved with it. `pyruntime.c` is now only the snippets' home.
- **Other versions, detectors only:** 3.8 picks two stubs (the handler with `signals_pending`, the GC test on `_gc_next`); 3.11 the per-interpreter breaker with a copy of `eval_frame_handle_pending`, `recursion_remaining`, the handled exception as one value, `_gc_next`; 3.12 the thread state as a thread-local, `_Py_HandlePending` (not exported), `py_recursion_remaining`, `current_exception`; 3.13 eval_breaker per thread and an exported handler.
- **Checks:** generation twice, same output, and the build used it; `tools/samecode.py 59c5fba f254b5d`: 83 units same, 0 differ (4,089 code objects); checked build `tests/run.py` 749 pass, `compile_bounds.py` 7, `cpython_tests.py -j 2` 285 modules 0 unexpected; `tests/mechanisms.py` 0 failed (every mechanism has an expected row per version).
- **Not done:** the handler copy could come from the `ceval.c` reader (the loop top on 3.7 and 3.8, a function on 3.11 and 3.12); on 3.13 it is exported.

## Layouts: D19's stage 0 (`ee76274`, `9869ad9`, D76) [run]

The CPython fields Ni relies on are a list in the policy table (`LAYOUTS`, `RUNTIME`); `tools/metac/gen_layouts.py` checks them against the target's headers and writes `layouts_gen.h` and `runtime_layouts_gen.h` (offsets by `offsetof`, a `_Static_assert` per assumption). Details: METACOMPILER.md "Stage 0: layouts".
- **3.7:** 57 fields (55 in 22 structs, 2 in `_PyRuntime`); 43 field descriptors (`CPY_FIELD`), one array, 22 backend offsets and `pyruntime.c`'s two runtime fields use them; five private struct copies left Ni's code. Hand-written: Ni's own structs, the index chain and flags, virtual-only slots, mechanism snippets, and two mechanisms (the eval-breaker handler, the inline store's GC test).
- **Checks:** `tools/samecode.py baseline-3.7 ee76274`: 83 units same, 0 differ (4,089 code objects); generation twice, same output, and the build used it; mutation tests fail generation or the build naming the field.
- **Other versions:** 3.8.20 one field changed (`PyGC_Head`), 3.11.11 six, 3.12.9 and 3.13.2 fourteen (thirteen fields and `PyASCIIObject.state`'s bits); the table in METACOMPILER.md is the input for those versions' work, and says which are mechanisms for D75's detectors.

## 3.8 dry run (`2a71def`, PORT38.md) [run]

Every generator run against 3.8.20's source with a 3.8.20 interpreter (uv's), Ni not built. `generate.py` stops at the policy table, as designed: 128 shapes differ and 2 keys are gone. Past it, after seven general rules that read 3.8 (`2a71def`; 3.7's headers byte-identical, `tests/run.py` 755 pass): `gen_long`, `gen_float` and `gen_layouts` give 3.7's text; `gen_ceval` reads 83 of 120 cases, every effect equal to 3.8's `dis.stack_effect`, and generates 60, of which 58 are 3.7's text byte for byte, `MAP_ADD` takes key and value in 3.8's order and `ROT_FOUR` is new: the opcode changes reached Ni through the input alone. `gen_abstract` translates 3.8's new sequence and mapping checks, but loses four index paths to a late `#undef PyIndex_Check` (a generator limit). Mechanisms pick 3.7's snippets but two stubs. What a 3.8 build needs, with estimates (about 3–5 days, mostly the core's block stack for 3.8's finally opcodes): PORT38.md "What 3.8 needs".

## Risks

- **Step 4 is the method.** No generator has yet translated a function with general control flow and ownership; Psyco's hand translations make these choices by intuition. If token rules and then pycparser both fail on `abstract.c`, the phase needs re-planning there, not later.
- **Policy hidden in translations.** Hand translations carry Ni's own decisions inline (`SLOT_OR_GOTO`, `need_reference`, D59's forgetting). The inventory (step 0) must find each and move it to the policy table or the core, or the generated code will lack it.
- **Judgement shortcuts.** Some hand code is faster than a faithful translation (one-digit int paths, P26's float compare whitelist). Same code shows where; each becomes a policy entry or is accepted with its cost (D29).
- **`ceval.c` irregularity.** Shared labels, fall-through and macros between cases. The reader lists what it can't read rather than guessing (GOAL "breaks loudly").
- **Build-time generation** needs the matching CPython source at build time; a missing or different tarball must fail the build, not fall back.
