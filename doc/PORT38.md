# Porting to 3.8: a dry run of generation

GOAL.md says 3.8 is the first test that versions differ only in the input (D67). Every generator runs against 3.8.20's source with `--past-policy`, without building Ni for it. Figures at `962af09`; the dry-run narrative is in git (`git show e404100:doc/PORT38.md`). [run] unless marked.

## What generates, 3.8.20 against 3.7.17

`generate.py --past-policy` writes all 32 headers (the same set as 3.7.17), the same text twice. A "resume" is a path that calls CPython's C at run time.

| Area (header) | 3.7.17 | 3.8.20 | Text against 3.7 |
|---|---|---|---|
| Opcodes (`pycompiler_gen.h`) | 75 of 119 | 72 of 120 | 68 cases identical; `BUILD_TUPLE_UNPACK`, `COMPARE_OP` differ only in effect flags; `MAP_ADD` differs (key before value); `ROT_FOUR` new; lost `BUILD_TUPLE_UNPACK_WITH_CALL`, `BUILD_MAP_UNPACK_WITH_CALL`, `IMPORT_FROM`, `FORMAT_VALUE` |
| Opcode helper copies | 11 | 6 | the 6 the same; the 5 lost are the lost opcodes' copies |
| Abstract | 52 | 52 | real 3.8 C translated (`PyMapping_Size`, `PySequence_Get/Set/DelItem` refuse the other protocol, a resume each); 1 call loses effect flags |
| Containers | 15 | 15 | 13 residual calls lose `CfNoPython \| CfNoBlock`; `member_get` gains `READ_RESTRICTED` and resumes at `PySys_Audit` |
| Numbers (long, float slots) | 12 + 11 | 12 + 11 | real changes translated (`long_float`, `PyFloat_AsDouble` takes `nb_index`); 16 calls lose effect flags (3.8's `PyErr_NoMemory` and others reach `_PyErr_SetObject`, which the `PyErr_SetObject` seed doesn't cover, and its decref [run]); `long_invmod` skipped |
| Builtins | 20 | 11 | 7 clinic wrappers resume at once (`_PyArg_CheckPositional`), so 6 `_impl`s aren't reached; `id` refused (`&&`); `pow`, `pow_impl` skipped (keywords) |
| Math | 12 | 12 | same |
| Call binding | 1 | 1 | `co_posonlyargcount` read from the C; `*args` is the real call (`_PyTuple_FromArray`) |
| Virtuals | 3 + 3 | 3 + 3 | same but 3 constructor calls lose effect flags |
| Mechanisms | 10 snippets | 8 snippets, 2 stubs | `ceval_loop_top_signals` and `store_gc: gc_next` are `#error` stubs (needs 6, 7) |
| Layouts | 51 fields | 51 fields | same but four comments |
| Policy table | 0 problems | 131 | 129 shapes differ, 2 keys missing (need 1) |
| donecheck | generated 212, reasoned 228, none 0 | generated 195, reasoned 224, none 0 | 4 fewer reasoned: `SETUP_LOOP`, `SETUP_EXCEPT`, `BREAK_LOOP`, `CONTINUE_LOOP` have no 3.8 `TARGET` |

In all, 35 lines differ only by `CfNoPython`/`CfNoBlock` (never `CfPure`): cause 3 below.

## 3.8 refusals by cause

Most first; "blocks" counts definitions lost from the generated headers, or degraded to a resume.

| # | Cause | Blocks on 3.8 | What would fix it |
|---|---|---|---|
| 2 | **`tstate` parameters in helper copies**: `helper_copy` (D88) copies only helpers that take objects. | 8: `BUILD_TUPLE_UNPACK_WITH_CALL`, `BUILD_MAP_UNPACK_WITH_CALL`, `IMPORT_FROM` and 5 copies | a `tstate` parameter as the running thread's; `import_from` also needs its `goto` |
| 3 | **Effect rules on 3.8**: the walk goes round `SEEDS`/`ASSUMPTIONS` through `_PyErr_*`, and follows `_PyObject_ASSERT`'s `NDEBUG`-off branch. | 0 refused; at least 35 calls lose `CfNoPython \| CfNoBlock`, so known classes are dropped across them (F55) | need 4 |
| 4 | **Keyword calling convention**: `pow` is `METH_FASTCALL \| METH_KEYWORDS`. | 2: `builtin_pow`, `builtin_pow_impl` | a `METH_ARGS` entry with the positional count only |
| 5 | **An error test inside `&&`**: `if (x && PySys_Audit(...) < 0)`. | 1: `builtin_id`; `member_get` resumes on its restricted path | read `A && f() < 0` as nested tests |
| 6 | **A `switch` arm that can't be taken** in `FORMAT_VALUE`. | 1: `FORMAT_VALUE` | an arm outside a compile-time value's range as dead |
| 7 | **Another file's function with no form** (`_PyTuple_FromArray`) in the binding. | 0 refused; `*args` binding is the real call | inline a `TYPES` constructor plus item stores |
| 8 | **Block stack and the finally model** (D100). | 0 generated (core on both); stops a build | need 9 |
| 9 | **Vectorcall** (PEP 590). | 0 generated; 5 policy entries and 3 `TYPES` shapes to re-read | needs 1, 11 |
| 10 | **Positional-only parameters** (PEP 570). | 0 | nothing |

