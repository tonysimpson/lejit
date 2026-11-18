---
written: 2026-11-18
status: dry run of generation against CPython 3.8.20 at `2a71def`, rerun at `3186aca` ("Now"); Ni not built for 3.8
---
# Porting to 3.8: a dry run of generation

GOAL.md says 3.8 is the first test that versions differ only in the input (D67). This is a dry run of every generator against 3.8.20's source, without building Ni for it: what generates the same as on 3.7.17, what differs and why, what fails, and what a 3.8 build still needs. [run] unless marked.

## How it was run

- **Source:** `~/.cache/ni/cpython/Python-3.8.20` (the cached tarball, hash in `cpython_sha256.txt`).
- **Interpreter:** none in pyenv, but uv already had one: `~/.local/share/uv/python/cpython-3.8.20-linux-x86_64-gnu/bin/python3.8` (python-build-standalone, built with Clang, CFLAGS with `-DNDEBUG`). Nothing was installed. It gave `dis.stack_effect`, `sysconfig`'s CFLAGS and the installed `pyconfig.h`, so nothing was blocked for want of an interpreter.
- **`generate.py --python <3.8> --src <3.8>`** stops at the policy check, as designed (below). To see the generators past it, a scratch driver built the `cslots.Target` the same way and called each generator's `build(target)` on its own, catching `GenError` and exceptions. The baseline was `generate.py` on 3.7.17 at `7088760`. Outputs were compared header by header, and the ceval cases case by case.

## Now (`962af09`, 2026-11-18)

`generate.py --python <uv 3.8.20> --src ~/.cache/ni/cpython/Python-3.8.20 --out DIR --past-policy` runs to the end and writes all 32 headers (the same set as 3.7.17), twice with the same output. Two generator bugs stood in the way: F112 (`gen_ceval`'s label copies, `96eeba2`) and, behind it, F113 (`gen_virtual` reading `PyTuple_New`, `24dd41d`). A third, found by reading the output, would have stopped a 3.8 compile: F115 (`ceval_BUILD_MAP_UNPACK_0` named `tstate`, `962af09`). `--past-policy` (`a2def0d`, item 13 below as a flag) prints the policy table's problems instead of stopping; `donecheck.py` has the same flag (`3186aca`). No fix changes 3.7.17's output (byte-identical headers and report before and after each). [run]

### What generates, 3.8.20 against 3.7.17

Counted from each generator's report (`-v`); "same text" compares the headers with the version string masked. A "resume" is a path that calls CPython's C at run time.

| Area (header) | 3.7.17 | 3.8.20 | Text against 3.7 |
|---|---|---|---|
| Opcodes (`pycompiler_gen.h`) | 75 of 119 | 72 of 120 | 68 cases identical; `BUILD_TUPLE_UNPACK`, `COMPARE_OP` differ only in effect flags; `MAP_ADD` differs (real: key before value); `ROT_FOUR` new; lost `BUILD_TUPLE_UNPACK_WITH_CALL`, `BUILD_MAP_UNPACK_WITH_CALL`, `IMPORT_FROM`, `FORMAT_VALUE` |
| Opcode helper copies (`pycompiler_copies_gen.h`) | 11 | 6 | the 6 the same (`ct_format_exc_check_arg` and `unbound_local_error` included, F112; `ceval_BUILD_MAP_UNPACK_0` since F115); the 5 lost are the lost opcodes' copies |
| Abstract (`pabstract_gen.h`) | 52 | 52 | real 3.8 C translated: `PyMapping_Size` and `PySequence_Get/Set/DelItem` refuse the other protocol (a slot read with a resume each); `sequence_repeat` and `PyObject_*Item` read `PyIndex_Check` expanded (D78 (8)), same meta; `PyIndex_Check` itself is a 3.8 function, skipped; 1 call loses effect flags |
| Containers (tuple 6, list 1, dict 1, bytes 1, str 3, `structmember` 1, descriptors 1, object 1) | 15 | 15 | tuple, str, `PyMember_GetOne`: 13 residual calls lose `CfNoPython \| CfNoBlock`; `member_get` gains 3.8's `READ_RESTRICTED` arm and resumes at `PySys_Audit` |
| Numbers (`plongobject_gen.h`, `pfloatslots_gen.h`) | 12 + 11 | 12 + 11 | real changes translated: `long_float` checks `-1.0` and an error (a scalar copy), `PyFloat_AsDouble` takes `nb_index`; 16 residual calls lose effect flags; `long_invmod` (3.8's `pow(x, -1, m)`) skipped (`k_mul` calls itself) |
| Builtins (`pbltinmodule_gen.h`) | 20 (13 builtins, 7 `_impl`s) | 11 | the 7 clinic wrappers of `delattr`, `divmod`, `getattr`, `hasattr`, `isinstance`, `issubclass`, `setattr` are generated but resume at once (`_PyArg_CheckPositional`), so their 6 `_impl`s aren't reached; `id` refused (`&&`); `pow` and `pow_impl` skipped (`METH_FASTCALL \| METH_KEYWORDS`); `abs`, `callable`, `hash`, `len` the same text as 3.7 |
| Math (`pmathmodule_gen.h`) | 12 | 12 | not a C change (`math_1_to_whatever` is the same): all 12 metas' scalar copies start at `if (x == -1.0 && PyErr_Occurred())`, and the metas no longer test `PsycoFloat_AsDouble`'s result for NULL (F114, open) |
| Call binding (`psyfunc_gen.h`) | 1 | 1 | real changes translated: `co_posonlyargcount` in the keyword loops (positional-only names passed by keyword are the real call); `*args` is the real call (`_PyTuple_FromArray`, another file's function with no form) |
| Virtuals (fields, tuple, slice, method) | 3 + 3 | 3 + 3 | the same but 3 constructor calls (`PyTuple_New`, `PySlice_New`, `PyMethod_New`) lose effect flags |
| Mechanisms (10 headers) | 10 snippets | 8 snippets, 2 stubs | the 8 the same (but the header named in the comment); `eval_breaker_runtime: ceval_loop_top_signals` and `store_gc: gc_next` are `#error` stubs, as before (items 6, 7) |
| Layouts (`layouts_gen.h`, `runtime_layouts_gen.h`) | 51 fields | 51 fields | the same but four comments naming `Include/cpython/` headers |
| Policy table (`policy.check`) | 0 problems | 131 | 129 shapes differ, 2 keys missing (`format_kwargs_mapping_error`, `sys_getframe`); item 1 |
| donecheck (`--past-policy`) | generated 212 (6,228 lines), reasoned 228, none | generated 195 (5,980), reasoned 224, none | reasoned: the 4 fewer are `pycompiler.c`'s `SETUP_LOOP`, `SETUP_EXCEPT`, `BREAK_LOOP`, `CONTINUE_LOOP`, which 3.8's `ceval.c` has no `TARGET` for (so they mirror nothing); generated: the 17 lost above |

Effect flags in all: 35 lines differ only by `CfNoPython`/`CfNoBlock` (`CfPure` never), in 10 headers, plus the calls in 3.8's new code. The cause is the one found at `2a71def` (effect rules, below: the walk goes round the seeds through `_PyErr_*`, and reads `_PyObject_ASSERT`'s debug branch); since D92 the flags reach the generated code, so it now shows in the text.

### 3.8 refusals by cause

Each cause with what it blocks on 3.8 that 3.7 generates, most first. "Functions" counts definitions lost from the generated headers (`pycompiler_copies_gen.h`'s copies included), or degraded to a resume where noted.

| # | Cause | Blocks on 3.8 | What would fix it |
|---|---|---|---|
| 1 | **`_PyArg_CheckPositional` has no policy entry** (QUESTIONS 49: the table can't name a definition 3.7 lacks). 3.8's clinic wrappers check the count with it and read `args[i]` themselves, where 3.7 called `_PyArg_UnpackStack`. | 13: 7 wrappers resume at once (`delattr`, `divmod`, `getattr`, `hasattr`, `isinstance`, `issubclass`, `setattr`), so 6 `_impl`s are never reached | QUESTIONS 49's absent-in-some-sources shape, then `('other', 'unpack_arguments')` on it (D86 (3) already reads that form) |
| 2 | **Thread-state parameters in helper copies**: rule `helper_copy` (D88) copies a `ceval.c` helper for run time only if it takes objects; 3.8's take `PyThreadState *tstate` first. (`ct_copy`, the compile-time copy, already drops it.) | 8: `BUILD_TUPLE_UNPACK_WITH_CALL` (`check_args_iterable`), `BUILD_MAP_UNPACK_WITH_CALL` (`format_kwargs_error`), `IMPORT_FROM` (`import_from`, which also has a `goto`), and their 5 copies | a `tstate` parameter as the running thread's (`_PyThreadState_GET()` in the run-time copy, or `untstate`'s rule); `import_from` also needs its `goto` |
| 3 | **Effect rules on 3.8** (item 4): the walk goes round `SEEDS`/`ASSUMPTIONS` through the `_PyErr_*` internals, and follows `_PyObject_ASSERT`'s `NDEBUG`-off branch. Needs QUESTIONS 49 for the seeds. | 0 refused; at least 35 residual calls in 10 headers lose `CfNoPython \| CfNoBlock` (tuple concat and repeat, `PyErr_NoMemory`, `PyUnicode_Concat`/`Contains`, the three virtual constructors, `PyLong_From*LongLong`…), so known classes are dropped across them (F55) | item 4 |
| 4 | **Keyword calling convention**: `pow` became `METH_FASTCALL \| METH_KEYWORDS` (`_PyArg_UnpackKeywords`); `METH_ARGS` has no entry, as Ni passes no keyword names to a meta. | 2: `builtin_pow`, `builtin_pow_impl` | a `METH_ARGS` entry for it with no keywords (the positional count only), keyword calls the fallback |
| 5 | **An error test inside `&&`** (3.8's audit hooks): `if (x && PySys_Audit(...) < 0)`. | 1: `builtin_id`; and `member_get` gains a resume at `PySys_Audit` on its restricted path | read `A && f() < 0` as `if (A) { if (f() < 0) error }` |
| 6 | **A `switch` arm that can't be taken**: `FORMAT_VALUE` gained `case FVC_NONE` and a `default:` raising SystemError and jumping to `error`. | 1: `FORMAT_VALUE` | the reader reads a `switch` on a compile-time value's bits, an arm outside its range as dead (D88's note) |
| 7 | **Another file's function with no form** in the binding: `_PyTuple_FromArray(args + n, argcount - n)` replaced `PyTuple_New` and the fill loop. | 0 refused; `*args` binding is the real call | inline a source function that is a `TYPES` constructor plus item stores (failures log) |
| 8 | **Block stack and the finally model** (QUESTIONS 48, item 9). | 0 that 3.7 generates (they are core on both): `CALL_FINALLY`, `POP_FINALLY`, `BEGIN_FINALLY`, `END_ASYNC_FOR` refused, and Ni's core has no case for them; 4 core cases name opcodes 3.8 lacks. Stops a build, not generation | item 9 |
| 9 | **Vectorcall** (PEP 590). | 0 generated functions; 5 policy entries (`PyObject_Call`, `PyCFunction_Call`, `_PyFunction_FastCallDict`, `PyFunction_NewWithQualName`, `PyMethod_New`) and 3 `TYPES` shapes to re-read, and hand code (items 1, 11) | items 1, 11 |
| 10 | **Positional-only parameters** (PEP 570). | 0: the binding is generated with `co_posonlyargcount` read from the C | nothing |

Not a refusal but a 3.8-only generator bug: F114 (math's error test, row above; cause not found).

Also skipped on 3.8 only, with nothing lost: `PyIndex_Check` and `_PyUnicode_CheckConsistency` (3.8 defines them as functions; no Ni declaration), `long_invmod` (new).

## Summary (at `2a71def`)

| Stage | Before `2a71def` | After `2a71def` | Against 3.7.17 |
|---|---|---|---|
| `policy.check` (gate) | 154 problems | 130 (128 shapes, 2 missing keys) | 0 problems |
| `gen_long` | exception (`no struct PyNumberMethods`) | 5 generated | same text; two skip reasons differ (real C change) |
| `gen_float` | the same exception | 8 generated | same text |
| `gen_ceval` (reader) | exception (`case TARGET(op):`) | 83 of 120 read, 37 refused; every effect equals 3.8's `dis.stack_effect` | 82 of 119 read |
| `gen_ceval` (back half) | – | 60 generated | 58 cases identical, `MAP_ADD` differs (real), `ROT_FOUR` new (real) |
| `gen_layouts` | 51 fields | 51 fields | same but four header paths in comments |
| `gen_abstract` | `GenError` (`pycore_pystate.h` not found) | 49 generated | real C changes generated; 4 functions lose their index path (generator limit) |
| mechanisms | 10 headers | 10 headers | 8 variants the same; 2 stubs selected, as D77 expected |
| `effects.py` (tool) | exception (`no struct PyTypeObject`) | runs | 25 more `cannot_block` disagreements and more cautious flags (generator limits) |

So, with seven small general rules (below), every generator runs on 3.8's source and the generated text is 3.7's except where CPython's C changed. The build is still stopped by the policy table (every changed definition has to be read again) and would then stop at two mechanism stubs, `pyruntime.c`'s include and the core's block-stack opcodes.

## Per generator

### Policy table (`policy.py`, run first by `generate.py`)

130 problems after `2a71def`: 128 shapes differ, 2 keys are missing. Classified by the token difference (a scratch script comparing each definition in both trees):

| Class | Count | With policy items | What |
|---|---|---|---|
| Thread-state argument only | 27 | 7 | 3.8 passes `tstate` into the error functions and ceval.c's helpers: `PyErr_Format(…)` → `_PyErr_Format(tstate, …)`, a `PyThreadState *tstate` parameter added, `PyThreadState_GET` → `_PyThreadState_GET`, `STACKADJ(-n)` → `STACK_SHRINK(n)`. Same behaviour [read]. Examples: `format_exc_check_arg`, `cmp_outcome`, `TARGET(UNARY_NOT)`, `TARGET(COMPARE_OP)`, `_PyEval_SliceIndex`. |
| Renames only | 6 | 0 | `dk_get_index` → `dictkeys_get_index` (`lookdict_split`), `DK_INCREF` → `dictkeys_incref`, `builtin_pow_impl`'s parameter names, `PyTuple_SetItem` → `PyTuple_SET_ITEM` (`long_divmod`), `PyTuple_New`'s counters; and `TARGET(MAP_ADD)`, which is a real change (below). |
| Other | 95 | 33 | 63 entries, 16 `CANNOT_BLOCK`, 10 `SEEDS`, 3 `ASSUMPTIONS`, 3 `TYPES`. Below. |
| Missing | 2 | 0 | `Python/ceval.c:format_kwargs_mapping_error` (3.8 has `format_kwargs_error`, also used by `CALL_FUNCTION_EX`); `Python/sysmodule.c:sys_getframe` (Argument Clinic: `sys__getframe_impl`) [read]. |

The 95 "other", by cause [read, from the token diffs]:
- **`PREDICTED(op)` moved into the case** (3.7 writes it before the next `TARGET`, so it counted in the previous case's shape): about 7 cases change only by that (`DELETE_NAME`, `JUMP_FORWARD`, `POP_JUMP_IF_FALSE`, `POP_JUMP_IF_TRUE`, `JUMP_IF_TRUE_OR_POP`, `JUMP_ABSOLUTE`, and with small edits `UNPACK_SEQUENCE`, `FOR_ITER`). A shape artefact, not a change in the case.
- **Real changes with no effect on what Ni does** [inferred]: `_PyObject_ASSERT(obj, …)` for `assert` (`_PyObject_GenericGetAttrWithDict`, `PyObject_SetAttr`, `insertdict`), `_PyDict_GetItemIdWithError` (frames, `import_name`, `LOAD_BUILD_CLASS`), `valid_index` (`list_item`, `list_ass_item`), `list_new_prealloc` (`list_subscript`, `list_concat`), clinic rewrites of `unicode_strip`/`lstrip`/`rstrip`/`replace`/`split` and `builtin_chr`, `Py_UNUSED(ignored)` parameters (`dictkeys_new`…), `PySys_Audit` hooks (`builtin_id`, `member_get`), `Py_DECREF`/`Py_XDECREF` as inline functions, `Py_UNREACHABLE` now `Py_FatalError`.
- **Real changes Ni's hand code must be read against** [read]:
  - Vectorcall (PEP 590): `PyObject_Call`, `PyCFunction_Call` (now `PyVectorcall_Call`), `_PyFunction_FastCallDict`, `PyFunction_NewWithQualName` and `PyMethod_New` (set `vectorcall`), `TYPES` `PyFunctionObject`, `PyCFunctionObject`, `PyMethodObject` (a `vectorcall` field). Ni's descriptors read none of the changed fields, and it builds these objects through their constructors, so its code is unaffected [inferred]; the entries for Ni's `PsycoObject_Call` and the call path (step 7) need reading.
  - Positional-only arguments (PEP 570): `_PyEval_EvalCodeWithName` (`co_posonlyargcount`). Ni binds positional arguments only and sends keyword calls to the real call (`psyfunc.c`), and 3.8's `co_argcount` counts positional-only parameters (`Include/code.h`), so nothing changes there [inferred].
  - The new finally model (unwinding moved into the compiler): `_PyEval_EvalFrameDefault`, `TARGET(RETURN_VALUE)` (`exit_returning`), `TARGET(YIELD_VALUE)` (`exit_yielding`), `TARGET(EXTENDED_ARG)`, `SETUP_WITH`, `WITH_CLEANUP_START`; and the opcache in `TARGET(LOAD_GLOBAL)` (CPython's own cache; Ni's `LOAD_GLOBAL` is core).
  - Dicts: `PyDict_New` makes an empty dict with the shared `Py_EMPTY_KEYS` and `empty_values`, so a new empty dict looks like a split table (`dk_usable` 0, `lookdict_split`); `PyDict_SetItem` has an `insert_to_emptydict` path. Ni's inline store and keys guards (STORES.md) need checking against that [inferred]. `_PyDict_NewPresized` now returns `PyDict_New()` for `minused <= USABLE_FRACTION(PyDict_MINSIZE)`, that is `<= 5`: exactly the `('extra', 'small_presized', 'minused <= 5')` Ni invented by hand, so on 3.8 the item has C behind it and is a `shortcut`.
  - Numbers: `float_richcompare` (`_PyLong_Lshift` for `PyNumber_Lshift`; F104's entry), `long_lshift`/`long_rshift` (now wrappers of `long_lshift1`/`long_rshift1`), `long_pow`, `PyFloat_AsDouble` (`nb_index`), `float___round___impl`, `builtin_round_impl`, `min_max`.
  - Errors: `PyErr_Fetch`, `PyErr_Restore`, `PyErr_SetString`, `PyErr_SetObject`, `PyErr_FormatV`, `PyErr_Format` are now wrappers of the `_PyErr_*(tstate, …)` functions, which matters for the effect rules (below).
  - `collect_generations` (a `state` parameter), `PyEval_SaveThread` (`_PyRuntimeState`), `PyThreadState_Get`.

None of this stops generation of the right code: the shapes are the gate. Each entry has to be read against its new C and given a second shape (D71's tuple), and two keys re-pointed.

### `gen_long`, `gen_float`

Failed with `LookupError: no struct PyNumberMethods`: the slot driver read only `Include/object.h`, and 3.8 declares it in `Include/cpython/object.h`. Fixed by a general rule (find it in any `Include/**/*.h`). Then `plongobject_gen.h` (5 generated) and `pfloatobject_gen.h` (8) are 3.7's text but for the version in the first line. The only other difference is two skip reasons: `nb_lshift`/`nb_rshift` are now refused as "guard `Py_SIZE(b) < 0` is not a one-digit test" instead of "no fast-path if after the type check", because 3.8 split them into wrappers (`longobject.c:long_lshift`, `long_lshift1`). Skipped on both versions.

### `gen_ceval`: the reader

Crashed (`IndexError` in `_statement`): 3.8 writes `case TARGET(op): {` (`TARGET(op)` expands to `op: TARGET_##op`), 3.7 `TARGET(op) {`. Fixed by a general rule in `ceval_reader.cases`; with it, `PREDICTED(op)` as a case's first statement (3.8's place for it) is dropped as the label of the case's start, `STACK_SHRINK(n)` in an error exit counts as a pop like 3.7's `STACKADJ(-n)`, and a bare `tstate` handed to a call is no longer refused as core (its fields still are).

**Read 83 of 120 cases, refused 37** (3.7.17: 82 of 119, 37). Every effect read equals 3.8's `dis.stack_effect` at the 13 opargs. Against 3.7's reader output: 91 cases read the same text (or the same refusal), and:
- **Same effect, body differs only by `tstate`:** `PRINT_EXPR`, `GET_AWAITABLE`, `UNPACK_SEQUENCE`, `UNPACK_EX`, `DELETE_DEREF`, `LOAD_DEREF`, `BUILD_CONST_KEY_MAP`, `BUILD_MAP_UNPACK`, `COMPARE_OP`, `IMPORT_NAME`, `IMPORT_FROM`, and the three calls (`call_function(tstate, &sp, …)`).
- **`MAP_ADD`, a real change:** `(map, unused[oparg - 1], value, key -- …)` on 3.7, `(map, unused[oparg - 1], key, value -- …)` on 3.8: dict comprehensions evaluate the key first (`ceval.c` `TARGET(MAP_ADD)`: `value = TOP(); key = SECOND();`; key below value means the key is pushed, so evaluated, first).
- **`BUILD_MAP_UNPACK_WITH_CALL`:** the input is named `in` (3.8's variable), and its error path is one call, `format_kwargs_error(tstate, in, arg)`.
- **New opcodes:** `ROT_FOUR` read (`fourth, third, second, top -- top, fourth, third, second`); `CALL_FINALLY` read (`-- ret`); `BEGIN_FINALLY` (pushes NULL), `POP_FINALLY` (block stack) and `END_ASYNC_FOR` (`goto exception_unwind`) refused.
- **Gone:** `SETUP_LOOP`, `SETUP_EXCEPT`, `BREAK_LOOP`, `CONTINUE_LOOP`.
- **Refused for another reason, both versions:** `FORMAT_VALUE` (3.8 adds a `FVC_NONE` case and a `default:` raising SystemError: control flow, a `switch` with `goto error`; it was read on 3.7 and stopped in the back half), `RAISE_VARARGS` (a `switch` on oparg; was `why = WHY_EXCEPTION`), `RETURN_VALUE`, `YIELD_VALUE`, `YIELD_FROM`, `END_FINALLY` (other labels: `exit_returning`, `exit_yielding`, `exception_unwind`), `WITH_CLEANUP_START`, `WITH_CLEANUP_FINISH` (block stack).

Refusals by kind on 3.8: core 20, shared code 6, stack 6, control flow 3, falls through 2 (`BUILD_TUPLE_UNPACK_WITH_CALL`, `BUILD_TUPLE_UNPACK`). Since D93 the reader reads a case that falls into the next with that body, so 3.8 has none falling through; `BUILD_TUPLE_UNPACK` generates (3.7's text but one call's effect flags) and `BUILD_TUPLE_UNPACK_WITH_CALL` is refused in the back half (`check_args_iterable` takes `tstate`). From `631c0dd` to `96eeba2`, 3.8's `gen_ceval` stopped at its label copies (`unbound_local_error`, F112, fixed).

### `gen_ceval`: the back half

**60 generated** (3.7.17: 59). The 58 cases both versions generate other than `MAP_ADD` are **byte-identical** (including `LOAD_FAST`, `DELETE_FAST`, `UNARY_NOT`, the conditional jumps and `BINARY_ADD`, which needed the reader rules above). `MAP_ADD` loads `key = NTOP(2)`, `value = NTOP(1)`, then calls `PsycoDict_SetItem(po, map, key, value)` and releases value before key, as 3.8's C does. `ROT_FOUR` is new and generated (four `NTOP`s, pops and pushes). This is D67's claim working: the opcode changes reached Ni through the input only.

Not generated, same reasons as 3.7 but with `tstate` in the statement text: `COMPARE_OP`, `IMPORT_NAME`, `IMPORT_FROM`; the builds with loops; cells. `CALL_FINALLY` is refused for `INSTR_OFFSET()` (vocabulary; it is core anyway). `BUILD_STRING` is refused for `operand '0'` as on 3.7, after `2a71def` made `PyUnicode_New` visible again (it moved to `Include/cpython/unicodeobject.h`).

### `gen_layouts`

51 fields, the same `NI_OFS_`, sizes and assertions; the only differences are four comments naming the header the struct was found in (`PyTypeObject`, `PyTupleObject`, `PyDictObject`, `PyASCIIObject` are in `Include/cpython/` on 3.8). Since D77 moved `PyGC_Head` into the `store_gc` snippet, nothing in the table differs on 3.8 (METACOMPILER's stage-0 table, written before D77, counted it). `runtime_layouts_gen.h` is empty on both.

### `gen_abstract`

Stopped with `GenError: gcc -E -dM Objects/abstract.c: pycore_pystate.h: No such file`: 3.8's core is compiled with `-I$(srcdir)/Include/internal` (`Makefile.pre.in`, `PY_CFLAGS_NODIST`). Fixed (`cparse.include_dirs` takes the `-I$(srcdir)/…` of `PY_CFLAGS_NODIST` and `PY_CPPFLAGS` from the source's own `Makefile.pre.in`; on 3.7 that is `Include` alone). Then 49 generated, as on 3.7, with these differences:
- **Real 3.8 changes, translated** [read]: `PyMapping_Size` refuses a sequence that isn't a mapping, and `PySequence_GetItem`/`SetItem`/`DelItem` one that is a mapping but not a sequence (`"%.200s is not a sequence"`, `abstract.c`); each new test reads a slot through `Psyco_TypeSlot` with a resume, as the rules say. The run-time copies (`cimpl_*`) carry the new tests too.
- **A generator limit:** `sequence_repeat`, `PyObject_GetItem`, `PyObject_SetItem` and `PyObject_DelItem` lose their index path and resume as a whole (`a test of the run-time value PyIndex_Check(n)`), and `PyIndex_Check`/`PyIter_Check` appear as skipped functions. 3.8's `abstract.c` `#undef`s `PyIndex_Check` and `PyIter_Check` (line 1251, 2593) to define functions of those names for the limited API, after the functions above have used the macro. `cparse.cpp` takes the macros from `gcc -E -dM`, which is the state at the end of the file, so the macro is gone and the call is read as the function. On 3.8 this would lose `seq * int` (F102's fix through `PsycoNumber_AsSsize_t`) and the index paths, [inferred] a visible loss in `[x] * n`.
- **Rule proposed:** a macro's definition is the one in force where it is used. Simplest form: where the file `#undef`s a name that `-dM` doesn't list, take the definition from `-dM` of the file cut at the `#undef` (or from the headers alone); or run `-E -P` and read calls from the preprocessed text, which is position-correct. In `cparse.py`/`gen_abstract.py`, which another agent is changing (loops), so not done here. About 1–2 h.

### Mechanisms

The same ten headers as the detector table in METACOMPILER.md "Mechanisms": entry `frame_eval_hook`, tracing `tstate_use_tracing`, frames `frame_object`, `eval_breaker` `int32_address`, both tstate `runtime_address`, recursion `depth_up`, exceptions `curexc_triple` (3.7's snippets), and two stubs that `#error` in the compiler: **`eval_breaker_runtime: ceval_loop_top_signals`** (3.8 has `signals_pending`, handled by `handle_signals` before the pending calls) and **`store_gc: gc_next`** (`PyGC_Head` is `_gc_next`/`_gc_prev`; tracked when `_gc_next != 0`). A name check of every CPython identifier in `ni/` against 3.8's `Include/` found only `_PyGC_REFS_SHIFT` and `_PyGC_REFS_UNTRACKED` (in `gc_refs.h`, which 3.8 doesn't select) and four opcodes (below).

One more version-dependent line the detectors don't cover [read]: `Python/pyruntime.c` includes `"internal/pystate.h"`; 3.8 renamed it `internal/pycore_pystate.h`. A 3.8 build would fail there before reaching the stubs.

### `effects.py` (a tool; the build calls `effects.analyse`)

`effects.analyse` ran on 3.8 inside `gen_ceval` and `gen_abstract`, and the flags of every residual call in their output are the same as on 3.7. The tool's comparison with Ni's call sites failed (`no struct PyTypeObject`, read only from `Include/object.h`; fixed). After the fix it runs, and is much more cautious on 3.8 (3.7 → 3.8): `cannot_block` agree 37 → 13, disagree 1 → 25; `no_python` disagree 2 → 12; `pure` disagree 2 → 10. Two causes, both generator limits [run, from the witness paths]:
1. **3.8's assertion macros.** Paths such as `PyTuple_New → _PyObject_GC_TRACK → _PyObject_ASSERT_FROM → _PyObject_AssertFailed → _PyObject_Dump → PyObject_Print (blocks)`: 37 of the witnesses. 3.7 used `assert()`, which the call graph doesn't follow; 3.8's `_PyObject_ASSERT*` are macros that `#ifdef NDEBUG` away, and ctokens reads both branches. Rule proposed: read macros as compiled (`cparse.cpp`'s `-dM` with the target's CFLAGS, as `gen_abstract` does), or a function declared `Py_NO_RETURN` that ends in a fatal seed is itself fatal (`_PyObject_AssertFailed` ends in `Py_FatalError`). About 2 h.
2. **The seeds miss the `_PyErr_*` internals.** `PyErr_NoMemory → _PyErr_SetNone → _PyErr_SetObject (decref …)` and `PyErr_Clear → _PyErr_Clear → _PyErr_Restore`: the walk stops at `SEEDS` `PyErr_SetObject` and `ASSUMPTIONS` `PyErr_Restore`/`PyErr_SetString` by name, and on 3.8 the paths go round them through the internal functions. This also makes the 7 allocators 3.7 recognises (`PyFloat_FromDouble`, `PyLong_FromLong`, `PyMethod_New`…) read as running Python. Needs seeds and assumptions on `_PyErr_SetObject`, `_PyErr_Restore`, `_PyErr_SetString`, `_PyErr_FormatV`, which don't exist on 3.7, and `policy.py` fails a key not in the source: the table needs a version-free way to say "absent in some sources" (QUESTIONS 49). About 1 h after that decision.

## Fixes made (`2a71def`)

General rules, each needed to read 3.8 and each a no-op on 3.7:
1. `cslots.number_methods`, `effects.header_tokens`: a struct from whichever `Include/**/*.h` declares it, `Include/object.h` first.
2. `cparse.include_dirs`: the core's include path from the source's `Makefile.pre.in`.
3. `ceval_reader.cases`: `case TARGET(op):` labels and a leading `PREDICTED(op);`.
4. `ceval_reader._pop_count`: `STACK_SHRINK(n)` is a pop like `STACKADJ(-n)` in an error exit.
5. `ceval_reader._is_argument`: a bare `tstate` passed to a call is the running thread's state, not core; `tstate->` still is. `ERROR_FORMS` match with a leading `tstate,` argument.
6. `csource`: a `TARGET`'s shape leaves out the label's `case` and `:`; an `Include/` key is found in any `Include/**/*.h` (PyTupleObject, `Py_SETREF`, `PyIter_Check` moved to `Include/cpython/`).
7. `gen_ceval.c_signatures` reads `Include/**/*.h` (top-level headers first and winning); its "core" skip names the function the stack pointer is handed to (`call_function`, not `tstate`). `policy.method_table` finds the function after 3.8 clinic's `(PyCFunction)(void(*)(void))` cast.

**Checks:** `generate.py` on 3.7.17 gives byte-identical headers and report before and after (all sixteen headers); a checked build (`NI_TRACE=1 NI_ALL_CHECKS=1`) compiled those same headers (compared with the build directory's `gen/`); `tests/run.py` 755 pass, 0 fail; `tests/mechanisms.py` 0 failed; `policy.py --src 3.7.17` 0 problems; `effects.py` on 3.7.17 the same output; `gen_ceval.py --try` on 3.12.9's `bytecodes.c` still 23 of 186.

## What 3.8 needs

Estimates are AI elapsed time, rough.

| # | What | Kind | Estimate |
|---|---|---|---|
| 1 | **Policy table re-read:** 128 entries against their 3.8 C, a second shape each; `format_kwargs_mapping_error` → `format_kwargs_error`, `sys_getframe` → `sys__getframe_impl`; `small_presized` from `extra` to `shortcut`. Most are mechanical (27 thread-state, 6 renames, about 7 `PREDICTED` moves); 33 of the rest carry policy items. A `policy.py --diff OLD_SRC` that prints each changed entry's classified token diff (the scratch classifier) would make it a reading job. | rules (data) | tool 1 h; reading 4–6 h |
| 2 | **Shape rule for `PREDICTED(op);`:** leave it out of a `TARGET`'s shape wherever written (re-records about 8 of 3.7's shapes, outputs unchanged). Optional: it only removes artefacts from item 1. | generator | 30 min |
| 3 | **Done** (`9ce5ad4`, D78 (8)): **Macros as in force where used** (`gen_abstract`/`cparse`: `PyIndex_Check`, `PyIter_Check` `#undef`ed late in `abstract.c`). Without it 4 metas lose their index path on 3.8. Coordinate with the loops work. | generator | 1–2 h |
| 4 | **Effect rules on 3.8:** macros as compiled (or `Py_NO_RETURN` → fatal), and seeds/assumptions on the `_PyErr_*` internals, which needs item 5. Not needed for today's generated output, needed before step 5's families or computing D53's list. | rules | 2–3 h |
| 5 | **Policy keys absent in some sources** (QUESTIONS 49): e.g. a shape tuple entry `None` meaning "this source has no such definition", checked like a shape. Needed for item 4 and for any 3.8-only function Ni must say something about (`_PyObject_Vectorcall`, `PyVectorcall_Call`). | rules (checker) | 1 h |
| 6 | **Mechanism `eval_breaker_runtime: ceval_loop_top_signals`:** 3.7's `ceval_loop_top.h` plus 3.8's `signals_pending` test, `handle_signals` (static: main thread check, `UNSIGNAL_PENDING_SIGNALS`, `_PyErr_CheckSignals`), `make_pending_calls` (static) and `_PyRuntime.ceval` (3.8's struct). Or, per D77 (6), generate it from the loop top with the reader. | mechanism snippet | 2–3 h |
| 7 | **Mechanism `store_gc: gc_next`:** `NI_GC_UNTRACKED_TEST(rg)` as a compare of `_gc_next` (16 bytes before the object) with 0, with assertions on `PyGC_Head`'s two fields. | mechanism snippet (backend) | 1 h |
| 8 | **`pyruntime.c`'s internal header** (`internal/pystate.h` → `internal/pycore_pystate.h`): a detector for where `_PyRuntime` is declared, with the include in the runtime snippets. | mechanism | 30 min |
| 9 | **The block stack and finally opcodes in the core:** `mergepoints.c` (its analysis and its opcode whitelist) and `pycompiler.c` name `SETUP_LOOP`, `SETUP_EXCEPT`, `BREAK_LOOP`, `CONTINUE_LOOP`, which 3.8's `opcode.h` doesn't define, so they don't compile; 3.8 needs `BEGIN_FINALLY`, `CALL_FINALLY`, `POP_FINALLY`, `END_ASYNC_FOR`, the new `END_FINALLY`/`WITH_CLEANUP_*` stacks, and `ROT_FOUR` in the whitelist (3.8 emits it when a `return` leaves an `except` handler, `compile.c:compiler_unwind_fblock`). How to do it without `#if` is a design question (QUESTIONS 48). The biggest item. | core | 1–2 days |
| 10 | **Dicts' shared empty keys:** check the inline store, keys guards and `PsycoDict_*` against 3.8's empty dicts (`Py_EMPTY_KEYS`, `empty_values`: a split-looking table with `dk_usable` 0) [inferred risk]. | core (STORES.md) | 2–4 h |
| 11 | **Re-read the hand metas whose C changed:** `PsycoObject_Call` and the call path (vectorcall), `pfloat_richcompare` (F104), list item and subscript, `PyDict_New`/`SetItem`. Item 1 finds them; same code against the 3.8 hand version where it applies. | translated | 2–3 h |
| 12 | **3.8 build and tests:** an interpreter to build against (pyenv 3.8.20, or the uv one if its headers and `libpython` suit `setup.py`), `cpython_tests.py`'s module lists for `lib/python3.8/test`, `check.sh` for a second version, behaviour tests whose expectations depend on 3.7 bytecode. | tooling | 3–5 h |
| 13 | **Done** (`a2def0d`, `3186aca`): `generate.py --past-policy` reports the policy problems and runs the generators past them, writing every header (for reading, not a build); `donecheck.py --past-policy` likewise. | tooling | – |
| 14 | **Since the rerun at `3186aca`** ("3.8 refusals by cause"): `_PyArg_CheckPositional`'s entry once item 5 exists (13 builtin definitions); a `tstate` parameter in `helper_copy` (3 opcodes, 5 copies); `METH_FASTCALL \| METH_KEYWORDS` for `pow`; `A && f() < 0` as nested tests (`id`); a dead `switch` arm (`FORMAT_VALUE`); F114 (math's error test). | generator, rules | 4–6 h |

Not needed for 3.8 [inferred]: an entry mechanism other than the frame hook (the detectors give `frame_eval_hook`; 3.8's functions still evaluate through `_PyEval_EvalFrame`, so the PEP 523 hook sees every Python call); anything for positional-only parameters (Ni binds positional arguments only); new layouts (Ni reads none of the changed fields).

Order: 8, 7, 6 and 9 are what a compile needs; 1 (with 2) is what generation needs; then 3, 11 and 12 for same behaviour; 4 and 5 before step 5's families. Total, roughly 3–5 days, most of it item 9.

**After D88** (`fa3dae7`) [run]: 71 generated on 3.8.20 (3.7.17: 73). `STORE_DEREF`, `DELETE_DEREF`, `BUILD_STRING` and `BUILD_MAP_UNPACK` are byte-identical to 3.7's. Refused on 3.8 only: `IMPORT_FROM` and `BUILD_MAP_UNPACK_WITH_CALL` (their helpers take `PyThreadState *tstate`; rule `helper_copy` takes object parameters only, and 3.8's `import_from` has a `goto`), `FORMAT_VALUE` (the `switch` gained `case FVC_NONE` and a `default:` that raises SystemError and jumps to `error`, which the reader refuses). 3.8 needs: a `tstate` parameter in a copy as the current thread's, and a switch arm that can't be taken (a compile-time value outside the arms) read as dead.

## Rerunning

`generate.py --python ~/.local/share/uv/python/cpython-3.8.20-linux-x86_64-gnu/bin/python3.8 --src ~/.cache/ni/cpython/Python-3.8.20 --out DIR --past-policy -v` (since `a2def0d`; the scratch driver of `2a71def` is no longer needed), and the same for 3.7.17 without `--src`/`--past-policy`. Compare per header with the version string masked, `gen_ceval`'s output case by case (`      case X: {` starts a case), and the `-v` reports entry by entry. `donecheck.py --python <3.8> --src <3.8> --past-policy` runs the done test. `python3 tools/metac/ceval_reader.py SRC` and `policy.py --src SRC` run alone.