## What a 3.8 build needs

| # | What | Kind |
|---|---|---|
| 1 | **Policy table re-read:** 128 entries against their 3.8 C, a second shape each; `format_kwargs_mapping_error` → `format_kwargs_error`, `sys_getframe` → `sys__getframe_impl`; `small_presized` from `extra` to `shortcut`. Mostly mechanical; 33 carry policy items. A `policy.py --diff OLD_SRC` would make it a reading job. | rules (data) |
| 2 | **`PREDICTED(op);` left out of a `TARGET`'s shape** (optional: removes artefacts from 1). | generator |
| 4 | **Effect rules on 3.8:** macros as compiled (or `Py_NO_RETURN` → fatal), seeds on the `_PyErr_*` internals (needs 5). | rules |
| 5 | **Policy keys absent in some sources** (D100), e.g. a shape `None` for "no such definition". | rules (checker) |
| 6 | **Mechanism `ceval_loop_top_signals`:** 3.7's snippet plus 3.8's `signals_pending`, `handle_signals`, `make_pending_calls`, `_PyRuntime.ceval`; or generate it from the loop top (D77 (6)). | mechanism |
| 7 | **Mechanism `store_gc: gc_next`:** compare `_gc_next` (16 bytes before the object) with 0. | mechanism (backend) |
| 8 | **`pyruntime.c`'s internal header** (`internal/pycore_pystate.h`): a detector for where `_PyRuntime` is declared. | mechanism |
| 9 | **The block stack and finally opcodes in the core:** `mergepoints.c` and `pycompiler.c` name opcodes 3.8 lacks; 3.8 needs `BEGIN_FINALLY`, `CALL_FINALLY`, `POP_FINALLY`, `END_ASYNC_FOR`, the new `END_FINALLY`/`WITH_CLEANUP_*` stacks, and `ROT_FOUR` in the whitelist. Without `#if` is D100. The biggest item. | core |
| 10 | **Dicts' shared empty keys:** check the inline store, keys guards and `PsycoDict_*` against 3.8's empty dicts [inferred risk]. | core |
| 11 | **Re-read the hand metas whose C changed:** `PsycoObject_Call` and the call path (vectorcall), `pfloat_richcompare` (F104), list item and subscript, `PyDict_New`/`SetItem`. | translated |
| 12 | **3.8 build and tests:** an interpreter to build against, `cpython_tests.py`'s 3.8 module lists, `check.sh` for a second version, tests that depend on 3.7 bytecode. | tooling |
| 14 | **The refusal causes above:** 2, 4, 5, 6 (1, `_PyArg_CheckPositional`, is gen2's `ARGUMENT_CHECKS`). | generator, rules |

(3, macros as in force where used, and 13, `--past-policy`, are done.) Not needed [inferred]: another entry mechanism (the PEP 523 hook still sees every call), anything for positional-only parameters, new layouts.

Order: 8, 7, 6 and 9 for a compile; 1 (with 2) for generation; then 11 and 12 for same behaviour; 4 and 5 before step 5's families or computing D53's list. Roughly 3–5 days, most of it item 9.

## Rerunning

`generate.py --python ~/.local/share/uv/python/cpython-3.8.20-linux-x86_64-gnu/bin/python3.8 --src ~/.cache/ni/cpython/Python-3.8.20 --out DIR --past-policy -v`, and the same for 3.7.17 without `--src`/`--past-policy`. Compare per header with the version string masked, `gen_ceval`'s output case by case (`      case X: {` starts a case), and the `-v` reports entry by entry. `donecheck.py --gen DIR` runs the done test on that run's report. `python3 tools/metac/ceval_reader.py SRC` and `policy.py --src SRC` run alone.
