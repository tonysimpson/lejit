---
written: 2026-11-18
head_ref: b36a1ac
sources: every line of the files below read against the CPython 3.7.17 source (`~/.pyenv/sources/3.7.17/Python-3.7.17`); five agents by file family, assembled and spot-checked by a sixth
---
# Inventory: what in Ni mirrors CPython, function by function

GENERATION.md step 0, the inventory half (the same-code tool, `tools/samecode.py`, is the other). It answers GOAL point 1's question: what "everything that mirrors CPython is generated" means, in lines and in functions, and what policy the hand translations carry that the generator's rules or the core must take over.

**Scope.** Every file MAP marks `translated`, `core + translated`, `translated + core`, `glue + translated` or `translated + glue`, plus `ni/Objects/psliceobject.c`/`.h`, which have no MAP row (they would be `core + translated` and `glue`). The generated headers (`plongobject_gen.h`, `pfloatobject_gen.h`, `pycompiler_gen.h`) are counted as generated and not inventoried; since `9eae61d` (D68) they are built into `build/temp.*/gen/` and are no longer in git. At `6c6d305` they were 161, 215 and 573 lines, and `pycompiler_gen.h` held 41 opcode cases.

**How to read the tables.** One row per function, macro group, table, registration block or (in `pycompiler.c`) opcode case; the rows of a file cover every line of it once, so the line counts add up to the file. Columns:
- **class**: `translated` (mirrors a CPython function with only mechanical patterns), `core` (Ni's own machinery or representation, no counterpart), `mixed` (both; the notes say what part is core), `glue` (includes, declarations, registration, file comments).
- **CPython 3.7 counterpart**: `file:function`, each checked to exist in the 3.7.17 tree; `partial:` when only some of the function is translated; `(Py2)` when the counterpart is Python 2 only.
- **patterns**: PATTERNS.md IDs.
- **policy**: what the policy table or the core must carry, by kind (below).

Everything in the per-file tables was checked in code **[read]** unless the cell says **[inferred]**. Nothing was built or run for this inventory.

## Summary

### Now, from the code: `tools/metac/donecheck.py` (D90) [run]

The tables below are this inventory at `b36a1ac`, by hand, and are not kept up to date. What is generated and what is still hand-written, with or without a reason, is computed from the code by `tools/metac/donecheck.py --python EXE` (GENERATION step 10; reasons in `tools/metac/hand_table.py`, the policy table's items and the generators' refusals). At `88d850c`, CPython 3.7.17: 191 definitions generated (5,645 lines), 229 hand-written with a reason (5,020), 22 hand-written with none (331).

| file | reasoned | lines | no reason | lines |
|---|---:|---:|---:|---:|
| `ni/Modules/pmath.c` | 3 | 18 |  |  |
| `ni/Objects/pabstract.c` | 7 | 173 | 3 | 72 |
| `ni/Objects/pboolobject.c` | 2 | 18 |  |  |
| `ni/Objects/pbytesobject.c` | 2 | 74 |  |  |
| `ni/Objects/pclassobject.c` | 1 | 53 |  |  |
| `ni/Objects/pdescrobject.c` | 2 | 18 |  |  |
| `ni/Objects/pdictobject.c` | 3 | 37 | 2 | 21 |
| `ni/Objects/pfloatobject.c` | 14 | 121 | 2 | 8 |
| `ni/Objects/pfloatobject.h` | 6 | 13 |  |  |
| `ni/Objects/pfuncobject.c` | 4 | 124 |  |  |
| `ni/Objects/piterobject.c` | 4 | 128 | 1 | 5 |
| `ni/Objects/piterobject.h` | 1 | 5 |  |  |
| `ni/Objects/plistobject.c` | 9 | 259 |  |  |
| `ni/Objects/plongobject.c` | 17 | 282 | 2 | 21 |
| `ni/Objects/plongobject.h` | 4 | 4 |  |  |
| `ni/Objects/pmethodobject.c` | 6 | 113 |  |  |
| `ni/Objects/pmethodobject.h` | 1 | 13 |  |  |
| `ni/Objects/pobject.c` | 23 | 527 | 4 | 129 |
| `ni/Objects/pobject.h` |  |  | 2 | 5 |
| `ni/Objects/prangeobject.c` | 5 | 91 | 1 | 4 |
| `ni/Objects/psliceobject.c` | 2 | 57 |  |  |
| `ni/Objects/pstructmember.c` | 1 | 144 |  |  |
| `ni/Objects/ptupleobject.c` |  |  | 1 | 21 |
| `ni/Objects/ptupleobject.h` | 2 | 6 |  |  |
| `ni/Objects/punicodeobject.c` | 9 | 185 |  |  |
| `ni/Python/frames.c` | 1 | 12 |  |  |
| `ni/Python/pbltinmodule.c` | 13 | 337 |  |  |
| `ni/Python/pycompiler.c` | 68 | 1,803 | 3 | 38 |
| `ni/Python/pycompiler.h` | 1 | 16 |  |  |
| `ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h` | 2 | 38 |  |  |
| `ni/mechanisms/exceptions/curexc_triple.h` | 1 | 10 |  |  |
| `ni/mechanisms/recursion/depth_up.h` | 6 | 63 |  |  |
| `ni/mechanisms/tstate/runtime_address.h` | 1 | 7 |  |  |
| `ni/mergepoints.c` | 1 | 7 | 1 | 7 |
| `ni/psyfunc.c` | 7 | 264 |  |  |
| **total** | **229** | **5,020** | **22** | **331** |

The tool counts definitions that mirror CPython, not files: the core around them (promotion, merge points, the backend) is not in these lines, and neither are comments or glue, so the totals are not comparable with "Lines by class". Generated, by header: `pabstract_gen.h` 52 (1,921 lines), `pycompiler_gen.h` 73 cases (1,125), `pfloatslots_gen.h` 10 (789), `pmathmodule_gen.h` 12 (516), `ptupleobject_gen.h` 4 (370), `plongobject_gen.h` 12 (330), `pbltinmodule_gen.h` 15 (228), `psyfunc_gen.h` 1 (153), the three `*_virtual_gen.h` 3 (149), `pycompiler_copies_gen.h` 6 (40), the list, dict and bytes lengths 3 (24). The 22 with no reason, and what would settle each: GENERATION.md step 10.

### Lines by class [read]

| file | lines | translated | mixed | core | glue |
|---|---:|---:|---:|---:|---:|
| `ni/Python/pycompiler.c` | 3,570 | 1,352 | 655 | 1,323 | 240 |
| `ni/Objects/pobject.c` | 1,552 | 248 | 444 | 815 | 45 |
| `ni/Objects/pobject.h` | 266 | 30 | 0 | 137 | 99 |
| `ni/Objects/pabstract.c` | 1,106 | 991 | 36 | 65 | 14 |
| `ni/Objects/pabstract.h` | 115 | 0 | 0 | 0 | 115 |
| `ni/Objects/plongobject.c` | 538 | 109 | 289 | 60 | 80 |
| `ni/Objects/plongobject.h` | 57 | 8 | 0 | 27 | 22 |
| `ni/Objects/pfloatobject.c` (after D87: 230 lines) | 326 | 157 | 83 | 35 | 51 |
| `ni/Objects/pfloatobject.h` (after D87: 84 lines) | 88 | 12 | 0 | 23 | 53 |
| `ni/Modules/pmath.c` (after D87: 70 lines) | 188 | 84 | 22 | 0 | 82 |
| `ni/Python/pbltinmodule.c` (after D86: 546 lines) | 668 | 174 | 333 | 84 | 77 |
| `ni/Objects/pstringobject.c` (compiled out) | 1,233 | 11 | 455 | 696 | 71 |
| `ni/Objects/pstringobject.h` (compiled out) | 73 | 18 | 0 | 36 | 19 |
| `ni/Objects/plistobject.c` | 439 | 28 | 237 | 119 | 55 |
| `ni/Objects/plistobject.h` | 44 | 0 | 0 | 11 | 33 |
| `ni/Objects/ptupleobject.c` | 278 | 26 | 64 | 150 | 38 |
| `ni/Objects/ptupleobject.h` | 43 | 0 | 0 | 15 | 28 |
| `ni/Objects/piterobject.c` | 252 | 25 | 109 | 92 | 26 |
| `ni/Objects/piterobject.h` | 41 | 7 | 0 | 13 | 21 |
| `ni/Objects/prangeobject.c` | 251 | 83 | 28 | 103 | 37 |
| `ni/Objects/prangeobject.h` | 13 | 0 | 0 | 0 | 13 |
| `ni/Objects/pmethodobject.c` | 290 | 23 | 101 | 137 | 29 |
| `ni/Objects/pmethodobject.h` | 59 | 0 | 0 | 20 | 39 |
| `ni/Objects/pfuncobject.c` | 396 | 11 | 0 | 344 | 41 |
| `ni/Objects/pfuncobject.h` | 57 | 0 | 0 | 32 | 25 |
| `ni/Objects/pclassobject.c` | 132 | 0 | 54 | 58 | 20 |
| `ni/Objects/pclassobject.h` | 32 | 0 | 0 | 7 | 25 |
| `ni/Objects/pdescrobject.c` | 66 | 20 | 0 | 0 | 46 |
| `ni/Objects/pdictobject.c` | 84 | 71 | 0 | 4 | 9 |
| `ni/Objects/pdictobject.h` | 26 | 0 | 0 | 0 | 26 |
| `ni/Objects/pbytesobject.c` | 96 | 16 | 64 | 2 | 14 |
| `ni/Objects/punicodeobject.c` | 273 | 54 | 158 | 18 | 43 |
| `ni/Objects/pstructmember.c` | 157 | 144 | 0 | 0 | 13 |
| `ni/Objects/pstructmember.h` | 16 | 0 | 0 | 0 | 16 |
| `ni/Objects/pboolobject.h` | 29 | 0 | 0 | 16 | 13 |
| `ni/Objects/psliceobject.c` (no MAP row) | 127 | 0 | 60 | 42 | 25 |
| `ni/Objects/psliceobject.h` (no MAP row) | 18 | 0 | 0 | 0 | 18 |
| `ni/psyfunc.c` | 1,272 | 101 | 517 | 611 | 43 |
| `ni/psyfunc.h` | 68 | 0 | 0 | 25 | 43 |
| `ni/mergepoints.c` | 808 | 8 | 13 | 753 | 34 |
| `ni/Python/frames.c` | 374 | 0 | 13 | 326 | 35 |
| `ni/Python/frames.h` | 60 | 0 | 0 | 23 | 37 |
| `ni/Python/pyruntime.c` | 71 | 29 | 18 | 0 | 24 |
| **total** | **15,652** | **3,840** | **3,753** | **6,222** | **1,837** |
| total without `pstringobject.*` | 14,346 | 3,811 | 3,298 | 5,490 | 1,747 |

What this says:
- **About a quarter is pure translation** (3.8k lines, 27% of the live 14.3k), a quarter mixed, two-fifths core. "All generated" (GOAL point 1) is at most the 3.8k translated lines plus the translated part of the 3.3k mixed; the core lines must be named as core in MAP.
- **MAP's layers are off for several files.** Mostly core though marked `core + translated`: `mergepoints.c` (93% core; no CPython analysis behind it, `peephole.c:markblocks` is the nearest), `frames.c` (87%), `pfuncobject.c` (87%: call policy and guards, only `func_descr_get` is translated), `psyfunc.c` (about half). Marked `translated` but half mixed: `pbltinmodule.c` (genexpr fusion D46, `super()` on the compiled frame P30, the virtual `range`, `isinstance`'s static-type answer). Really translated: `pabstract.c` (90%), `pstructmember.c`, `pdictobject.c`.
- **Dead code inside the scope**, which a generator need not reproduce: `pstringobject.*` (1,306 lines, compiled out by `#ifdef IS_PY3K`); the PsycoFunction proxy in `psyfunc.c` (about 300 lines plus `psyfunc.h`; `psyco_PsycoFunction_NewEx` has no caller, though `is_proxycode` is still tested in every `psyco_call_pyfunc`); and, with no callers, `PsycoSequence_GetSlice`, `PsycoSequence_SetSlice`, `PsycoSequence_Tuple`, `PsycoEval_CallObjectWithKeywords`, `PsycoObject_Repr`, the `DEF_KNOWN_RET_TYPE_n` macros, `PsycoIter_Check`, `PsycoSequence_Check`, `PsycoNumber_Divide`/`InPlaceDivide` (declared, never defined), `PsycoFloat_AsDouble`, `PsycoLong_FromLong`, `PsycoDict_Copy`, `PsycoDict_SetItem`, `PsycoList_SingletonNew`, `pbltinmodule.c`'s `cimpl_range`/`get_len_of_range`/`longobj_as_long`/`parse_range_args`, `KNOWN_VAR`, `CF_NONPURE_FP_HELPER`/`CF_PURE_FP_HELPER`.

### Policy found, by kind [read]

Counted per row and kind (a row with two guards of one kind counts once per item written).

| kind | pycompiler | object/abstract | numbers, builtins, math | containers | call path | total |
|---|---:|---:|---:|---:|---:|---:|
| EFFECT (Cf* flags carrying knowledge not in the signature) | 9 | 14 | 20 | 23 | 6 | **72** |
| DRIFT (differs from 3.7) | 23 | 17 | 4 | 14 | 7 | **65** |
| OTHER (see below) | 16 | 17 | 6 | 13 | 12 | **64** |
| VIRTUAL (what is virtual, weights, limits) | 10 | 2 | 17 | 28 | 3 | **60** |
| GUARD (D54 slot guards, version tags, `compiled_code`) | 5 | 44 | 3 | 6 | 1 | **59** |
| SHORTCUT (fast paths not in the C) | 5 | 16 | 18 | 14 | 0 | **53** |
| LIKELY (branch-likelihood choices) | 3 | 7 | 6 | 12 | 4 | **32** |
| RESTYPE (asserted result types, P08) | 6 | 3 | 8 | 10 | 0 | **27** |
| PROMOTE (what is promoted, static-only promotion) | 3 | 4 | 5 | 12 | 1 | **25** |
| FORGET (D59/D62/D63) | 4 | 11 | 0 | 0 | 2 | **17** |
| REF (`need_reference`, ownership beyond the C) | 7 | 3 | 0 | 2 | 2 | **14** |
| MEGA (megamorphic cut-offs) | 1 | 4 | 0 | 2 | 0 | **7** |

The main ones, by where they must go:
- **GUARD → the rules, applied by the generator.** Nearly all are one rule: every slot read on a type that may be a heap type goes through `Psyco_TypeSlot` (`pabstract.c:NB_SLOT_OF`…, `SQ_SLOT_OR`; `pobject.c:Psyco_HeapSlot`, `Psyco_TypeIsSubtype`), with the changed path resuming CPython's algorithm (D54). The rest is core: the attribute load/store guards shared per (type, name, kind) (`pobject.c:load_guard`, `store_guard`, D39, D60), the LOAD_GLOBAL version-tag cache (`pycompiler.c:load_global_cached`, D25), `pfuncobject.c:compiled_code`/`compiled_defaults` (F52, F57), `piterobject.c:piter_iternext`'s `it_seq` check (F48), `psyfunc.c:psyco_call_closure`'s globals compare.
- **FORGET → core, driven by EFFECT data.** `pobject.c:Psyco_ForgetClasses`, `forget_mutable_types`, `Psyco_TypeHintGuard`, `Psyco_ConstantType` and the calls of `Psyco_ForgetClasses` in `pycompiler.c` (end of instruction, POP_JUMP_IF_*, inlined frames, unwind) and `psyfunc.c` (direct calls, genexprs). Whether a call sets `class_unchecked` depends on its `CfPure`/`CfNoPython` flags and on the deallocs a decref can reach (P31).
- **EFFECT → the effect rules (step 3).** 72 hand-written flag sets, including the D53 list of C functions that can't block (`psyfunc.c:cannot_block_init`), `CfNoPython` on the instance-dict helpers (`pobject.c:cimpl_instance_dict_get`, F34), `CfPure` on allocators (`pmethodobject.c:compute_cfunction`, `pclassobject.c:compute_method`, `punicodeobject.c:punicode_item`'s `PyUnicode_FromOrdinal`: with constant inputs these fold to one shared object [inferred]) and one wrong flag (F104).
- **SHORTCUT → policy entries; some have no 3.7 C to generate from.** One-digit `& | ^ << >>` (`plongobject.c:plong_bitop`; 3.12 has it for `& | ^`), `divmod()`'s fast path and power-of-two divisors (`plong_divmod_op`, `floor_divmod_words`), one-digit hash and compare, `PsycoRange_New` for one-digit ints only, `PsycoDict_NewPresized`'s `minused <= 5` → `PyDict_New`, type whitelists (`PsycoNumber_Power`, `pfloat_richcompare`, `pmath.c:PMATH_TO_INT`, `pbuiltin_round`, `ptype_call`), the safe-descriptor whitelist and the no-`__del__` whitelist (`pobject.c:safe_descriptor`, `collect_undeletable_vars`), fixed-arity C calls without a tuple (`pmethodobject.c:call_fast`, F13). Dropping any of these shows in same code as a loss.
- **VIRTUAL, RESTYPE, PROMOTE, MEGA, REF → the policy table** (PATTERNS E, P03, P04, P08, P17), mostly at the virtual-object constructors and their `compute_*` functions, the `psyco_atcompiletime_static_type` sites (F25) and the two `_mega` promotions (`pfuncobject.c:pfunction_call`, `pobject.c:need_type_mega`).
- **OTHER**, in four groups. Field mutability chosen by hand (`FIX_size`/`VAR_size`, `FMUT` on `ob_item`, `it_index`, cells, every struct member): judgement for the descriptors PATTERNS P15 generates. What is compiled at all (`mergepoints.c:_build_merge_points`'s opcode whitelist, `frame_locals_builtins` C15, `psyco_is_genexpr` by the name `<genexpr>`, `psyfunc.c:fncall_init`'s callee filter, tracing D46). Inlining and merge points (`MAX_MP_DISTANCE` 170, `INLINE_MAXIMUM_WEIGHT` 6, `IS_LIGHT_INSTR`, `MP_FLAGS_INLINABLE`, D10). Caches and eval-breaker placement (D25 `mark_varying`, D31 `keys_inline`, D38/D47 store cache, `breaker_after_call` D45/D53). All core.
- **DRIFT**: 65 places where Ni differs from 3.7. Most are deliberate (Ni's finally states in the block-stack cases, LOAD_METHOD always pushing NULL, no `Py_EnterRecursiveCall` on direct calls C25) or harmless (`it_index` as `long`, `ma_used` read as `int`); the module-code ones (`STORE_NAME` assuming `f_locals == f_globals`) are unreachable while module code isn't compiled. Eight are bugs, added to KNOWN_BUGS as F97–F104. Several are Py2 leftovers: `cimpl_import_all_from`, `cimpl_import_name`'s level −1 test, `cimpl_finalize_frame_locals` (still referenced by `x64/ipyencoding.h`), `pclassobject.c`'s unbound-method branch, `psycofunction_call`'s fallback.

### CPython functions translated more than once [read]

| CPython 3.7 | Ni | why |
|---|---|---|
| `abstract.c:binary_op1`, `binary_iop1`, `sequence_repeat` | `pabstract.c`: the meta and a run-time `cimpl_` copy each | D54: the copy resumes the algorithm when a slot changed. One C function, two outputs: the generator must produce both |
| `abstract.c:PyNumber_Add`, `PyNumber_Multiply`, `PyNumber_InPlaceAdd`, `PyNumber_InPlaceMultiply` (sequence fallbacks) | `PsycoNumber_*` and `pabstract.c:cimpl_sequence_op` | as above |
| `ceval.c` LOAD_GLOBAL | `pycompiler.c:load_global_cached` (compile time) and `cimpl_load_global` (run time) | the D25 cache |
| `ceval.c:unpack_iterable` | `pycompiler.c:unpack_iterable`, `cimpl_unpack_iterable`, `cimpl_unpack_ex`; `cimpl_unpack_list` copies UNPACK_SEQUENCE's list branch | output array (P12) |
| `ceval.c:do_raise` | `cimpl_do_raise`, `cimpl_reraise` | P27 |
| `ceval.c` `fast_block_end` | `cimpl_push_exc_info`, `unwind_except_handler`/`cimpl_pop_exc_info`, the unwind loop | Ni's finally states |
| `ceval.c` eval_breaker block and its skip list | `ni_eval_breaker_handler`, `ni_eval_breaker_at_entry` (since D77 in `mechanisms/eval_breaker_runtime/ceval_loop_top.h`); skip list in `mergepoints.h:BREAKER_SKIPS` and again inline in `ni_eval_breaker_at_entry` | runtime-poll; the skip list is copied twice |
| `frameobject.c:_PyFrame_New_NoTrack` (the builtins choice) | `psyfunc.c:callee_builtins` and `pycompiler.c:psy_get_builtins` (also called from `psycofunction_call`) | P28 |
| `object.c:PyObject_SelfIter` | `piterobject.c:piter_getiter` and `prangeobject.c:prangeiter_getiter`, registered on the same C pointer | identical; the later registration wins for every iterator type |
| `listobject.c:list_item` | `plistobject.c:plist_item`, used for list `sq_item` and by `piterobject.c:piter_iternext` | shortcut |
| `longobject.c:long_long` | generated (D83; was `plong_pos`), and again inside `pbltinmodule.c:pbuiltin_round`, `ptype_call` | |
| `floatobject.c:float___trunc___impl`, `float___round___impl` | split into `pfloatobject.c` helpers, put together again in `pbltinmodule.c` (`float_whole_to_int`, `pbuiltin_round`) | float's `nb_int` has no meta |
| `rangeobject.c:get_len_of_range` | `prangeobject.c:cimpl_range_len` (the dead copy in `pbltinmodule.c` removed, D86) | |
| `floatobject.c:PyFloat_AsDouble` | generated since D87 (`pfloatslots_gen.h`, `PsycoFloat_AsDouble`); was `pfloatobject.c:PsycoFloat_AsDouble` (dead) and `pmath.c:PMATH_CONVERT_TO_DOUBLE` (an approximation, F99, fixed) | |
| `bltinmodule.c:min_max` | `pbltinmodule.c:pbuiltin_min_max`, and `pycompiler.c:cimpl_minmax_step`/`cimpl_minmax_result` (genexpr fusion) | |
| index normalisation and the step-1 slice case | per type in `pbytes_subscript`, `punicode_item`, `cimpl_bytes_slice`, `cimpl_unicode_slice`, `cimpl_list_slice` | P18a |
| `call.c:_Py_CheckFunctionResult` | `pmethodobject.c:check_result`, `cimpl_null_result`, `cimpl_result_with_error` | |

### CPython functions translated partially [read]

Only a case or a branch of these is translated; the rest goes to the CPython function (residual call) or is not compiled. The per-file tables say which part.
- **ceval.c:** `_PyEval_EvalCodeWithName` (positional arguments, defaults, `*args`, free variables; no keywords, a wrong count takes the real call: generated since D85, `psyfunc_gen.h`, called from `psyfunc.c:fncall_bind`), `call_function`, CALL_FUNCTION_EX, SETUP_WITH, WITH_CLEANUP_START, UNPACK_SEQUENCE's list branch, `cmp_outcome` (`PyCmp_EXC_MATCH`), the `error:` label's `PyTraceBack_Here`, the `use_tracing` test.
- **call.c:** `PyObject_Call` (`tp_call` branch), `_PyFunction_FastCallDict` (`psyfunc.c:psyco_call_pyfunc`), `_PyMethodDef_RawFastCallDict` (FASTCALL cases only).
- **abstract.c:** `PyIter_Next` (without clearing StopIteration), `PyNumber_Power`, `binary_op` (without the `>>` print hint), `PyObject_IsInstance` (compile-time answers only).
- **object.c:** `do_richcompare`, `_PyObject_GenericGetAttrWithDict` and `_PyObject_GenericSetAttrWithDict` (one step each in `load_kind`, `store_kind`, `cimpl_instance_dict_get`/`set`), `_PyObjectDict_SetItem` (combined-dict branch).
- **dictobject.c:** `insertdict` (split-table branch, `pobject.c:inline_store`, emitted by `x64/ipyencoding.c`), `_PyDict_GetItem_KnownHash`.
- **longobject.c:** `long_hash`, `long_richcompare`, `long_div`/`long_mod`/`long_divmod`, `fast_floor_div`, `long_and` and the other bit operations, `PyLong_AsDouble`: one-digit cases.
- **floatobject.c:** `float_richcompare` (float/float), `float___trunc___impl`, `float___round___impl` (no `ndigits`).
- **Others:** `listobject.c:list_subscript`, `list_ass_subscript`; `tupleobject.c:tuplesubscript` (index branch, via the generic `pabstract.c:psyco_generic_subscript`); `bytesobject.c:bytes_subscript`; `unicodeobject.c:unicode_subscript`, `PyUnicode_RichCompare`; `iterobject.c:iter_iternext` (no `PY_SSIZE_T_MAX` check); `rangeobject.c:fast_range_iter`; `sliceobject.c:PySlice_Unpack` (None step, None or int bounds); `descrobject.c:member_get`, `method_get` (no `descr_check`; `pobject.c:generic_getattr` checks the type first); `typeobject.c:super_init` (no arguments), `type_call` (`type(x)`, `int(x)`, `float(x)`); `bltinmodule.c:min_max` (two or more positional arguments), `builtin_sum_impl` (a tuple of known length), `builtin_round_impl`; `mathmodule.c:math_1_to_int`, `math_ceil`/`math_floor`; `genobject.c:gen_send_ex` (PEP 479 part); `frameobject.c:PyFrame_New`; `traceback.c:PyTraceBack_Here`; `sysmodule.c:sys_getframe` (F100); `Include/ceval.h:Py_EnterRecursiveCall`/`Py_LeaveRecursiveCall` (as emitted code).

### The opcodes [read]

- **Generated** (`pycompiler_gen.h`, from `bytecodes37.c`): 41 cases: NOP, POP_TOP, ROT_TWO, ROT_THREE, DUP_TOP, DUP_TOP_TWO, LOAD_CONST, LOAD_FAST, STORE_FAST, the three UNARY_* but NOT, the 13 BINARY_*, the 13 INPLACE_*, GET_ITER, STORE_SUBSCR. `bytecodes37.c` describes 43; UNARY_NOT and DELETE_SUBSCR are described but hand-written.
- **Core** (Ni's block stack, finally states and calls): BREAK_LOOP, CONTINUE_LOOP, RAISE_VARARGS, POP_BLOCK, POP_EXCEPT, END_FINALLY, SETUP_LOOP/EXCEPT/FINALLY, SETUP_WITH, WITH_CLEANUP_START/FINISH, CALL_FUNCTION/KW/EX, CALL_METHOD; mostly core: RETURN_VALUE, YIELD_VALUE (genexpr fusion), LOAD_METHOD (Ni's own form).
- **Translatable with a core part:** LOAD_GLOBAL/LOAD_NAME (D25 cache), STORE/DELETE_GLOBAL and _NAME (`mark_varying`), FOR_ITER (StopIteration as a pseudo-exception), the jumps (merge points, `psyco_compile_cond`), BUILD_TUPLE_UNPACK/LIST_UNPACK (virtual concatenation).
- **Translatable:** the other hand-written cases (UNARY_NOT, DELETE_SUBSCR, LIST_APPEND, FORMAT_VALUE, BUILD_STRING, UNPACK_*, STORE/DELETE_ATTR, DELETE_FAST, the DEREF and CLOSURE loads, LOAD_BUILD_CLASS, BUILD_*, SET_ADD, MAP_ADD, LOAD_ATTR, COMPARE_OP, IMPORT_*, MAKE_FUNCTION, EXTENDED_ARG).
- **Not handled** (the code is never compiled): PRINT_EXPR, GET_AITER, GET_ANEXT, GET_AWAITABLE, YIELD_FROM, GET_YIELD_FROM_ITER, LOAD_CLASSDEREF, SETUP_ANNOTATIONS, BEFORE_ASYNC_WITH, SETUP_ASYNC_WITH. RETURN_NONE and STORE_MAP are dead `#ifdef` cases.

### Outside the scope, but mirroring CPython

Not inventoried (MAP marks them `backend`, `glue` or `core + backend`), but they hold CPython knowledge a version change could break: `ni/codegen.c` (the rich-compare inversion table), `ni/x64/ipyencoding.c` (`insertdict`'s split-table store, `Py_DECREF`/`_Py_Dealloc`), `ni/x64/iencoding.c` (CPython's error conventions in `compile_time_check`/`run_time_result_check`), `ni/eval_hook.c` (the PEP 523 hook point), `ni/compat2to3.h`, `ni/Python/pycinternal.h` (`opcode.h`).

### Layouts, generated since D76 [run]

The per-file rows below that are field descriptors (`OB_type`, `FIX_size`, `FLOAT_ob_fval`, `LIST_ob_item`, `TUPLE_ob_item`, `LONG_ob_digit0`, `SEQITER_*`, `DICT_*`, `SLICE_*`, `METHOD_*`, `FUNC_*`, `CFUNC_*`, `RANGE_*`, `RANGEITER_*`, `UNICODE_*` and `STATE_*`, `CELL_ob_ref`, `KEYS_nentries`, `TSTATE_depth`/`_overflowed`, `CF_TSTATE_curexc_type`, `BYTES_sval`) now take each CPython field's offset, read size and sign from `layouts_gen.h` (`CPY_FIELD`; `tools/metac/gen_layouts.py`, `policy_table.LAYOUTS`). The copies of private structs (`rangeobject`, `rangeiterobject`, `seqiterobject`, `ni_dictkeys_t`, `ni_dictentry_t`) are generated from the source and gone from `prangeobject.c`, `piterobject.h` and `pobject.h`. What each row calls core stays core: the index chain, mutability, `FPYREF`/`FARRAY`, virtual-only slots. The backend's offsets (`x64/ipyencoding.*`, `iprocessor.c`, `iencoding.c`, `pycompiler.h`) and `pyruntime.c`'s two `_PyRuntime` fields come from the same headers (`CPY_OFFSET`, `NI_RUNTIME_`). METACOMPILER.md "Stage 0: layouts".

### What this changes in GENERATION.md

1. **The target is smaller than the file list suggests.** Of 14.3k live lines, 3.8k are translated and 3.3k mixed; step 0's split is above, and MAP's layers should follow it (the files listed under "Lines by class").
2. **Step 7 is mostly core.** Only `psyfunc.c`'s `fncall_*` binding is translation (generated since D85) (from `ceval.c:_PyEval_EvalCodeWithName`); `_PyFunction_FastCall*` are in `Objects/call.c` in 3.7, not `ceval.c`; `pfuncobject.c` is 87% core. The PsycoFunction proxy can be deleted first.
3. **Step 3's table has a list to start from**: the policy items above, per CPython function, in the per-file tables. The judgement that is code today (D53's `cannot_block_init`, the inlining limits, the opcode whitelist, field mutability) is data for the table or stays core.
4. **Same code will differ where a faithful translation fixes a bug** (F97–F104, the OverflowError/IndexError cases, `pmath.c`'s conversion): those differences are expected and need their reason recorded, not a match.
5. **One C function, two outputs**: the D54 resume copies (`cimpl_binary_op1` etc.) are a second product of the same function; step 4 should generate both.
6. **Docs out of date**, found on the way: PATTERNS P10 and rule 5 (`pbltinmodule.c` imports `builtins` and registers 14 builtins plus `type_call`; `binary_op1` follows 3.7 and `Py_TPFLAGS_CHECKTYPES` is gone, so it is no longer a drift test case); MAP's rows for `pdescrobject.c` ("unreachable": `generic_getattr` calls its metas), `pfuncobject.c` (its suspected bugs are fixed), `punicodeobject.c` (`unicode_richcompare` doesn't exist in 3.7; it is `PyUnicode_RichCompare`) and the missing `psliceobject.*`; the header comments of `pycompiler.c` (CALL_FUNCTION's oparg is read as 3.7 does) and `frames.h` (`psyco_emulate_frame`'s comment describes upstream's `psyco_get_frame`); GENERATION's "39 generated" (41).

## Per file

The rows of each table cover the file in order. Line counts, not line numbers (the project anchors to `file:function`).

## Front end: `pycompiler.c`

### `ni/Python/pycompiler.c`

Ni's compile-time copy of `ceval.c`'s main loop, the `cimpl_*` run-time copies of `ceval.c` helpers, and Ni's own machinery: meta registry, pseudo-exceptions, the D25 LOAD_GLOBAL cache, promotion, inlining, merge points and NI_CODESTATS **[read]**. The 41 simple opcode cases come from `pycompiler_gen.h` (one row here); every other case is hand-written in this file **[read]**. Surprising: UNPACK_SEQUENCE takes its tuple and list fast paths for subclasses; the header's Py2 note on CALL_FUNCTION's oparg is stale; `cimpl_finalize_frame_locals` is Py2 code still referenced by `x64/ipyencoding.h` **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment | 29 | glue | none |  | DRIFT: says CALL_FUNCTION's oparg is read as na \| nk << 8 (Py2), but psyco_ext_do_calls reads it as 3.7 does | The CALL_FUNCTION sentence is stale [read] |
| includes | 24 | glue | none |  |  |  |
| NI_CODESTATS: includes, promotion-site state | 23 | core | none |  |  | Statistics build only (ni._promostats) |
| psyco_cs_promo_site | 30 | core | none |  |  | Records a promotion site with a backtrace (NI_CODESTATS) |
| psyco_cs_promo_begin | 46 | core | none |  |  | NI_CODESTATS |
| psyco_cs_promo_end | 18 | core | none |  |  | NI_CODESTATS |
| psyco_cs_promo_counter | 16 | core | none |  |  | Writes raw x86-64 bytes (mov r11; inc [r11]): backend-specific; NI_CODESTATS |
| psyco_cs_promo_miss | 2 | core | none |  |  | NI_CODESTATS |
| psyco_cs_promo_stats | 34 | core | none |  |  | NI_CODESTATS; closes the #if |
| KNOWN_VAR macro | 3 | core | none |  |  | Unused anywhere in ni/ [read] |
| Psyco_Meta_Dict, Psyco_DefineMeta | 29 | glue | none | P09 |  | Meta registry keyed on the C function pointer |
| Psyco_DefineMetaModule | 9 | glue | none | P10 |  |  |
| Psyco_GetModuleObject | 19 | glue | none | P10 |  |  |
| Psyco_Unregistered_Metas, meta_not_registered | 21 | glue | none | P10 |  | Feeds ni._unregistered_metas() (F13) |
| Psyco_DefineModuleFn | 26 | glue | none | P10 | OTHER: registers only when ml_flags match exactly |  |
| Psyco_DefineModuleC | 15 | glue | none | P10 |  | Maps a callable type's tp_new |
| Psyco_Meta1x | 10 | core | none | P09,P12 |  | Meta lookup, else residual call |
| Psyco_Meta2x | 10 | core | none | P09,P12 |  |  |
| Psyco_Meta3x | 11 | core | none | P09,P12 |  |  |
| psyco_pyobj_atcompiletime_mega | 18 | core | none | P04 | MEGA: a RunTime_Megamorphic value returns 0; promotion via psyco_nonfixed_pyobj_promotion_mega |  |
| psyco_atcompiletime_static_type | 18 | core | none | P03 | PROMOTE: promotes static types only (psyco_static_type_promotion, F25) | Used by pbltinmodule.c |
| pyc_data_build | 17 | core | none |  |  | Rebuilds stack_base/stack_level from vlocals |
| block_setup | 11 | translated | Objects/frameobject.c:PyFrame_BlockSetup |  |  | Compile-time block stack in po->pr (block stack is core in GENERATION step 8) |
| block_pop | 7 | translated | Objects/frameobject.c:PyFrame_BlockPop |  |  |  |
| pseudo-exception sources ERtPython, EReturn, EBreak, EContinue, EInline | 9 | core | none |  |  | EReturn/EBreak/EContinue stand for ceval's WHY_* codes |
| PycException_SetString | 8 | core | none | P24 |  | Compile-time analogue of errors.c:PyErr_SetString |
| PycException_SetObject | 7 | core | none | P24 |  | Analogue of PyErr_SetObject |
| PycException_SetVInfo | 5 | core | none | P24 |  |  |
| PycException_Promote | 10 | core | none | P01,P03 |  | Raises the promotion pseudo-exception |
| PycException_SetFormat | 19 | core | none | P24 |  | Analogue of PyErr_Format |
| PycException_Matches | 20 | core | none | P12,P13 | EFFECT: PyErr_GivenExceptionMatches called CfPure\|CfReturnTypeInt; PyErr_ExceptionMatches CfReturnTypeInt | Analogue of errors.c:PyErr_ExceptionMatches for run-time or virtual exceptions |
| clear_pseudo_exception | 14 | core | none | P17 |  |  |
| PycException_Clear | 9 | core | none | P12 | EFFECT: PyErr_Clear CfNoReturnValue | Analogue of PyErr_Clear |
| psyco_virtualize_exception | 21 | core | none |  |  | A compile-time Python error becomes a pseudo-exception |
| cimpl_pyerr_fetch | 17 | mixed | partial: Python/errors.c:PyErr_Fetch | P12 | DRIFT: NULL becomes None | Core part: Ni's stack can't hold NULL |
| cimpl_finalize_frame_locals | 25 | translated | (Py2) Python/ceval.c:reset_exc_info |  | DRIFT: Py2 semantics (restores curexc_* and sets sys.exc_type/exc_value/exc_traceback) | Obsolete: the header says the per-frame saved exception is never set; still referenced by x64/ipyencoding.h FINALIZE_FRAME_LOCALS [read] |
| cimpl_push_exc_info | 35 | translated | partial: Python/ceval.c:_PyEval_EvalFrameDefault (fast_block_end, WHY_EXCEPTION into SETUP_EXCEPT/SETUP_FINALLY) | P12 | DRIFT: None where ceval pushes NULL for the old tb/value | Six objects through an A array |
| cimpl_pop_exc_info | 19 | translated | Python/ceval.c:UNWIND_EXCEPT_HANDLER (restoring half) | P12 | REF: does not consume its arguments; the caller decrefs |  |
| PycException_Fetch | 20 | core | none | P12 | EFFECT: cimpl_pyerr_fetch CfNoReturnValue | Run-time exception into three vinfos |
| cimpl_new_frame | 28 | mixed | partial: Objects/frameobject.c:PyFrame_New |  | OTHER: the real frame if the code has one, else a frame not linked into tstate->frame | Core part: emulated frames for compiled code (C11) |
| cimpl_rt_traceback | 22 | mixed | partial: Python/traceback.c:PyTraceBack_Here (ceval's error: label) |  | OTHER: links an emulated callee's f_back to this frame | Core part: emulated frame chain (C11) |
| cimpl_vt_traceback | 30 | mixed | partial: Python/traceback.c:PyTraceBack_Here |  | OTHER: builds a traceback without setting the exception | Core part: traceback for a virtual exception |
| PsycoTraceBack_Here | 19 | core | none | P12 | EFFECT: cimpl_vt_traceback CfCommonNewRefPyObjectNoError; cimpl_rt_traceback CfNoReturnValue | Compile-time form of ceval's error: PyTraceBack_Here |
| psyco_sk* constants, s_builtin_object, psyco_pycompiler_init | 37 | glue | none |  |  | Init |
| stack and frame macros (CHKSTACK, PUSH, POP, NTOP, TOP, POP_DECREF, GETCONST, GETNAMEV, GETLOCAL, SETLOCAL, JUMPBY, JUMPTO...) | 41 | core | none | P17 |  | Compile-time analogues of ceval.c's stack macros; the generated cases use them |
| unwind_except_handler | 19 | mixed | Python/ceval.c:UNWIND_EXCEPT_HANDLER | P12,P17 |  | Core part: compile-time block and stack; exc_info restored by cimpl_pop_exc_info |
| SAVE_NEXT_INSTR macro | 2 | core | none |  |  | Restart point |
| mark_varying | 17 | core | none |  | OTHER: a name marked as changing is loaded at run time (D25 cache policy) | Copy-on-write dict in po->pr |
| MISSING_OPCODE (commented out) | 6 | glue | none |  |  | Dead comment |
| psy_get_builtins | 30 | translated | partial: Objects/frameobject.c:_PyFrame_New_NoTrack (the builtins lookup) | P28 | DRIFT: one shared minimal builtins dict made once (3.7 makes a new one per frame); no back->f_builtins shortcut | Used by psyfunc.c for LOC_BUILTINS (F49) |
| global_guard_t, changed_global_t, DICT_ma_version_tag/GUARD_* descriptors | 35 | core | none | P15 | GUARD: dict version-tag guard (PEP 509, D25) |  |
| do_changed_global | 12 | core | none |  | GUARD: on a tag mismatch redoes the lookup; the same object resets the guard, else respawn | repairable-shared-guard shape |
| tag_changed | 18 | core | none | P15 | GUARD: tag xor expected tag |  |
| load_global_cached | 85 | core | partial: Python/ceval.c:TARGET(LOAD_GLOBAL) (the lookup) | P14,P15,P05 | GUARD: globals and builtins version tags; PROMOTE: the global becomes a compile-time constant; LIKELY: guard failure unlikely (respawn) | D25; ends the block (split_after_global) |
| psyco_ext_do_calls | 51 | mixed | partial: Python/ceval.c:call_function | P18,P22 | VIRTUAL: positional arguments as a virtual tuple, keywords as a dict; DRIFT: keywords in a dict, not kwnames (same semantics) | Core part: the call goes to PsycoObject_Call (inlining, direct calls) |
| cimpl_call_function_ex | 51 | translated | Python/ceval.c:TARGET(CALL_FUNCTION_EX) with check_args_iterable, format_kwargs_mapping_error | P12 | REF: holds a reference to kwargs during the call (ceval's stack holds one) |  |
| psyco_ext_do_call_ex | 33 | mixed | partial: Python/ceval.c:TARGET(CALL_FUNCTION_EX) | P01,P12 | PROMOTE: the type of the * argument; SHORTCUT: an exact tuple without ** goes to PsycoObject_Call; EFFECT: cimpl_call_function_ex CfCommonNewRefPyObject | Core part: the specialised call |
| error message macros, unbound_local_error | 15 | translated | Python/ceval.c:format_exc_check_arg (UNBOUNDLOCAL_ERROR_MSG, NAME_ERROR_MSG) | P24 |  |  |
| unbound_deref_error | 17 | translated | Python/ceval.c:format_exc_unbound | P24 |  |  |
| CELL_ob_ref descriptor | 2 | core | none | P15 | OTHER: FMUT (a cell can change under any call) | Field descriptor |
| cimpl_import_from | 53 | translated | Python/ceval.c:import_from | P12 |  | **Removed, generated (D88, `fa3dae7`)**: `ceval_import_from`, a run-time copy of the C (rule `helper_copy`); was: Copy (F44) |
| cimpl_exc_match | 17 | translated | partial: Python/ceval.c:cmp_outcome (PyCmp_EXC_MATCH) | P12 |  | F43 |
| cimpl_store_deref | 7 | translated | Python/ceval.c:TARGET(STORE_DEREF) | P12 | REF: increfs v (ceval steals it) | **Removed, generated (D88, `fa3dae7`)**: the case stores the cell inline (`field_store`) |
| cimpl_build_unpack | 34 | translated | Python/ceval.c:TARGET(BUILD_TUPLE_UNPACK_WITH_CALL)/(BUILD_TUPLE_UNPACK)/(BUILD_LIST_UNPACK), check_args_iterable | P12 |  | Loops over a tuple of parts instead of PEEK(i) |
| cimpl_build_map_unpack | 52 | translated | Python/ceval.c:TARGET(BUILD_MAP_UNPACK)/(BUILD_MAP_UNPACK_WITH_CALL), format_kwargs_mapping_error | P12 |  | **Removed, generated (D88, `fa3dae7`)**: one wrapper call per part (`error_handler`); was: Over a tuple of parts |
| cimpl_build_set_unpack | 14 | translated | Python/ceval.c:TARGET(BUILD_SET_UNPACK) | P12 |  |  |
| special_lookup | 10 | translated | Python/ceval.c:special_lookup |  |  | Copy |
| cimpl_setup_with | 25 | translated | partial: Python/ceval.c:TARGET(SETUP_WITH) (up to the block setup) | P12 | DRIFT: mgr stays on the stack while __enter__ runs | Results through an A array |
| cimpl_call_exit | 10 | translated | partial: Python/ceval.c:TARGET(WITH_CLEANUP_START) (the __exit__ call) | P12 |  |  |
| cimpl_load_build_class | 25 | translated | Python/ceval.c:TARGET(LOAD_BUILD_CLASS) | P12 |  |  |
| cimpl_delete_deref | 11 | translated | partial: Python/ceval.c:TARGET(DELETE_DEREF) | P12,P27 |  | **Removed, generated (D88, `fa3dae7`)**: the case stores NULL inline and releases with `psyco_decref_v`; was: Returns 1 for an empty cell; the caller raises |
| cimpl_load_global | 20 | translated | partial: Python/ceval.c:TARGET(LOAD_GLOBAL) | P12 | DRIFT: always PyDict_GetItem (3.7: _PyDict_LoadGlobal for exact dicts, else PyObject_GetItem), so lookup errors are swallowed and a dict subclass's __getitem__/__missing__ ignored; OTHER: RECLIMIT_SAFE_ENTER | The uncached path |
| unpack_iterable | 86 | translated | Python/ceval.c:unpack_iterable |  | DRIFT: fills sp[] upwards (the caller pushes in reverse) | 3.7 messages (F30) |
| cimpl_unpack_iterable | 4 | translated | Python/ceval.c:unpack_iterable | P12 |  | Wrapper (argcntafter -1) |
| cimpl_unpack_ex | 5 | translated | Python/ceval.c:TARGET(UNPACK_EX) | P12 |  | Wrapper |
| cimpl_unpack_list | 14 | translated | partial: Python/ceval.c:TARGET(UNPACK_SEQUENCE) (the list branch) | P12 | DRIFT: reached for list subclasses too (see UNPACK_SEQUENCE) |  |
| cimpl_reraise | 30 | translated | partial: Python/ceval.c:do_raise (exc NULL) with Python/errors.c:_PyErr_GetTopmostException | P27 | OTHER: records its own traceback entry on the RuntimeError path | F18 |
| cimpl_do_raise | 63 | translated | Python/ceval.c:do_raise | P12 | REF: takes its own references (does not consume its arguments) |  |
| cimpl_import_name | 26 | translated | partial: Python/ceval.c:import_name | P12 | DRIFT: always calls __import__ (no PyImport_ImportModuleLevelObject fast path); passes None as locals (3.7 passes f_locals at module level); keeps Py2's level -1 test |  |
| cimpl_import_all_from | 53 | translated | Python/ceval.c:import_all_from | P12 | DRIFT: Py2 shape (GetAttrString, AS_STRING[0] test, always PyDict_SetItem) | Called with globals as locals |
| compute_and_raise_exception | 34 | core | none | P12 | EFFECT: PyErr_Restore CfNoReturnValue | Raises a virtual exception for real |
| cimpl_format_value | 27 | translated | Python/ceval.c:TARGET(FORMAT_VALUE) | P12 |  | **Removed, generated (D88, `fa3dae7`)**: the conversion switch folded (`switch_arms`) |
| cimpl_build_string | 16 | translated | Python/ceval.c:TARGET(BUILD_STRING) | P12 | OTHER: up to four parts as arguments, else a tuple | **Removed, generated (D88, `fa3dae7`)**: `ceval_BUILD_STRING_0` (`residual_wrapper`, `ARRAY_ARGS_MAX`) |
| cimpl_minmax_step | 15 | translated | partial: Python/bltinmodule.c:min_max (one step, no key or default) | P12 |  | For fused generator-expression consumers (core policy) |
| cimpl_minmax_result | 10 | translated | partial: Python/bltinmodule.c:min_max (empty-sequence error) | P12 |  |  |
| cimpl_generator_stop | 23 | translated | partial: Objects/genobject.c:gen_send_ex (PEP 479) with Python/errors.c:_PyErr_ChainExceptions | P12 | DRIFT: the generator message only (coroutines and async generators never reach it) | For fused generator expressions |
| exit_function | 43 | core | none | P17 |  | Return or exception exit; PEP 479 for fused generator expressions |
| NI_CODESTATS: code-size state | 16 | core | none |  |  | Statistics build only |
| codestats_flush | 24 | core | none |  |  | NI_CODESTATS |
| codestats_begin | 25 | core | none |  |  | NI_CODESTATS; closes the #if |
| breaker_after_call | 23 | core | none |  | OTHER: eval-breaker test after a call only if a blocking C call was emitted (D45, D53) | runtime-poll |
| psyco_pycompiler_mainloop: prologue, instruction fetch, eval-breaker poll | 109 | core | partial: Python/ceval.c:_PyEval_EvalFrameDefault (the dispatch loop) |  | OTHER: recursion limit +50 while compiling; psyco_emit_header(po, 3) for try/except (room for Py2's f_exc_*) | runtime-poll; pending POP of a POP_JUMP target, merge check |
| #include pycompiler_gen.h (41 generated cases) | 3 | translated | Python/ceval.c:TARGET cases NOP, POP_TOP, ROT_TWO, ROT_THREE, DUP_TOP, DUP_TOP_TWO, LOAD_CONST, LOAD_FAST, STORE_FAST, UNARY_POSITIVE/NEGATIVE/INVERT, 13 BINARY_*, 13 INPLACE_*, GET_ITER, STORE_SUBSCR | P09 |  | Generated from tools/metac/bytecodes37.c (D26) |
| case UNARY_NOT | 13 | translated | Python/ceval.c:TARGET(UNARY_NOT) | P09,P23 |  | Hand-written; in bytecodes37.c but not generated (control flow) |
| case LIST_APPEND | 8 | translated | Python/ceval.c:TARGET(LIST_APPEND) | P09 |  | Hand-written |
| case DELETE_SUBSCR | 10 | translated | Python/ceval.c:TARGET(DELETE_SUBSCR) | P09 | OTHER: PyObject_DelItem as PsycoObject_SetItem(.., NULL) | Hand-written; in bytecodes37.c, not generated |
| case BREAK_LOOP | 4 | core | Python/ceval.c:TARGET(BREAK_LOOP) |  |  | Hand-written; EBreak pseudo-exception (WHY_BREAK) |
| case CONTINUE_LOOP | 5 | core | Python/ceval.c:TARGET(CONTINUE_LOOP) |  |  | Hand-written; EContinue |
| case RAISE_VARARGS | 28 | core | Python/ceval.c:TARGET(RAISE_VARARGS) | P12,P27 | EFFECT: cimpl_do_raise and cimpl_reraise CfPyErrAlways | Hand-written; exceptions; two commented MISSING_OPCODE lines |
| case RETURN_VALUE | 26 | mixed | Python/ceval.c:TARGET(RETURN_VALUE) | P12 | VIRTUAL: a fused generator expression returns its consumer's result (GENEXPR_*) | Hand-written; EReturn; core part the fusion |
| case FORMAT_VALUE | 24 | translated | Python/ceval.c:TARGET(FORMAT_VALUE) | P12,P08 | SHORTCUT: a known exact str with no conversion stays; RESTYPE: result asserted exact str | **Removed, generated (D88, `fa3dae7`)**; no result type (F98 fixed); a conversion is two calls; was: Hand-written; BUG [inferred]: a user __format__ may return a str subclass (PyObject_Format checks PyUnicode_Check, not exact) KNOWN_BUGS F98. |
| case BUILD_STRING | 27 | translated | Python/ceval.c:TARGET(BUILD_STRING) | P12,P08,P18 | RESTYPE: str | **Removed, generated (D88, `fa3dae7`)**; one call, as by hand; was: Hand-written; more than 4 parts through a virtual tuple |
| case YIELD_VALUE | 53 | mixed | Python/ceval.c:TARGET(YIELD_VALUE) in name only | P09,P12,P05 | VIRTUAL: generator expression fused with its consumer (list, set, sum, min/max, any/all); SHORTCUT: sum by PsycoNumber_Add when the type is known; LIKELY: any/all early exit unlikely | Hand-written; core part the fusion; sum follows bltinmodule.c:builtin_sum's generic loop |
| case RETURN_NONE (#ifdef) | 13 | glue | none |  |  | Dead: not a 3.7 opcode; two commented MISSING_OPCODE lines |
| case POP_BLOCK | 9 | core | Python/ceval.c:TARGET(POP_BLOCK) | P17 |  | Hand-written; block stack (UNWIND_BLOCK) |
| case POP_EXCEPT | 11 | core | Python/ceval.c:TARGET(POP_EXCEPT) | P12 |  | Hand-written; block stack, exc_info |
| case END_FINALLY | 39 | core | Python/ceval.c:TARGET(END_FINALLY) |  | DRIFT: a pseudo-exception's state is a 3-tuple, not ceval's WHY_* int | Hand-written; Ni's finally scheme |
| case UNPACK_SEQUENCE | 75 | mixed | Python/ceval.c:TARGET(UNPACK_SEQUENCE) | P01,P15,P05,P22,P12,P17 | SHORTCUT: a virtual tuple of the right length unpacked at compile time; REF: need_reference on the items; DRIFT: tuple and list fast paths taken for subclasses (PyType_TypeCheck; 3.7 uses CheckExact) | Hand-written; BUG [inferred]: a tuple or list subclass that overrides __iter__ is unpacked by its items KNOWN_BUGS F97. |
| case UNPACK_EX | 16 | translated | Python/ceval.c:TARGET(UNPACK_EX) | P12,P08 | RESTYPE: the starred target is a list | Hand-written |
| case STORE_ATTR | 12 | translated | Python/ceval.c:TARGET(STORE_ATTR) | P09 |  | Hand-written; the name as a constant vinfo |
| case DELETE_ATTR | 11 | translated | Python/ceval.c:TARGET(DELETE_ATTR) | P09 |  | Hand-written |
| case STORE_NAME, STORE_GLOBAL | 15 | mixed | Python/ceval.c:TARGET(STORE_GLOBAL), TARGET(STORE_NAME) | P12 | OTHER: mark_varying (LOAD_GLOBAL cache, D25); DRIFT: STORE_NAME assumes f_locals == f_globals (module code only) | Hand-written; core part mark_varying |
| case DELETE_NAME, DELETE_GLOBAL | 18 | mixed | Python/ceval.c:TARGET(DELETE_GLOBAL), TARGET(DELETE_NAME) | P12,P05,P24 | OTHER: mark_varying; DRIFT: DELETE_NAME on globals | Hand-written |
| case LOAD_NAME, LOAD_GLOBAL | 22 | mixed | Python/ceval.c:TARGET(LOAD_GLOBAL), TARGET(LOAD_NAME) | P12 | GUARD: version-tag cached constant (load_global_cached); DRIFT: LOAD_NAME on globals (module code) | Hand-written; core part the cache (D25) |
| case DELETE_FAST | 14 | translated | Python/ceval.c:TARGET(DELETE_FAST) | P24 |  | Hand-written |
| case LOAD_CLOSURE | 12 | translated | Python/ceval.c:TARGET(LOAD_CLOSURE) | P17 |  | Hand-written; leading comment on cells |
| case LOAD_DEREF | 20 | translated | Python/ceval.c:TARGET(LOAD_DEREF) | P15,P05,P17,P24 | REF: need_reference on the borrowed cell content | Hand-written |
| case STORE_DEREF | 7 | translated | Python/ceval.c:TARGET(STORE_DEREF) | P12 |  | **Removed, generated (D88, `fa3dae7`)**; was: Hand-written |
| case DELETE_DEREF | 13 | translated | Python/ceval.c:TARGET(DELETE_DEREF) | P12,P05,P24 |  | **Removed, generated (D88, `fa3dae7`)**; was: Hand-written |
| case LOAD_BUILD_CLASS | 10 | translated | Python/ceval.c:TARGET(LOAD_BUILD_CLASS) | P12 |  | Hand-written |
| case BUILD_TUPLE | 7 | translated | Python/ceval.c:TARGET(BUILD_TUPLE) | P18 | VIRTUAL: virtual tuple | Hand-written |
| case BUILD_TUPLE_UNPACK_WITH_CALL, BUILD_TUPLE_UNPACK, BUILD_LIST_UNPACK | 50 | mixed | Python/ceval.c:TARGET(BUILD_TUPLE_UNPACK_WITH_CALL)/(BUILD_TUPLE_UNPACK)/(BUILD_LIST_UNPACK) | P22,P18,P12,P08 | VIRTUAL: parts that are tuples with known items make a virtual tuple; RESTYPE: list or tuple | Hand-written; core part the virtual concatenation |
| case BUILD_LIST | 9 | translated | Python/ceval.c:TARGET(BUILD_LIST) | P18 | VIRTUAL: PsycoList_New (VLIST_LENGTH_MAX) | Hand-written |
| case BUILD_MAP | 19 | translated | Python/ceval.c:TARGET(BUILD_MAP) | P09 |  | Hand-written |
| case BUILD_CONST_KEY_MAP | 29 | translated | Python/ceval.c:TARGET(BUILD_CONST_KEY_MAP) | P24 |  | Hand-written (D88: a negative type check of `keys`, no resumable arm; METACOMPILER failures log) |
| case BUILD_MAP_UNPACK, BUILD_MAP_UNPACK_WITH_CALL, BUILD_SET_UNPACK | 30 | translated | Python/ceval.c:TARGET(BUILD_MAP_UNPACK)/(BUILD_MAP_UNPACK_WITH_CALL)/(BUILD_SET_UNPACK) | P12,P08 | RESTYPE: dict or set | BUILD_MAP_UNPACK* generated (D88), BUILD_SET_UNPACK (D81); was: Hand-written |
| case STORE_MAP (#ifdef) | 12 | glue | none |  |  | Dead: not a 3.7 opcode |
| case MAP_ADD | 12 | translated | Python/ceval.c:TARGET(MAP_ADD) | P09 |  | Hand-written (#ifdef; defined in 3.7) |
| case BUILD_SET | 22 | translated | Python/ceval.c:TARGET(BUILD_SET) | P12,P08 | RESTYPE: set | Hand-written |
| case SET_ADD | 8 | translated | Python/ceval.c:TARGET(SET_ADD) | P12 |  | Hand-written |
| case LOAD_ATTR | 10 | translated | Python/ceval.c:TARGET(LOAD_ATTR) | P09 |  | Hand-written |
| case LOAD_METHOD | 14 | mixed | Python/ceval.c:TARGET(LOAD_METHOD) | P09 | DRIFT: always pushes (NULL, attribute), never (method, self); VIRTUAL: relies on a virtual bound method | Hand-written; paired with CALL_METHOD |
| case COMPARE_OP | 48 | translated | Python/ceval.c:TARGET(COMPARE_OP) with cmp_outcome | P09,P23,P12 | SHORTCUT: is / is not as word compares | Hand-written |
| case IMPORT_NAME | 22 | translated | Python/ceval.c:TARGET(IMPORT_NAME) | P12 |  | Hand-written (D88: `import_name` takes the frame, core); the Py<2.5 branch is dead |
| case IMPORT_STAR | 14 | translated | Python/ceval.c:TARGET(IMPORT_STAR) | P12 | EFFECT: CfReturnTypeInt\|CfPyErrIfNonNull; DRIFT: globals as locals, no PyFrame_FastToLocals | Hand-written |
| case IMPORT_FROM | 9 | translated | Python/ceval.c:TARGET(IMPORT_FROM) | P12 |  | **Removed, generated (D88, `fa3dae7`)**; was: Hand-written |
| case JUMP_FORWARD | 5 | mixed | Python/ceval.c:TARGET(JUMP_FORWARD) |  |  | Hand-written; core part the merge-point lookup |
| case POP_JUMP_IF_FALSE, POP_JUMP_IF_TRUE, JUMP_IF_FALSE_OR_POP, JUMP_IF_TRUE_OR_POP | 58 | mixed | Python/ceval.c:TARGET(POP_JUMP_IF_FALSE)/(POP_JUMP_IF_TRUE)/(JUMP_IF_FALSE_OR_POP)/(JUMP_IF_TRUE_OR_POP) | P09,P05 | FORGET: Psyco_ForgetClasses before the branch to a merge point (D59) | Hand-written; core part psyco_compile_cond and NEXT_INSTR_POP; the Py<2.7 branches are dead |
| case JUMP_ABSOLUTE | 5 | mixed | Python/ceval.c:TARGET(JUMP_ABSOLUTE) |  |  | Hand-written; core part the merge point |
| case FOR_ITER | 22 | mixed | Python/ceval.c:TARGET(FOR_ITER) | P09,P05 | LIKELY: StopIteration expected (runtime_NON_NULL_t) | Hand-written; StopIteration as a pseudo-exception (core) |
| case SETUP_LOOP, SETUP_EXCEPT, SETUP_FINALLY | 6 | core | Python/ceval.c:TARGET(SETUP_LOOP)/(SETUP_EXCEPT)/(SETUP_FINALLY) |  |  | Hand-written; block stack |
| case SETUP_WITH | 20 | core | Python/ceval.c:TARGET(SETUP_WITH) | P12 |  | Hand-written; leading comment on Ni's finally states |
| case WITH_CLEANUP_START | 43 | core | Python/ceval.c:TARGET(WITH_CLEANUP_START) | P12 | DRIFT: tells its three entry states apart from the block stack (Ni's finally scheme) | Hand-written |
| case WITH_CLEANUP_FINISH | 23 | core | Python/ceval.c:TARGET(WITH_CLEANUP_FINISH) | P09,P05 | DRIFT: unwinds a silenced exception itself instead of WHY_SILENCED | Hand-written |
| case CALL_METHOD | 16 | core | Python/ceval.c:TARGET(CALL_METHOD) | P18 | OTHER: eval-breaker test after a blocking call | Hand-written; calls |
| case CALL_FUNCTION, CALL_FUNCTION_KW, CALL_FUNCTION_EX | 23 | core | Python/ceval.c:TARGET(CALL_FUNCTION)/(CALL_FUNCTION_KW)/(CALL_FUNCTION_EX) | P18 | OTHER: eval-breaker test after a blocking call | Hand-written; calls; commented MISSING_OPCODE(MAKE_CLOSURE) |
| case MAKE_FUNCTION | 30 | translated | Python/ceval.c:TARGET(MAKE_FUNCTION) | P18 | VIRTUAL: virtual function; psyco_forking limits the nested weight of closure, annotations and defaults | Hand-written; pops before psyco_forking can fail [read], harmless only because that can't promote [inferred] |
| case BUILD_SLICE | 18 | translated | Python/ceval.c:TARGET(BUILD_SLICE) | P18 | VIRTUAL: virtual slice | Hand-written |
| case EXTENDED_ARG | 5 | translated | Python/ceval.c:TARGET(EXTENDED_ARG) |  |  | Hand-written; dispatch |
| default (unknown opcode), end of switch, error exit | 10 | core | none |  |  | Py_FatalError; mergepoints.c refuses code with unlisted opcodes |
| label fine: forget classes, buffer margin, split after a global, merge points | 57 | core | none |  | FORGET: Psyco_ForgetClasses after each instruction (D59); VIRTUAL: psyco_limit_nested_weight at merge points | The specialiser's control loop |
| promotion pseudo-exception | 22 | core | none | P01,P03 |  | The restart |
| EInline pseudo-exception (enter an inlined frame) | 24 | core | none |  | FORGET: on entering an inlined frame | Inlining |
| traceback entry for a Python exception | 9 | core | partial: Python/ceval.c:_PyEval_EvalFrameDefault (error: PyTraceBack_Here) | P27 |  | Skips END_FINALLY and a bare raise (F18) |
| unwinding an inlined frame | 45 | core | none | P17 | REF: need_reference on the inlined return value | Inlining |
| block-stack unwind | 92 | mixed | partial: Python/ceval.c:_PyEval_EvalFrameDefault (fast_block_end) | P12,P18 | FORGET: after unwinding to a handler; DRIFT: a pseudo-exception entering a finally block is one 3-tuple; EFFECT: cimpl_push_exc_info CfNoReturnValue | Core part the pseudo-exceptions |
| function return and finish | 26 | core | none |  |  | exit_function loop; restores the saved Python error |

## Objects and the abstract layer

### `ni/Objects/pobject.c`

Mirrors `Objects/object.c`'s generic protocol: `PyObject_IsTrue`, `PyObject_GetAttr`/`SetAttr` and the generic attribute steps, `PyObject_Hash`, `do_richcompare`/`PyObject_RichCompare(Bool)` **[read]**. Most of the file by line count is core: the type-promotion primitives, D59/D62/D63 forgetting, the heap-slot guard, and the attribute-load and attribute-store guards, caches and inline split-dict paths **[read]**. `PsycoObject_Repr` has no callers, and its exact-str assertion is wrong for a `__repr__` that returns a str subclass **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment and includes | 27 | glue | none |  |  | Header comment lists the core parts (load/store guards, inline store, Psyco_HeapSlot). |
| Psyco_NeedType | 17 | core | none (promotes Include/object.h:Py_TYPE) | P01 | PROMOTE: type of every run-time value promoted (no mega cut-off); FORGET: type-hint guard first (D63), class-assignable constant checked (F96) | The promote-type primitive every translated file uses. |
| Psyco_KnownType | 14 | core | none |  | FORGET: a class-assignable constant's type counts as unknown (F96) | Never fails; ignores type_hint (D63). |
| forget_mutable_types | 45 | core | none | P31 | FORGET: drops known heap/ModuleType types, keeps hint (D59, D63) | Recursive over vlocals. |
| Psyco_TypeHintGuard | 38 | core | none | P05 | FORGET: re-learn forgotten type by a guard instead of a promotion (D63); LIKELY: type unchanged is likely (_f on change) |  |
| Psyco_ForgetClasses | 8 | core | none | P31 | FORGET: entry point, gated by po->pr.class_unchecked (D59) |  |
| Psyco_ForgetConstantClasses | 5 | core | none | P31 | FORGET: constants' checked types dropped at block start (D62) |  |
| Psyco_ConstantType | 56 | core | none | P05 | GUARD: chain of ob_type checks against types recorded per constant (F96, D62) | Global dict keyed by object address; records never freed. |
| slot_word_t, SLOT_value | 9 | core | none | P15 |  | Field descriptor for one word at an address. |
| heap_word | 38 | core | none | P05 | GUARD: value first seen at a slot address, run-time compare (F50, D48); REF: optionally holds the object for good | Global 'seen' dict keyed by slot address. |
| Psyco_HeapSlot | 5 | core | none | P29 | GUARD: heap type slot guard (D54) |  |
| Psyco_TypeIsSubtype | 23 | mixed | Objects/typeobject.c:PyType_IsSubtype | P29 | GUARD: tp_mro recorded and held, compared at run time (D54) | Core: the tp_mro guard; the mro scan is PyType_IsSubtype's. |
| PsycoObject_IsTrue | 49 | translated | Objects/object.c:PyObject_IsTrue | P01, P09, P29 | GUARD: nb_bool, mp_length, sq_length of a heap type via Psyco_HeapSlot, changed -> PyObject_IsTrue; SHORTCUT: None folded to 0; EFFECT: CfCommonInquiry (nb_bool), CfCommonPySSizeTResult (lengths); DRIFT: a length is returned as is, not normalised to 0/1; no Py_True/Py_False identity tests (same result via long_bool) | Static types: slots folded at compile time. |
| PsycoObject_Repr | 13 | translated | Objects/object.c:PyObject_Repr | P12, P08 | RESTYPE: exact str asserted; DRIFT: PyObject_Repr returns a str subclass unchanged (PyUnicode_Check), so the assert can be wrong | No callers [read]; residual only. |
| generic_getattr forward declaration | 3 | glue | none |  |  |  |
| need_type_mega | 25 | core | none | P04 | MEGA: type promoted for at most MEGAMORPHIC_MAX types per site; PROMOTE: module constants never specialised (F96); FORGET: hint guard first (D63) |  |
| PsycoObject_GetAttr | 22 | mixed | Objects/object.c:PyObject_GetAttr | P04, P12 | MEGA: via need_type_mega; SHORTCUT: specialised only for a constant exact-str name and a heap type or PyObject_GenericGetAttr; DRIFT: a static type's own tp_getattro is never dispatched to its meta (residual PyObject_GetAttr; same behaviour, no speed-up) | Core: the choice of when to specialise; then generic_getattr. |
| attr_guard_t and the load-guard design comment | 45 | core | none |  | GUARD: version-tag guard design (A9, D30, D60) | Comment explains the respawn rules for loads. |
| field_xor | 17 | core | none | P15 |  | Emitter helper. |
| type_changed | 6 | core | none |  | GUARD: tp_flags + tp_version_tag inline guard (x64/ipyencoding.c:psyco_type_guard) |  |
| cimpl_instance_dict_get | 8 | translated | partial: Objects/object.c:_PyObject_GenericGetAttrWithDict (instance-dict step: _PyObject_GetDictPtr + PyDict_GetItem) | P12 | EFFECT: called CfReturnTypePtr \| CfNoPython (F34: a key's Python __eq__ is not seen) | Run-time helper. |
| cimpl_dict_get_known | 11 | translated | partial: Objects/dictobject.c:_PyDict_GetItem_KnownHash (errors dropped as PyDict_GetItem does) | P12 | EFFECT: CfReturnTypePtr \| CfNoPython; OTHER: name hash computed at compile time | Run-time helper. |
| shared_key_slot | 34 | core | none (reads Objects/dict-common.h layout; nearest Objects/dictobject.c:lookdict_split) |  | SHORTCUT: inline split-dict load only when dk_refcnt > 1 (D31); REF: keys held for good by the code |  |
| keys_guard_t and field descriptors (KGUARD_*, INSTANCE_dict, DICT_ma_keys, DICT_ma_values, VALUES_item, KEYS_nentries) | 23 | core | none | P15 |  | Descriptors of a Ni struct and of dict internals. |
| keys_inline | 38 | core | none |  | OTHER: serial-gated decision whether to inline (D31) | Global dict keyed by type address. |
| keys_mismatch | 27 | core | none | P15 |  | Emitter helper. |
| instance_dict_get | 90 | mixed | partial: Objects/object.c:_PyObject_GenericGetAttrWithDict (dict step); Objects/dictobject.c:lookdict_split | P15, P05, P12 | SHORTCUT: inline shared-key load behind a per-site keys guard (D31); LIKELY: dict present likely, keys match likely; EFFECT: helpers CfReturnTypePtr \| CfNoPython; OTHER: hash computed at compile time | Core: the shared-keys guard and inline load; the helper fallbacks are translated. |
| c_descriptor, safe_descriptor | 11 | core | none | P26 | SHORTCUT: whitelist of builtin descriptor types whose __get__ runs no Python code |  |
| load_kind | 21 | mixed | partial: Objects/object.c:_PyObject_GenericGetAttrWithDict (descriptor lookup via _PyType_Lookup) | P26 | SHORTCUT: specialise only safe builtin descriptor types; GUARD: heap descriptor type -> generic; DRIFT: none (C descriptors used only when tp is a subtype of d_type, as descr_check requires) | Core: which kinds are specialised. |
| enable_version_tag | 44 | core | none (works around Objects/typeobject.c:type_mro_modified) |  | GUARD: gives a heap class with a metaclass the version tag 3.7 withholds (D60); OTHER: writes tp_flags of CPython types |  |
| load_guard_t and table statics | 15 | core | none |  | GUARD: shared per (type, name, kind) (D60) |  |
| load_guard_slot | 7 | core | none |  |  | Hash of (type, name, kind). |
| load_guard | 41 | core | none |  | GUARD: open-addressing table of guards, never freed (D60) |  |
| load_guard_set | 8 | core | none |  | GUARD |  |
| load statistics variables | 7 | glue | none |  |  | ni._stats() counters; NI_CODESTATS counters. |
| load_type_changed | 21 | core | none |  | GUARD: repairs the guard of the kind the type now gives (D60) |  |
| cimpl_load_type_changed | 4 | core | none |  | GUARD: failure path, then Objects/object.c:PyObject_GetAttr |  |
| cimpl_generic_load_type_changed | 5 | core | none |  | GUARD: failure path, then Objects/object.c:PyObject_GenericGetAttr |  |
| generic_getattr | 96 | mixed | Objects/object.c:_PyObject_GenericGetAttrWithDict | P04, P05, P09, P17, P12 | MEGA: need_type_mega; GUARD: version-tag guard shared per (type, name, kind); PROMOTE: a class attribute without __get__ becomes a compile-time constant; LIKELY: found in dict likely unless a class attribute exists; REF: need_reference on the borrowed dict value, descr held for good; OTHER: split_after_global ends the block | Core: the guard, kinds and repair; the step order is 3.7's. Not the 'suppress' variant. |
| PsycoObject_GenericGetAttr | 6 | translated | Objects/object.c:PyObject_GenericGetAttr | P09 |  | Meta registered on PyObject_GenericGetAttr. |
| cimpl_instance_dict_set | 13 | translated | partial: Objects/object.c:_PyObject_GenericSetAttrWithDict (dict step via _PyObjectDict_SetItem, KeyError -> AttributeError) |  |  | Run-time helper. |
| cimpl_dict_set_known | 10 | translated | partial: Objects/object.c:_PyObject_GenericSetAttrWithDict (combined-dict case of _PyObjectDict_SetItem, i.e. Objects/dictobject.c:_PyDict_SetItem_KnownHash) |  | OTHER: name hash computed at compile time | Run-time helper. |
| STORE_CACHE_MAX_REFRESHES, ni_dict_version, store statistics | 11 | core | none |  | OTHER: refresh cap 64 (D47); own ma_version_tag counter from 2^63 (D35) |  |
| store_cache_set | 18 | core | none |  | OTHER: content-checked cache (D38) |  |
| store_cache | 33 | core | none |  | OTHER: (class, name) cache, never freed (D38) |  |
| cimpl_store_attr_miss | 19 | core | none |  | OTHER: miss refreshes the cache (D47) | Calls cimpl_instance_dict_set. |
| cimpl_store_miss, store_miss_glue | 13 | mixed | partial: Objects/object.c:_PyObjectDict_SetItem (combined-dict branch) |  |  | Core: miss dispatch from the inline store. |
| store_kind | 19 | mixed | partial: Objects/object.c:_PyObject_GenericSetAttrWithDict (descriptor lookup, tp_descr_set test) |  | GUARD: a class attribute of a heap type counts as a data descriptor | Core: what a store site compiles to. |
| store_guard | 30 | core | none |  | GUARD: shared per (type, name, kind), never freed (D39) |  |
| store_guard_set | 7 | core | none |  | GUARD | Same body as load_guard_set. |
| cimpl_store_type_changed | 19 | core | none |  | GUARD: repair, then Objects/object.c:PyObject_SetAttr |  |
| init_store | 7 | core | none |  | OTHER: heuristic, a store to __init__'s first argument is likely the next empty slot |  |
| inline_store | 31 | mixed | partial: Objects/dictobject.c:insertdict (split-table branch; emitted by x64/ipyencoding.c:psyco_inline_store_begin) | P15a | FORGET: sets class_unchecked for the old value's dealloc (F55); SHORTCUT: GC-tracking choice from the value's known type; LIKELY: miss unlikely | Core: cache, GC shortcut, error check; the store order is insertdict's. |
| heap_setattr | 80 | mixed | Objects/object.c:_PyObject_GenericSetAttrWithDict; Objects/object.c:_PyObjectDict_SetItem | P05, P09, P12 | GUARD: version-tag store guard (D39); LIKELY: dict exists likely, combined dict likely; OTHER: compute_vinfo before the guard; split_after_global; name hash at compile time | Core: guard and choice of inline/known-hash/helper store. |
| PsycoObject_SetAttr | 49 | mixed | Objects/object.c:PyObject_SetAttr | P04, P09, P12 | MEGA: need_type_mega; SHORTCUT: specialised only for a constant exact-str name | Core: megamorphic promotion and heap_setattr dispatch; the tp_setattro/tp_setattr order is 3.7's. |
| swapped_op | 3 | translated | Objects/object.c:_Py_SwappedOp |  |  | Duplicate of a CPython table. |
| try_rich_compare | 68 | translated | partial: Objects/object.c:do_richcompare (the three slot calls) | P01, P29, P09, P25 | GUARD: tp_richcompare of a heap type, operand order via Psyco_TypeIsSubtype; EFFECT: CfCommonCheckNotImplemented | Changed path calls PyObject_RichCompare (no slot run yet). |
| opstrings | 2 | translated | Objects/object.c:opstrings |  |  |  |
| PsycoObject_RichCompare | 25 | translated | Objects/object.c:PyObject_RichCompare; partial: Objects/object.c:do_richcompare (default part) | P24, P23, P25 | VIRTUAL: identity result as a condition-code bool (P23); DRIFT: no Py_EnterRecursiveCall(' in comparison') |  |
| PsycoObject_RichCompareBool | 35 | translated | Objects/object.c:PyObject_RichCompareBool | P12 | PROMOTE: an unknown-type result's truth is a residual PyObject_IsTrue (temporaries can't be promoted); EFFECT: CfCommonInquiry; SHORTCUT: v == w for Py_EQ/Py_NE (same vinfo, else a run-time word compare unless one is virtual; F109, D84); DRIFT: no PyBool_Check fast path |  |
| PsycoObject_Hash | 21 | translated | Objects/object.c:PyObject_Hash | P01, P29, P09 | GUARD: tp_hash of a heap type; EFFECT: CfReturnTypeLong \| CfPyErrIfMinus1 | NULL slot (type not ready) goes residual, as CPython readies it. |
| collect_undeletable_vars | 30 | core | none | P26 | SHORTCUT: whitelist str, bool, float, int, None, range have no __del__ |  |
| Psyco_SafelyDeleteVar | 35 | core | none |  |  | For dispatcher.c. |
| psy_object_init | 8 | glue | none | P09 |  | Registers PyObject_GenericGetAttr's meta; builds store_miss_glue. |

### `ni/Objects/pobject.h`

The compile-time `object.h`: `ob_type`/`ob_size` field descriptors, promote/verify/assert-type primitives, `Psyco_TypeSlot`, and copies of 3.7's private dict-keys structs **[read]**. Almost all of it is core or declarations. `PsycoIter_Check`, `PsycoSequence_Check` and every `DEF_KNOWN_RET_TYPE_n` have no users **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, include guard, includes | 11 | glue | none |  |  |  |
| OB_type, FIX_size, VAR_size, FIX/VAR_signed_size, iOB_TYPE, iFIX_SIZE, iVAR_SIZE | 11 | core | none (from Include/object.h:PyObject, PyVarObject) | P15 | OTHER: ob_size mutability (FIX_ vs VAR_) and non-negativity chosen by hand | Field descriptors. |
| PyType_TypeCheck, PsycoIter_Check | 10 | translated | Include/object.h:PyObject_TypeCheck; Include/abstract.h:PyIter_Check |  |  | Type-level forms of the macros. PsycoIter_Check has no users [read]. |
| Psyco_SlotsMutable | 6 | core | none |  | GUARD: heap types' slots are guarded, static types' folded (F50) |  |
| Psyco_ClassMutable | 8 | core | none (fact from Objects/typeobject.c:object_set_class) | P31 | FORGET: heap types and ModuleType subtypes can change (F55) |  |
| Psyco_ForgetClasses, Psyco_TypeHintGuard declarations | 10 | glue | none |  |  |  |
| Psyco_ConstantClassMutable | 8 | core | none | P31 | FORGET: class-assignable constants (F96) |  |
| Psyco_ConstantType, Psyco_ForgetConstantClasses declarations | 11 | glue | none |  |  |  |
| Psyco_HeapSlot declaration | 7 | glue | none |  |  |  |
| Psyco_TypeSlot | 10 | core | none | P29 | GUARD: static slot read now, heap slot guarded (D54) |  |
| Psyco_TypeIsSubtype declaration | 7 | glue | none |  |  |  |
| PsycoSequence_Check | 4 | translated | Objects/abstract.c:PySequence_Check |  | DRIFT: no PyDict_Check exclusion | Type-level form; no users [read]. |
| Psyco_NeedType declaration | 4 | glue | none |  |  |  |
| Psyco_FastType | 14 | core | none | P01 | FORGET: asserts not used on a class-assignable constant without a checked type (F96) |  |
| Psyco_VerifyType | 9 | core | none | P02 |  |  |
| Psyco_KnownType declaration | 3 | glue | none |  |  |  |
| Psyco_AssertType | 7 | core | none | P08 |  |  |
| object_non_null | 11 | core | none | P05 | OTHER: a virtual or already-read pointer is assumed non-NULL |  |
| declarations of PsycoObject_* and statistics variables | 39 | glue | none |  |  |  |
| DEF_KNOWN_RET_TYPE_1..4, DEF_KNOWN_RET_TYPE_internal | 31 | core | none | P08, P12 | RESTYPE: macro to assert a residual call's result type | No users [read]. |
| IS_IMPLEMENTED, IS_NOTIMPLEMENTED | 8 | core | none | P25 |  |  |
| Psyco_SafelyDeleteVar declaration | 4 | glue | none |  |  |  |
| ni_dictkeys_t, ni_dictentry_t | 16 | translated | Objects/dict-common.h:struct _dictkeysobject, PyDictKeyEntry |  |  | Copies of 3.7's private structs (not installed headers). |
| store_cache_t | 14 | core | none |  | OTHER: content-checked (class, name) store cache (D38) |  |
| include guard end | 3 | glue | none |  |  |  |

### `ni/Objects/pabstract.c`

**Since `1aa473c` (D74)** all but `PsycoObject_Call`, `PsycoEval_CallObjectWithKeywords`, `type_error_tp`, the two `ob_size` lengths, `PsycoNumber_Power`, `PsycoObject_GetIter`, `cimpl_iternext`, `PsycoIter_Next` and the two generic subscripts are generated (`pabstract_gen.h`, by `tools/metac/gen_abstract.py`); the file is 297 lines. The rows below are the file as inventoried at `b36a1ac`. The `binary_iop1` BUG note is KNOWN_BUGS F101 (fixed); the `PsycoLong_AsLong` shortcut and DRIFT notes of GetItem, SetItem and `psequence_repeat` are F102 (fixed there; the generic subscripts keep it). [read]

Mirrors `Objects/abstract.c` (with `PyObject_Call` and `PyEval_CallObjectWithKeywords` from `Objects/call.c`) closely and almost mechanically: promote the type, read each slot through `Psyco_TypeSlot`, dispatch with `Psyco_META*` **[read]**. MAP's claim that `binary_op1` only dispatches when both types are the same (`NEW_STYLE_NUMBER`) is stale: the code is now 3.7's algorithm **[read]**. `binary_op1`, `binary_iop1` and `sequence_repeat` are each translated twice, once as a meta and once as a run-time `cimpl_` copy for the path where a slot changed **[read]**. `PsycoSequence_GetSlice`, `SetSlice`, `Tuple` and `PsycoEval_CallObjectWithKeywords` have no callers **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment and includes | 14 | glue | none |  |  |  |
| translation note, CF_AS_SSIZE_T | 7 | core | none |  | EFFECT: PyNumber_AsSsize_t is CfReturnTypeNative \| CfPyErrCheckMinus1 | Error convention of one CPython function, as data. |
| type_error_tp | 8 | translated | Objects/abstract.c:type_error | P24 |  | Takes the type, not the object. |
| table_slot, NB_SLOT_OF, SQ_SLOT_OF, MP_SLOT_OF, INDEX_SLOT_OF, SLOT_OR_GOTO | 35 | core | none | P29 | GUARD: every slot read of a possibly heap type (D54) |  |
| PsycoObject_Call | 24 | translated | partial: Objects/call.c:PyObject_Call (tp_call branch) | P01, P29, P09, P24 | GUARD: tp_call of a heap type, changed -> PyObject_Call; EFFECT: CfCommonNewRefPyObject; DRIFT: no Py_EnterRecursiveCall and no _Py_CheckFunctionResult around tp_call; kw is 0 for none | PyFunction/PyCFunction branches reach the same metas via tp_call [inferred]. |
| PsycoEval_CallObjectWithKeywords | 45 | translated | Objects/call.c:PyEval_CallObjectWithKeywords | P02, P12 |  | Wrong arg types go residual (CPython raises there). No callers [read]. |
| PsycoObject_GetItem | 59 | translated | Objects/abstract.c:PyObject_GetItem | P01, P29, P09, P12, P24 | GUARD: mp_subscript, sq_item, key's nb_index; SHORTCUT: int key converted inline (PsycoLong_AsLong); DRIFT: int key too big raises OverflowError (PyLong_AsLong) where 3.7 raises IndexError (PyNumber_AsSsize_t(key, PyExc_IndexError)); __class_getitem__ path is residual PyObject_GetItem | KNOWN_BUGS F102. |
| PsycoObject_SetItem | 67 | translated | Objects/abstract.c:PyObject_SetItem; Objects/abstract.c:PyObject_DelItem | P01, P29, P09, P12, P24 | GUARD: mp_ass_subscript, key's nb_index, sq_ass_item; SHORTCUT: int key via PsycoLong_AsLong; DRIFT: same OverflowError-for-IndexError as GetItem | value NULL is DelItem. KNOWN_BUGS F102. |
| PsycoObject_Size | 23 | translated | Objects/abstract.c:PyObject_Size; Objects/abstract.c:PyMapping_Size | P01, P29, P09, P24 | GUARD: sq_length, mp_length; EFFECT: CfCommonPySSizeTResult |  |
| psyco_generic_immut_ob_size | 5 | translated | Objects/tupleobject.c:tuplelength (and bytes/str length) | P15 | OTHER: ob_size chosen immutable (FIX_size) | Registered on tuple's sq_length; bytes' is generated (D84). |
| psyco_generic_mut_ob_size | 5 | translated | Objects/listobject.c:list_length | P15 | OTHER: ob_size chosen mutable (VAR_size) | No longer registered: list's sq_length is the generated `list_length` (D84, same read). |
| PsycoSequence_GetItem | 40 | translated | Objects/abstract.c:PySequence_GetItem | P01, P29, P05, P06, P09, P24 | GUARD: sq_item, sq_length; LIKELY: negative index unlikely (_f) |  |
| PsycoSequence_SetItem | 52 | translated | Objects/abstract.c:PySequence_SetItem; Objects/abstract.c:PySequence_DelItem | P01, P29, P05, P06, P09, P24 | GUARD: sq_ass_item, sq_length; LIKELY: negative index unlikely | value NULL is DelItem, with its own message. |
| PsycoSequence_GetSlice | 9 | translated | Objects/abstract.c:PySequence_GetSlice | P12 |  | Residual only. No callers [read]. |
| PsycoSequence_SetSlice | 12 | translated | Objects/abstract.c:PySequence_SetSlice; Objects/abstract.c:PySequence_DelSlice | P12 |  | Residual only. No callers [read]. |
| PsycoSequence_Contains | 20 | translated | Objects/abstract.c:PySequence_Contains | P01, P29, P09, P12 | GUARD: sq_contains; EFFECT: CfCommonIntBoolOrError | Fallback is residual _PySequence_IterSearch, as 3.7. |
| PsycoSequence_Tuple | 13 | generated (D78) | Objects/abstract.c:PySequence_Tuple | P12, P08 | RESTYPE: exact tuple (policy `restype`) | Hand-written definition removed; the generated one keeps the C's exact-tuple and list tests and calls PySequence_Tuple otherwise. No callers [read]. |
| NB_SLOT, NB_BINOP, NB_TERNOP | 5 | translated | Objects/abstract.c:NB_SLOT, NB_BINOP, NB_TERNOP |  |  | Same macros as CPython. |
| unary_op | 22 | translated | Objects/abstract.c:PyNumber_Negative (shape of Positive, Negative, Invert, Absolute) | P01, P29, P09, P24 | GUARD: the nb_ slot of a heap type, changed -> the PyNumber_ function | Factors four CPython functions. |
| PsycoNumber_Positive | 6 | translated | Objects/abstract.c:PyNumber_Positive |  |  |  |
| PsycoNumber_Negative | 6 | translated | Objects/abstract.c:PyNumber_Negative |  |  |  |
| PsycoNumber_Invert | 6 | translated | Objects/abstract.c:PyNumber_Invert |  |  |  |
| PsycoNumber_Absolute | 6 | translated | Objects/abstract.c:PyNumber_Absolute |  |  |  |
| cimpl_binary_op1 | 31 | translated | Objects/abstract.c:binary_op1 | P29 |  | Run-time C copy, called once a heap slot changed. Second translation of binary_op1 in this file. |
| binary_op1 | 66 | translated | Objects/abstract.c:binary_op1 | P01, P29, P09, P25 | GUARD: both nb_ slots and the operand-order subtype test (Psyco_TypeIsSubtype); EFFECT: CfCommonCheckNotImplemented | Both types promoted before any call (restart-safe). |
| binop_type_error | 8 | translated | Objects/abstract.c:binop_type_error | P24 | DRIFT: '%s' instead of '%.100s' (op name and type names not truncated) |  |
| binary_op | 10 | translated | partial: Objects/abstract.c:binary_op (without the >> print hint) | P25 |  | The hint is in PsycoNumber_Rshift. |
| BINARY_FUNC; PsycoNumber_Or, Xor, And, Lshift | 10 | translated | Objects/abstract.c:BINARY_FUNC (PyNumber_Or, PyNumber_Xor, PyNumber_And, PyNumber_Lshift) |  |  |  |
| cimpl_rshift_error | 14 | translated | partial: Objects/abstract.c:binary_op (nb_rshift print hint) |  |  | Run-time helper (F46). |
| PsycoNumber_Rshift | 13 | translated | Objects/abstract.c:PyNumber_Rshift (BINARY_FUNC) + binary_op's hint | P25, P12 |  | Hint test hoisted to the exact type PyCFunction_Type, name tested at run time. |
| PsycoNumber_Subtract, Divmod, FloorDivide, TrueDivide, MatrixMultiply | 8 | translated | Objects/abstract.c:PyNumber_Subtract, PyNumber_Divmod (BINARY_FUNC), PyNumber_FloorDivide, PyNumber_TrueDivide, PyNumber_MatrixMultiply |  |  | Stale Py2 'tp_flags test' comment. |
| SEQ_ADD/SEQ_MUL/SEQ_IADD/SEQ_IMUL enum | 6 | core | none |  |  | Selector for cimpl_sequence_op. |
| cimpl_sequence_repeat | 14 | translated | Objects/abstract.c:sequence_repeat |  |  | Run-time C copy. Second translation of sequence_repeat (see psequence_repeat). |
| cimpl_sequence_op | 37 | translated | partial: Objects/abstract.c:PyNumber_Add, PyNumber_Multiply, PyNumber_InPlaceAdd, PyNumber_InPlaceMultiply (sequence fallbacks) | P29 |  | Run-time C copy of four tails; one message format for all four (3.7's binop_type_error). |
| sequence_op_changed | 6 | core | none | P12, P29 |  | Residual call to cimpl_sequence_op. |
| SQ_SLOT_OR | 11 | core | none | P29 | GUARD: sequence slot, changed -> cimpl_sequence_op |  |
| PsycoNumber_Add | 16 | translated | Objects/abstract.c:PyNumber_Add | P25, P29, P09 | GUARD: sq_concat; DRIFT: v's type after a slot ran is the one promoted before (a user __add__ assigning __class__ is not seen) [inferred] |  |
| psequence_repeat | 30 | translated | Objects/abstract.c:sequence_repeat | P29, P09, P12 | GUARD: n's nb_index; SHORTCUT: int count via PsycoLong_AsLong; DRIFT: an int count too big gives PyLong_AsLong's OverflowError message, not 'cannot fit 'int' into an index-sized integer' | KNOWN_BUGS F102. |
| PsycoNumber_Multiply | 26 | translated | Objects/abstract.c:PyNumber_Multiply | P25, P29, P09 | GUARD: sq_repeat of both | Stale Py2.2/2.3 comment. |
| PsycoNumber_Remainder | 5 | translated | Objects/abstract.c:PyNumber_Remainder |  |  |  |
| PsycoNumber_Power | 36 | mixed | partial: Objects/abstract.c:PyNumber_Power (ternary_op) | P01, P26, P09, P12 | SHORTCUT: only exact int/float operands, calling float's or int's nb_power directly; everything else residual PyNumber_Power | Core: the whitelist. Third operand never dispatched. |
| cimpl_binary_iop1 | 16 | translated | Objects/abstract.c:binary_iop1 | P29 |  | Run-time C copy (second translation of binary_iop1). |
| binary_iop1 | 26 | translated | Objects/abstract.c:binary_iop1 | P01, P29, P09, P25 | GUARD: v's inplace slot; EFFECT: CfCommonCheckNotImplemented | BUG [inferred]: w's type is promoted (in binary_op1) only after v's inplace slot call is emitted, against restart rule 1. KNOWN_BUGS F101. |
| binary_iop | 11 | translated | Objects/abstract.c:binary_iop | P25 |  |  |
| INPLACE_BINOP; PsycoNumber_InPlaceOr..InPlaceMatrixMultiply | 20 | translated | Objects/abstract.c:INPLACE_BINOP instances, PyNumber_InPlaceFloorDivide, PyNumber_InPlaceTrueDivide, PyNumber_InPlaceMatrixMultiply |  |  | 3.7's own INPLACE_BINOP line misnames one function PyNumber_InMatrixMultiply; the real one is a separate function. |
| PsycoNumber_InPlaceAdd | 17 | translated | Objects/abstract.c:PyNumber_InPlaceAdd | P25, P29, P09 | GUARD: sq_inplace_concat, sq_concat |  |
| PsycoNumber_InPlaceMultiply | 28 | translated | Objects/abstract.c:PyNumber_InPlaceMultiply | P25, P29, P09 | GUARD: sq_inplace_repeat, sq_repeat |  |
| PsycoNumber_InPlaceRemainder | 6 | translated | Objects/abstract.c:PyNumber_InPlaceRemainder |  |  |  |
| PsycoNumber_InPlacePower | 8 | translated | Objects/abstract.c:PyNumber_InPlacePower | P12 |  | Residual only. |
| PsycoObject_GetIter | 39 | translated | Objects/abstract.c:PyObject_GetIter | P01, P29, P09, P18 | GUARD: tp_iter, sq_item; SHORTCUT: a static type's tp_iter result is not checked with PyIter_Check; VIRTUAL: no tp_iter gives a virtual seq-iterator (PsycoSeqIter_New); DRIFT: PySequence_Check's dict exclusion omitted (unreachable for dicts, which have tp_iter) | A heap type's tp_iter is always the residual PyObject_GetIter. |
| cimpl_iternext | 14 | translated | partial: Python/ceval.c:FOR_ITER (tp_iternext call) |  |  | Run-time helper. |
| PsycoIter_Next | 23 | translated | partial: Objects/abstract.c:PyIter_Next (without the StopIteration clear); Python/ceval.c:FOR_ITER | P01, P29, P09 | GUARD: tp_iternext; EFFECT: CfCommonIterNext |  |
| psyco_generic_subscript | 32 | translated | partial: Objects/tupleobject.c:tuplesubscript (index branch); Objects/listobject.c:list_subscript (index branch) | P01, P12 | SHORTCUT: int key via PsycoNumber_AsSsize_t(key, PyExc_IndexError), as 3.7 (F102 fixed, D78); DRIFT: negative index adds sq_length of o's own type (a subclass's __len__ runs) where CPython adds Py_SIZE | Meta on tuple's mp_subscript; called by plist_subscript. KNOWN_BUGS F103. |
| psyco_generic_ass_subscript | 30 | translated | partial: Objects/listobject.c:list_ass_subscript (index branch) | P01, P12 | SHORTCUT: int key via PsycoNumber_AsSsize_t (F102 fixed, D78); DRIFT: as psyco_generic_subscript (subclass __len__) | Called by plist_ass_subscript. KNOWN_BUGS F103. |

### `ni/Objects/pabstract.h`

Declarations only **[read]**. `PsycoNumber_Divide` and `PsycoNumber_InPlaceDivide` are declared but not defined **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, include guard, includes | 12 | glue | none |  |  |  |
| PsycoObject_Call, PsycoEval_CallObjectWithKeywords declarations | 7 | glue | none |  |  |  |
| PsycoObject_Size/GetItem/SetItem declarations | 5 | glue | none |  |  |  |
| PsycoSequence_* declarations | 12 | glue | none |  |  |  |
| PsycoNumber_* declarations | 57 | glue | none |  | DRIFT: PsycoNumber_Divide and PsycoNumber_InPlaceDivide declared, never defined (Py2 nb_divide) |  |
| PsycoIter_Next, PsycoObject_GetIter declarations | 9 | glue | none |  |  |  |
| psyco_generic_* declarations | 11 | glue | none |  |  | Comment still says Python 2.3. |
| include guard end | 2 | glue | none |  |  |  |

## Numbers, builtins, `math`, Py2 strings

### `ni/Objects/plongobject.c`

Virtual ints on the one-digit fast paths of 3.7's `longobject.c`. `plongobject_gen.h` (by `gen_abstract.py`'s long unit since D83, was `gen_long.py`) supplies `long_add`, `long_sub`, `long_mul`, `long_neg`, `long_invert`, `long_bool`, `long_long` (unary +), `long_abs`, `long_true_divide` and `long_pow`; no slot is both generated and hand-written here **[read]**. The hand-written `& | ^ << >>`, the `divmod()` fast path and the power-of-two divisor rewrite are Ni's own shortcuts with no 3.7 counterpart, so they are policy, not translation **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header comment and includes | 21 | glue | none |  |  | Header says which slots are generated and why the rest are hand-written |
| plong_medium_value | 52 | mixed | Objects/longobject.c:MEDIUM_VALUE (with the Py_ABS(Py_SIZE(x)) <= 1 tests of long_add etc.) | P15,P05,P07 | SHORTCUT: -1 <= ob_size <= 1 and the virtual word range test each as one unsigned compare; LIKELY: one digit is _t, zero is _f | Core part: the virtual-int branch (a word range test, there is no ob_size). Used by plongobject_gen.h, psliceobject.c, prangeobject.c |
| PsycoLong_MediumValue | 5 | glue | none |  |  | Exported wrapper of plong_medium_value |
| PsycoLong_AsLong | 11 | mixed | Objects/longobject.c:PyLong_AsLong | P12 | EFFECT: CfReturnTypeLong\|CfPyErrCheckMinus1 | Core part: a virtual int's word is returned directly; otherwise residual |
| PsycoLong_AsIndex (since `1aa473c`: `PsycoNumber_AsSsize_t(po, v, err)`, also used by the generated `sequence_repeat` and item functions; a known int subclass goes through `plong_medium_value`) | 29 | mixed | Objects/abstract.c:PyNumber_AsSsize_t (err=IndexError) | P14,P12 | SHORTCUT: compile-time fold of a constant, inline for a virtual or one-digit int; EFFECT: CfReturnTypeNative\|CfPyErrCheckMinus1 | Core part: virtual word; a constant that overflows is left to raise at run time. Used by punicodeobject.c, pbytesobject.c |
| PsycoLong_AsDouble | 18 | mixed | Objects/longobject.c:PyLong_AsDouble | P13,P12 | EFFECT: CfPure\|CfReturnTypeDouble on cimpl_fp_from_long, CfPyErrCheck\|CfReturnTypeDouble on PyLong_AsDouble | Core part: a virtual int's word converts by (double)long, the same rounding as PyLong_AsDouble [read] |
| PsycoLong_FromUnsignedLong | 9 | translated | Objects/longobject.c:PyLong_FromUnsignedLong | P12,P08 | RESTYPE: int | Residual; used by pstructmember.c |
| PsycoLong_FromLong | 6 | core | none | P18 | VIRTUAL: virtual int | Virtual PyLong_FromLong (incref). No callers outside this file [read]: dead |
| PsycoLong_FROM_LONG | 5 | core | none | P18 | VIRTUAL: virtual int | Virtual PyLong_FromLong consuming v |
| compute_long, direct_compute_long, psyco_computed_long | 27 | core | none | P18 | EFFECT: CfPure on PyLong_FromLong | Virtual-int forcing |
| compute_bool, direct_compute_bool, psyco_computed_bool | 22 | core | none | P18,P23 | EFFECT: CfPure on PyBool_FromLong | Virtual-bool forcing (pboolobject.h builds them) |
| both_longs | 10 | translated | Objects/longobject.c:CHECK_BINOP | P02 |  | PyLong_Check on both |
| plong_fallback2 (plong_fallback1 removed, D83) | 8 | translated | none (residual call of the int slot itself) | P12,P08 | RESTYPE: int | The whole CPython slot is called (nothing has run yet), so no P29 resume point is needed |
| #include plongobject_gen.h | 3 | glue | none |  |  | Generated: long_add, long_sub, long_mul, long_neg, long_invert, long_bool, long_long, long_abs, long_true_divide, long_pow (D83) |
| cimpl_long_hash1, plong_hash | 17 | translated | partial: Objects/longobject.c:long_hash (cases -1, 0, 1) | P13,P12 | SHORTCUT: one-digit path; EFFECT: CfPure\|CfReturnTypeLong on the helper, CfReturnTypeLong\|CfPyErrIfMinus1 on tp_hash | -1 hashes to -2 as in long_hash [read] |
| plong_richcompare | 33 | mixed | partial: Objects/longobject.c:long_richcompare (long_compare on one digit) | P02,P25,P05,P12,P23 | SHORTCUT: one-digit compare as a word compare; RESTYPE: bool (virtual); DRIFT: no self == other test (same result for ints) | Core part: medium values of virtual ints. Not one digit: residual tp_richcompare |
| plong_bitop | 46 | mixed | partial: Objects/longobject.c:long_and, long_or, long_xor, long_lshift, long_rshift (CHECK_BINOP and the fallback only) | P02,P05,P07,P12 | SHORTCUT: one-digit & \| ^ << >> as word ops, not in 3.7's C (3.12 has it for & \| ^); LIKELY: << count <= 32 is _t | Policy, not translation: a generator from 3.7 can't produce the fast path |
| BITOP: plong_and, plong_or, plong_xor, plong_lshift, plong_rshift | 12 | glue | none |  |  | Thin wrappers binding the slot |
| cimpl_floor_div, cimpl_floor_mod | 20 | translated | Objects/longobject.c:fast_floor_div, fast_mod (on words) | P13 |  | Python floor rounding on C longs |
| floor_divmod_words | 46 | mixed | partial: Objects/longobject.c:fast_floor_div, fast_mod | P13,P14,P05 | SHORTCUT: constant power-of-two divisor as shift and mask; EFFECT: CfPure\|CfReturnTypeLong on the helpers; LIKELY: divisor != 0 is _t | Core part: the power-of-two rewrite. Zero divisor: real slot raises |
| divmod_fallback | 8 | translated | none (residual call of the int slot) | P12,P08 | RESTYPE: int except for divmod (tuple, not asserted) |  |
| plong_divmod_op | 38 | mixed | partial: Objects/longobject.c:long_div, long_mod, long_divmod | P02,P25,P12,P18 | SHORTCUT: one-digit path also when the dividend is 0, and for divmod(), which has no fast path in 3.7's long_divmod; VIRTUAL: divmod result is a virtual tuple of virtual ints | 3.7 long_div/long_mod need Py_ABS(Py_SIZE) == 1 on both [read] |
| plong_div, plong_mod, plong_divmod | 13 | glue | none |  |  | Thin wrappers binding the slot |
| psy_longobject_init | 26 | glue | none |  |  | Registers the generated slots, then 15 hand-written metas |

### `ni/Objects/plongobject.h`

Field descriptors for `PyLongObject` plus the virtual int, whose `iLONG_VALUE` word is not a C field **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header, include guard, includes | 10 | glue | none |  |  |  |
| PsycoLong_Check | 2 | translated | Include/longobject.h:PyLong_Check |  |  | On a type, not an object |
| LONG_ob_size, LONG_ob_digit0, iLONG_OB_DIGIT0 | 6 | translated | Include/longintrepr.h:struct _longobject | P15 |  | Field descriptors; immutability (FIX_) is judgement |
| iLONG_VALUE, LONG_TOTAL | 6 | core | none | P18 |  | Virtual-only slot, not a C field |
| psyco_computed_long/bool, PsycoLong_IsVirtual | 11 | core | none | P18 |  |  |
| PsycoLong_FROM_WORD | 10 | core | none | P18 | VIRTUAL: virtual int | Virtual PyLong_FromLong |
| declarations | 12 | glue | none |  |  |  |

### `ni/Objects/pfloatobject.c`

Virtual floats plus the helpers `pbltinmodule.c` uses for `int(x)` and `round(x)`. Since D87 `pfloatslots_gen.h` (by `gen_abstract.py`'s float unit; `gen_float.py` and `pfloatobject_gen.h` are gone) supplies `float_add`, `float_sub`, `float_mul`, `float_rem`, `float_neg`, `float_abs`, `float_bool`, `float_div`, `float_float` and `PsycoFloat_AsDouble`, and the hand `PsycoFloat_AsDouble` and `psyco_convert_to_double` rows below are removed; before D87 `pfloatobject_gen.h` supplied the first eight; the hand-written part is `float_richcompare` (float/float only) and `float_hash`, so nothing is done twice **[read]**. `PsycoFloat_AsDouble` has no callers **[read]**. `pfloat_richcompare`'s residual call to compare with an int was flagged NoError, but `float_richcompare` can return NULL there: fixed (F104, D83) **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header comment and includes | 18 | glue | none |  |  |  |
| #if HAVE_FP_FN_CALLS, CF_NONPURE_FP_HELPER, CF_PURE_FP_HELPER | 10 | glue | none |  |  | Both CF_* macros are unused [read]: dead |
| cimpl_fp_eq_fp, _ne_, _le_, _lt_ | 6 | translated | partial: Objects/floatobject.c:float_richcompare (the double compares) | P13 |  |  |
| cimpl_fp_from_long | 2 | translated | partial: Objects/longobject.c:PyLong_AsDouble (one-digit case) | P13 |  | Also used by plongobject.c |
| cimpl_fp_from_float | 2 | translated | partial: Python/structmember.c:PyMember_GetOne (T_FLOAT) | P13 |  | Takes a float but is called with tag 'd'; works because the float's bits are the low 32 bits of the word [inferred] |
| cimpl_fp_fits_long, cimpl_fp_to_long | 6 | translated | partial: Objects/floatobject.c:float___trunc___impl (LONG_MIN < x < LONG_MAX) | P13 |  |  |
| cimpl_fp_trunc | 7 | translated | partial: Objects/floatobject.c:float___trunc___impl (modf) | P13 |  |  |
| cimpl_fp_round_even | 8 | translated | partial: Objects/floatobject.c:float___round___impl (no ndigits) | P13 |  | Same code as 3.7 [read] |
| PsycoFloat_Trunc | 6 | translated | partial: Objects/floatobject.c:float___trunc___impl | P13 | EFFECT: CfPure\|CfReturnTypeDouble | Used by pbltinmodule.c (int(x)) |
| PsycoFloat_RoundEven | 6 | translated | partial: Objects/floatobject.c:float___round___impl | P13 | EFFECT: CfPure\|CfReturnTypeDouble | Used by pbltinmodule.c (round(x)) |
| PsycoFloat_WholeToLong | 20 | mixed | partial: Objects/floatobject.c:float___trunc___impl (fits-long fast path) | P13,P05,P18 | EFFECT: CfPure on both helpers; VIRTUAL: virtual int result; LIKELY: fits is _t | Core part: virtual int; does not fit -> caller asks CPython |
| PsycoFloat_FromFloat | 11 | translated | partial: Python/structmember.c:PyMember_GetOne (T_FLOAT) | P13,P18 | EFFECT: CfPure\|CfReturnTypeDouble | Used by pstructmember.c |
| PsycoFloat_AsDouble | 37 | translated | Objects/floatobject.c:PyFloat_AsDouble | P01,P09,P15,P24 | DRIFT: message 'a float is required' (3.7: 'must be real number, not %.50s'), __float__'s result not checked to be a float; GUARD: nb_float read without Psyco_TypeSlot | No callers [read]: dead. Removed in D87: generated from the C instead |
| compute_float | 21 | core | none | P18 | EFFECT: CfPure on PyFloat_FromDouble |  |
| direct_compute_float, psyco_computed_float | 14 | core | none | P18 |  | Unreachable (KNOWN_BUGS F6) |
| psyco_convert_to_double | 20 | translated | Objects/floatobject.c:convert_to_double, CONVERT_TO_DOUBLE | P01,P15 | DRIFT: tests int before float (no observable difference) | Removed in D87: the generated slots inline `convert_to_double` (rule `inline_static`); pmath.c no longer uses it |
| pfloat_richcompare | 63 | mixed | partial: Objects/floatobject.c:float_richcompare (float/float case) | P01,P13,P12,P26,P23,P25 | SHORTCUT: float/float only (P26), int is the residual slot; EFFECT: CfCommonNewRefPyObject\|CfPure on the residual float_richcompare (NoError until F104's fix); RESTYPE: bool (virtual) | Core part: GE/GT as swapped LE/LT. BUG [read]: float_richcompare with a big int can return NULL (its Error: label, MemoryError), but NoError means the NULL is not checked KNOWN_BUGS F104. |
| pfloat_hash | 9 | translated | Objects/floatobject.c:float_hash | P15,P13 | EFFECT: CfPure\|CfReturnTypeLong on _Py_HashDouble |  |
| #include pfloatobject_gen.h | 3 | glue | none |  |  | Removed in D87: `pfloatslots_gen.h` has the slots |
| psy_floatobject_init | 15 | glue | none |  |  | Registers the generated slots, then nb_positive, tp_richcompare, tp_hash |
| #else psy_floatobject_init (no FP calls) | 5 | glue | none |  |  |  |

### `ni/Objects/pfloatobject.h`

The `ob_fval` descriptor and the virtual-float builder; one word holds the double **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header, include guard, HAVE_FP_FN_CALLS, includes | 14 | glue | none |  |  |  |
| FLOAT_ob_fval, iFLOAT_OB_FVAL, FLOAT_TOTAL, PsycoFloat_Check | 7 | translated | Include/floatobject.h:PyFloatObject, PyFloat_Check | P15 |  | One vinfo for the double (upstream i386 used two) |
| psyco_computed_float, PsycoFloat_FROM_DOUBLE, PsycoFloat_FromDouble | 23 | core | none | P18 | VIRTUAL: virtual float | Comment 'not-yet-computed integers' is stale |
| PsycoFloat_FromFloat declaration | 2 | glue | none |  |  |  |
| PsycoFloat_AS_DOUBLE | 5 | translated | Include/floatobject.h:PyFloat_AS_DOUBLE | P15 |  | psyco_get_const: ob_fval treated as immutable |
| declarations, commented-out prototypes, #endif | 37 | glue | none |  |  |  |

### `ni/Modules/pmath.c`

One-argument `math` functions as macros that mirror `FUNC1`/`math_1_to_whatever`, plus `ceil`/`floor` for an exact float **[read]**. Conversion goes through `psyco_convert_to_double` (float's `CONVERT_TO_DOUBLE`) instead of `PyFloat_AsDouble`, so an int subclass's `__float__` is ignored **[read]**. **Since D87** the twelve `FUNC1` metas are generated (`pmathmodule_gen.h`, by `gen_builtins.py`'s math unit, with `PsycoFloat_AsDouble`: F99 fixed); what is left is `ceil`/`floor` (`PMATH_TO_INT`, reading the exact float's double directly) and the registration. The rows for `PMATH_CONVERT_TO_DOUBLE`, `cimpl_math_1`, `PMATH_FUNC1` and the `FUNC1` instances describe the code before D87.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header comments, includes, extern prototypes | 44 | glue | none |  |  | Old Psyco TODO list (lines 12-25) is stale |
| PMATH_CONVERT_TO_DOUBLE, PMATH_RELEASE_DOUBLE | 18 | translated | partial: Objects/floatobject.c:PyFloat_AsDouble (as used by Modules/mathmodule.c:math_1_to_whatever) | P01 | DRIFT: psyco_convert_to_double, not PyFloat_AsDouble: an int subclass's __float__ is ignored (PyLong_AsDouble instead of nb_float) | BUG [read]: math.sin(I(1)) for class I(int) with __float__ returning 2.0 gives sin(1.0) compiled, sin(2.0) in CPython. Other types go to the real function KNOWN_BUGS F99. |
| cimpl_math_1 | 32 | translated | Modules/mathmodule.c:math_1_to_whatever, is_error | P13 | EFFECT: CfPyErrCheck\|CfReturnTypeDouble (not CfPure: no compile-time fold) | Same tests as 3.7 [read]; the PyFPE macros are dropped |
| PMATH_FUNC1 | 15 | translated | Modules/mathmodule.c:FUNC1, math_1 | P10,P12,P18 | VIRTUAL: virtual float result; OTHER: fallback passes NULL as the module | A small hand-written meta-compiler |
| cimpl_floor_to_int, cimpl_ceil_to_int | 3 | translated | partial: Modules/mathmodule.c:math_1_to_int (floor, ceil) | P13 |  | Skips math_1_to_whatever's tests; PyLong_FromDouble raises the same errors for NaN and infinities [inferred] |
| PMATH_TO_INT | 22 | mixed | partial: Modules/mathmodule.c:math_ceil, math_floor | P01,P26,P12 | SHORTCUT: exact float only (P26), skipping _PyObject_LookupSpecial(__ceil__/__floor__) because 3.7's float has neither [read] | Core part: none beyond the whitelist. Other types: the real function |
| PMATH_FUNC1/PMATH_TO_INT instances (acos..tanh) | 16 | translated | Modules/mathmodule.c:FUNC1 table (acos, asin, atan, ceil, cos, cosh, exp, fabs, floor, sin, sinh, sqrt, tan, tanh) | P10 |  | can_overflow flags match 3.7 [read] |
| psyco_initmath | 32 | glue | none | P10 |  | All METH_O in 3.7; a mismatch is listed by ni._unregistered_metas() |
| #else psyco_initmath (no FP calls) | 6 | glue | none |  |  |  |

### `ni/Python/pbltinmodule.c`

Hand metas for 7 builtins and for `type_call`; 10 more are generated into `pbltinmodule_gen.h` (D86, `58e52de`). It imports `builtins`, not `__builtin__`, so the claim in PATTERNS P10 and rule 5 that it registers nothing on Py3 is out of date **[read]**. MAP calls it `translated`, but most of the code is core or policy: genexpr fusion, zero-argument `super()` on the compiled frame, the virtual `range`, and the static-type-only promotion used in `isinstance` and `type_call` **[read]**. The Py2 range helpers (`get_len_of_range`, `longobj_as_long`, `parse_range_args`, `cimpl_range`) are dead **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| includes, cimpl_* fallback pointers | 27 | glue | none |  |  | Includes `pbltinmodule_gen.h` (D86); the pointers of the generated builtins are the header's `cfunc_*` |
| genexpr_consume | 34 | core | none |  | RESTYPE: list/set/tuple/bool per consumer; EFFECT: CfCommonNewRefPyObject on PyList_New/PySet_New/PyList_AsTuple | Genexpr fusion (D46, psyfunc.c:psyco_call_genexpr) |
| pbuiltin_any | 11 | mixed | Python/bltinmodule.c:builtin_any | P10,P12 | OTHER: genexpr fused only while untraced (D46) | Core part: genexpr fusion; otherwise residual |
| pbuiltin_all | 10 | mixed | Python/bltinmodule.c:builtin_all | P10,P12 | OTHER: genexpr fused only while untraced (D46) | Core part: genexpr fusion; otherwise residual |
| statics type_call, object_class_descr, str___class__ | 4 | glue | none |  |  |  |
| pbuiltin_globals | 8 | mixed | Python/bltinmodule.c:builtin_globals_impl | P30,P28 |  | Core part: LOC_GLOBALS of the compiled frame instead of PyEval_GetGlobals (F47) |
| pbuiltin_min_max | 46 | mixed | partial: Python/bltinmodule.c:min_max (two or more positional, no keywords) | P22,P01,P05,P12 | PROMOTE: every argument's type before the first compare (restart rule, 0feefaf); SHORTCUT: compiled only for a known tuple of 2+; LIKELY: runtime_NON_NULL_f (keep current best) | Core part: genexpr fusion for one argument |
| pbuiltin_min, pbuiltin_max | 10 | glue | none |  |  | Wrappers (builtin_min, builtin_max) |
| isinstance_known | 32 | mixed | partial: Objects/abstract.c:PyObject_IsInstance, recursive_isinstance | P14 | SHORTCUT: compile-time answer for an exact-type cls and a static obj type only; OTHER: tuple depth > 8 gives up (CPython counts recursion); GUARD: heap types never folded (__bases__ can change) | False needs obj.__class__ to be object.__class__ with generic getattr. Tuple tested before PyType_CheckExact (disjoint, no difference) |
| isinstance_known_v | 28 | core | none | P22,P03 | PROMOTE: psyco_atcompiletime_static_type on the class (F25); VIRTUAL: walks a virtual tuple item by item |  |
| PsycoObject_IsInstance | 31 | mixed | Objects/abstract.c:PyObject_IsInstance | P03,P15,P12,P23 | PROMOTE: static type of obj only (F25); GUARD: Psyco_ConstantClassMutable for a constant (D62, F96); EFFECT: CfCommonIntBoolOrError on PyObject_IsInstance; SHORTCUT: zero_or_one | Was `pbuiltin_isinstance`'s body; the generated `builtin_isinstance_impl` calls it (D86). Returns 1/0 or the residual word |
| pbuiltin_sum | 76 | mixed | partial: Python/bltinmodule.c:builtin_sum_impl | P22,P01,P12 | PROMOTE: every item's type before the first add (restart rule, d487bbb); SHORTCUT: known-length tuple only; OTHER: a result of unknown type uses the residual PyNumber_Add (temporaries can't be promoted, F24) | Core part: genexpr fusion. CPython's int/float fast paths are not mirrored; results are the same [inferred] |
| float_whole_to_int | 20 | translated | partial: Objects/floatobject.c:float___trunc___impl, float___round___impl | P13,P15 |  | Helper composing the pfloatobject.c helpers |
| pbuiltin_round | 28 | mixed | partial: Python/bltinmodule.c:builtin_round_impl (with Objects/longobject.c:long_round, Objects/floatobject.c:float___round___impl) | P01,P26,P10 | SHORTCUT: exact int returns itself, exact float rounds inline (P26); skips _PyObject_LookupSpecial(__round__) | Otherwise PsycoCFunction_CallFast |
| cimpl_super_noargs | 39 | translated | partial: Objects/typeobject.c:super_init (no-argument branch) | P30,P24 |  | Same RuntimeError messages as 3.7 [read: names]; the frame values come as arguments |
| psuper_noargs | 22 | core | none | P30 |  | Maps co_cell2arg / __class__ cell to po->vlocals (D58) |
| ptype_call | 85 | mixed | partial: Objects/typeobject.c:type_call (with Objects/longobject.c:long_new_impl, Objects/floatobject.c:float_new_impl, Objects/abstract.c:PyNumber_Long, PyNumber_Float) | P03,P01,P05,P15,P18,P26,P12 | PROMOTE: static-type callee only (F25); SHORTCUT: type(x), int(x), float(x) for exact int/float (P26); LIKELY: super test on an unknown callee is _f; VIRTUAL: range(...) as a virtual range | Core part: zero-arg super, virtual range, genexpr fusion into list/tuple/set |
| psyco_bltinmodule_init | 27 | glue | none | P10 |  | Imports 'builtins', calls `gen_bltinmodule_init` (10 generated metas, D86) and registers globals, min, max, round, sum, any, all and type_call |

### `ni/Objects/pstringobject.c`

Py2 `str` only. The whole file is inside `#ifdef IS_PY3K #else`, and `compat2to3.h` always defines `IS_PY3K`. `setup.py` still compiles the file through its glob, but it compiles to nothing **[read]**. The `USE_CATSTR` parts are compiled out even on Py2 **[read]**. Counterparts are Py2 and were not checked against a Py2 tree (none is installed).

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header comment, includes, forward declarations | 26 | glue | none |  |  | Whole file inside #ifdef IS_PY3K #else; compat2to3.h always defines IS_PY3K [read] |
| psyco_computed_char, cimpl_character, compute_char, direct_compute_char | 36 | core | none | P18 | EFFECT: CfPure on cimpl_character | Py2 only |
| PsycoCharacter_NEW, PsycoCharacter_New, first_character | 28 | core | none | P18 | VIRTUAL: one-char string | Py2 only |
| PsycoCharacter_Ord | 34 | mixed | partial: (Py2) Python/bltinmodule.c:builtin_ord [inferred] | P01,P05,P15 |  | Py2 only; core part: virtual char |
| psyco_computed_strslice, compute_strslice, direct_compute_strslice, PsycoStrSlice_NEW | 76 | core | none | P18 | VIRTUAL: string slice | Py2 only |
| USE_CATSTR block (catstr virtual strings) | 217 | core | none | P18 | VIRTUAL: catstr | Py2 only, and USE_CATSTR is 0: compiled out even on Py2 |
| pstring_item | 41 | mixed | (Py2) Objects/stringobject.c:string_item [inferred] | P07,P05,P06,P24,P21,P15 | SHORTCUT: a one-char exact str returns itself (P21); VIRTUAL: result is a virtual char | Py2 only |
| pstring_slice | 102 | mixed | (Py2) Objects/stringobject.c:string_slice [inferred] | P07,P05,P20,P21,P18 | OTHER: CHEAT_MAXINT clamping; VIRTUAL: string slice | Py2 only |
| pstring_concat (USE_CATSTR) | 112 | core | (Py2) Objects/stringobject.c:string_concat [inferred] | P18,P20 |  | Compiled out (USE_CATSTR 0) |
| psyco_richmemcmp | 145 | mixed | partial: (Py2) Objects/stringobject.c:string_richcompare (memcmp part) [inferred] | P05,P12,P15 | SHORTCUT: known lengths (0, different sizes for == and !=) decided at compile time | Py2 only |
| pstring_richcompare | 44 | mixed | (Py2) Objects/stringobject.c:string_richcompare [inferred] | P01,P21,P25 | SHORTCUT: vinfo_known_equal identity (P21) | Py2 only |
| pstring_mod | 11 | translated | (Py2) Objects/stringobject.c:string_mod [inferred] | P12,P25 |  | Py2 only; residual PyString_Format |
| USE_BUFSTR block: PsycoBufStr_NEW, PsycoBufStr_Type, cimpl_cb_new/grow/normalize, compute_bufstr, direct_compute_bufstr | 198 | core | none | P18,P20 | VIRTUAL: over-allocated buffer string (BUFSTR_OVERALLOCATION_MASK) | Py2 only |
| pstring_concat (USE_BUFSTR) | 89 | mixed | (Py2) Objects/stringobject.c:string_concat [inferred] | P01,P14,P20,P21,P12 | SHORTCUT: constant fold, '' identity, in-place buffer growth | Py2 only |
| pstr_memory_source | 29 | core | none |  |  | Py2 only |
| psy_stringobject_init, #endif | 45 | glue | none |  |  | Never called on Py3 |

### `ni/Objects/pstringobject.h`

The Py2 `str` virtual-string layouts. The body is empty on Py3, but `pobject.c`, `pstructmember.c`, `plistobject.h` and `pbltinmodule.c` still include it **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header, include guard, #ifdef IS_PY3K #else | 12 | glue | none |  |  | Empty on Py3 |
| includes, USE_CATSTR/USE_BUFSTR, STR_sval*, virtual string layouts | 36 | core | (Py2) Include/stringobject.h:PyStringObject [inferred] | P15,P18 |  | Py2 only; layout would fit PyBytesObject |
| PsycoString_Check, PsycoUnicode_Check, PsycoString_AS_STRING, PsycoString_GET_SIZE | 18 | translated | (Py2) Include/stringobject.h:PyString_Check, PyString_AS_STRING, PyString_GET_SIZE [inferred] | P15 |  | Py2 only |
| declarations, #endif | 7 | glue | none |  |  |  |

## Containers, functions, methods, descriptors

### `ni/Objects/plistobject.h`

Field descriptors for `PyListObject` and the virtual-list API; `VLIST_LENGTH_MAX` (3) is the virtual-list size policy **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard and includes | 12 | glue | none |  |  |  |
| LIST_ob_item, LIST_TOTAL, VLIST_ITEMS, LIST_itemsarray | 6 | core | none | P15 | OTHER: ob_item declared FMUT after VAR_size (judgement: size mutable) | layout of Include/listobject.h PyListObject; VLIST_ITEMS puts virtual items after the real fields |
| VLIST_LENGTH_MAX | 5 | core | none | P18 | VIRTUAL: lists longer than 3 are never virtual (code-size heuristic) |  |
| declarations | 21 | glue | none |  |  |  |

### `ni/Objects/plistobject.c`

Virtual short lists (core) plus metas for `list_item`, `list_ass_item`, `list_concat`, `list_subscript` and `list_ass_subscript`, each with a virtual fast path; the step-1 slice helpers are near-verbatim CPython **[read]**. Surprising: `tp_iter` is redirected to the generic seq-iterator, and integer subscripts go through `pabstract.c`'s generic path, not `list_subscript`'s own code **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes, psyco_computed_vlist | 24 | glue | none |  |  |  |
| vlist_length | 11 | core | none | P18 |  | length from the vinfo array size |
| compute_vlist | 45 | core | none | P18, P12, P15, P16, P17 | EFFECT: PyList_New CfCommonNewRefPyObject | residual PyList_New and FPYREF item stores, then forgets VAR_size |
| list_maybe_compute | 15 | core | none | P18 | VIRTUAL: forces a list longer than VLIST_LENGTH_MAX |  |
| PsycoList_NEW | 13 | core | none | P18 | VIRTUAL: length a fixed constant (SkFlagFixed) |  |
| PsycoList_New | 15 | core | none | P18 |  | BUILD_LIST (pycompiler.c) and psyco_plist_concat |
| PsycoList_SingletonNew | 9 | core | none | P18 |  | only caller is pstringobject.c, compiled out on Py3: dead |
| PsycoList_Load | 11 | core | none | P22 |  | virtual lists only |
| plist_item | 48 | mixed | Objects/listobject.c:list_item | P15, P07, P05, P06, P24, P19, P17 | VIRTUAL: virtual list with a constant index reads the vinfo array (P19); REF: need_reference on the item taken from the virtual array; LIKELY: out of range unlikely (_f) | core = virtual fast path. Also called by piter_iternext for list sequences |
| plist_ass_item | 58 | mixed | Objects/listobject.c:list_ass_item | P15, P07, P05, P06, P24, P19, P17, P12 | VIRTUAL: store into a virtual list at a constant index unless recursive (P19); EFFECT: deletion is residual sq_ass_item CfCommonIntZeroOk | core = virtual fast path; old item decref'd after the store, as Py_SETREF |
| psyco_plist_concat | 48 | mixed | Objects/listobject.c:list_concat | P01, P22, P12, P08 | PROMOTE: b's type promoted where list_concat only tests PyList_Check; VIRTUAL: two virtual lists give a virtual list; RESTYPE: list for residual list+list | core = virtual concat; non-list b's TypeError comes from the residual call |
| PsycoList_Append | 10 | translated | Objects/listobject.c:PyList_Append | P12 | EFFECT: CfCommonIntZeroOk | residual only; used by LIST_APPEND paths in pycompiler.c |
| cimpl_list_slice | 9 | translated | partial: Objects/listobject.c:list_subscript | P18a |  | step-1 slice case after PySlice_Unpack; PyList_GetSlice stands for list_slice |
| cimpl_list_ass_slice | 9 | translated | partial: Objects/listobject.c:list_ass_subscript | P18a |  | step-1 case; PyList_SetSlice is list_ass_slice behind a PyList_Check |
| plist_subscript | 53 | mixed | Objects/listobject.c:list_subscript | P01, P29, P18a, P12, P08 | GUARD: mp_subscript, sq_item, sq_length of o's type through Psyco_TypeSlot (F56); a subclass with its own slot gets residual list_subscript (F41); PROMOTE: o's type; RESTYPE: list for the slice helper | core = virtual-slice unpack and the slot check (list_subscript is also list.__getitem__, METH_COEXIST). Int keys go to pabstract.c:psyco_generic_subscript, not list_subscript's own index code |
| plist_ass_subscript | 30 | mixed | Objects/listobject.c:list_ass_subscript | P01, P18a, P12 | PROMOTE: key's type; EFFECT: helper CfCommonIntZeroOk | no slot guards (no __setitem__ METH_COEXIST on list, so a subclass slot never lands here [inferred]); non-slice keys go to psyco_generic_ass_subscript |
| psy_listobject_init | 31 | glue | none | P11 | OTHER: list tp_iter redirected to PsycoSeqIter_New (P11), so iter(list) is 'iterator' not 'list_iterator'; VIRTUAL: INIT_SVIRTUAL_NOCALL (forced at calls), weight NW_VLISTS | sq_length is the generated `list_length` (`plistobject_gen.h`, D84); the 'Python >= 2.3' tests are dead |

### `ni/Objects/ptupleobject.h`

Tuple item descriptor and vinfo-array access macros **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard and includes | 12 | glue | none |  |  |  |
| TUPLE_ob_item, iTUPLE_OB_ITEM | – | generated | none | P15 |  | **Generated** into `virtual_fields_gen.h` by `gen_virtual.py` from `policy_table.TYPES` and the D76 layout (D80) |
| PsycoTuple_GET_ITEM, PsycoTuple_SET_ITEM | 11 | core | none | P22 |  | vinfo-array access, no checks |
| declarations | 16 | glue | none |  |  |  |

### `ni/Objects/ptupleobject.c`

Mostly core: virtual tuples, constant folding of all-constant tuples, `PsycoTuple_Load`. `ptuple_item` is a straight `tupleitem` translation; tuple `mp_subscript` is the generic meta, not `tuplesubscript` **[read]**. Since D78 `tuplecontains` (`sq_contains`) is generated into `ptupleobject_gen.h`, which this file includes; `psy_tupleobject_init` calls its `gen_tupleobject_init()` **[run]**. Since D84 `tuplerichcompare` (`tp_richcompare`, a field of the type) is generated there too; Ni called the slot before **[run]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, include, psyco_computed_tuple | 18 | glue | none |  |  |  |
| compute_tuple, direct_compute_tuple, PsycoTuple_New | – | generated | Objects/tupleobject.c:PyTuple_New | P18, P14, P12 | SHORTCUT: all-constant items built at compile time (rule allocator_pure); EFFECT: PyTuple_New from effects.py (CfNoPython, CfNoBlock) | **Generated** into `ptupleobject_virtual_gen.h` by `gen_virtual.py` (D80); the hand versions (97 lines) are gone |
| CT_TUPLE_LOAD_SIZE_LIMIT, PsycoTuple_Load | 42 | core | none | P22 | VIRTUAL: constant tuples over 15 items are not loaded | -1 for a non-tuple constant (F45) |
| PsycoTuple_Concat | – | removed | Objects/tupleobject.c:tupleconcat | P02, P22, P21, P12 | | **Removed** (D80): `tupleconcat` and `tuplerepeat` are generated into `ptupleobject_gen.h` by the constructor rule; see METACOMPILER "Virtual objects" for the differences |
| ptuple_item | 26 | translated | Objects/tupleobject.c:tupleitem | P15, P07, P05, P24 | LIKELY: out of range unlikely (_f) | no assert_nonneg after the test, unlike plist_item |
| psy_tupleobject_init | 20 | glue | none |  | OTHER: tuple mp_subscript is the generic pabstract.c:psyco_generic_subscript, not a tuplesubscript translation; VIRTUAL: INIT_SVIRTUAL (stays virtual across calls), weights NW_TUPLES_NORMAL, NW_TUPLES_FUNCALL | sq_length is psyco_generic_immut_ob_size |

### `ni/Objects/piterobject.h`

A hand copy of CPython's private `seqiterobject` (with `long` for `Py_ssize_t`) and its descriptors **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard and includes | 12 | glue | none |  |  |  |
| seqiterobject | 7 | translated | Objects/iterobject.c:seqiterobject (struct) |  | DRIFT: it_index declared long, CPython Py_ssize_t (same size on x86-64) | private struct copied by hand |
| SEQITER_* descriptors | 8 | core | none | P15 | OTHER: it_index FMUT; it_seq declared immutable (cached, checked against memory for run-time iterators, F48) |  |
| psyco_computed_seqiter, PsycoSeqIter_NEW declarations | 7 | glue | none |  |  |  |
| PsycoSeqIter_New | 5 | core | none | P18 |  | the meta registered on list_iter (P11) |
| header end | 2 | glue | none |  |  |  |

### `ni/Objects/piterobject.c`

`piter_iternext` translates `iter_iternext` around a virtual iterator whose `it_seq` is cached and, for run-time iterators, checked against memory (F48) **[read]**. It lacks 3.7's `PY_SSIZE_T_MAX` overflow check **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 15 | glue | none |  |  |  |
| PsycoSeqIter_NEW | 19 | core | none | P18 | VIRTUAL: it_index made run-time at once (no specialisation on the loop counter) |  |
| piter_getiter | 6 | translated | Objects/object.c:PyObject_SelfIter |  |  | same C pointer as prangeiter_getiter (every iterator's tp_iter); the later registration wins, both identical |
| cimpl_seqiter_drop_seq | 6 | translated | partial: Objects/iterobject.c:iter_iternext |  |  | the 'it_seq = NULL; Py_DECREF(seq)' step |
| cimpl_seqiter_exhausted | 13 | core | none |  |  | materialises an exhausted virtual iterator |
| piter_stop | 6 | core | none | P24 |  | StopIteration at the meta level |
| piter_ends | 13 | translated | partial: Objects/iterobject.c:iter_iternext | P05 | LIKELY: IndexError expected (_t), StopIteration not (_f) | the PyErr_ExceptionMatches pair |
| piter_iternext | 109 | mixed | Objects/iterobject.c:iter_iternext | P15, P05, P01, P06, P12, P16, P24 | GUARD: a run-time iterator's cached it_seq compared with memory, mismatch goes to residual iter_iternext (F48); SHORTCUT: list sequences use plist_item (what listiter_next does) instead of PySequence_GetItem; PROMOTE: sequence type; DRIFT: no it_index == PY_SSIZE_T_MAX OverflowError; EFFECT: CfCommonIterNext, CfNoReturnValue | core = virtual-iterator state and it_seq caching. PyType_IsSubtype on a heap list subclass, not Psyco_TypeIsSubtype (a list subclass can't lose list by __bases__ [inferred]) |
| compute_seqiter, psyco_computed_seqiter | 54 | core | none | P18, P12, P16 | EFFECT: PySeqIter_New CfCommonNewRefPyObject |  |
| psy_iterobject_init | 11 | glue | none |  | VIRTUAL: INIT_SVIRTUAL_NOCALL, weight 1 |  |

### `ni/Objects/prangeobject.h`

Declaration only.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard, PsycoRange_New declaration | 13 | glue | none |  |  |  |

### `ni/Objects/prangeobject.c`

Virtual ranges and range iterators (core) with near-verbatim helpers for `get_len_of_range`, `fast_range_iter` and `rangeiter_next` **[read]**. The layout check in init fails silently **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 18 | glue | none |  |  |  |
| rangeobject, rangeiterobject | 11 | translated | Objects/rangeobject.c:rangeobject, rangeiterobject (structs) |  |  | private structs copied; init checks tp_basicsize |
| RANGE_*, iVRANGE_*, RANGEITER_* descriptors | 20 | core | none | P15 | VIRTUAL: a virtual range keeps start/stop/step as words past the real fields; OTHER: rangeiter start/step/len immutable, index FMUT |  |
| statics (source_virtual_t, range_iter) | 4 | glue | none |  |  |  |
| cimpl_range_len | 10 | translated | Objects/rangeobject.c:get_len_of_range | P13 | EFFECT: called CfPure\|CfReturnTypeLong (folds for constants) | same arithmetic |
| cimpl_range_new | 5 | core | none |  |  | PyObject_CallFunction(range, 'lll') |
| cimpl_rangeiter_new | 13 | translated | partial: Objects/rangeobject.c:fast_range_iter |  |  | index and len given; no LONG_MAX check (one-digit bounds) |
| compute_range | 13 | core | none | P18, P12 |  |  |
| compute_rangeiter | 13 | core | none | P18, P12, P16 |  |  |
| PsycoRange_New | 52 | core | none | P01, P05, P18 | SHORTCUT: only 1-3 exact ints of one digit (PsycoLong_MediumValue) and a non-zero step, else the real call; PROMOTE: argument types; LIKELY: step non-zero (_t) | replaces range_new for the handled case; called from pbltinmodule.c:ptype_call |
| prange_iter | 28 | mixed | Objects/rangeobject.c:range_iter | P18, P13, P12 | VIRTUAL: virtual range iterator, index run-time at once; EFFECT: real range is residual range_iter CfCommonNewRefPyObject | core = virtual iterator; fields as fast_range_iter (len by cimpl_range_len) |
| prangeiter_next | 44 | translated | Objects/rangeobject.c:rangeiter_next | P15, P05, P24 | VIRTUAL: result a virtual int (PsycoLong_FROM_WORD); LIKELY: index < len (_t); OTHER: sets StopIteration, not a bare NULL (meta convention) | unsigned wrap kept by word arithmetic |
| prangeiter_getiter | 5 | translated | Objects/object.c:PyObject_SelfIter |  |  | duplicate of piter_getiter on the same C pointer |
| psy_rangeobject_init | 15 | glue | none |  | OTHER: layout check fails silently (no metas registered, unlike punicodeobject.c); VIRTUAL: both INIT_SVIRTUAL_NOCALL (identity, C17), weights 4 and 1 |  |

### `ni/Objects/pmethodobject.h`

`PyCFunctionObject` descriptors and the inline virtual builtin-method builder **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard and includes | 12 | glue | none |  |  |  |
| CFUNC_* descriptors | 6 | core | none | P15 |  | m_ml, m_self; m_module not modelled |
| declarations, call-site counters, psyco_computed_cfunction | 25 | glue | none |  |  |  |
| PsycoCFunction_New | 14 | core | none | P18 |  | virtual builtin method (pdescrobject.c:pmethod_get) |
| header end | 2 | glue | none |  |  |  |

### `ni/Objects/pmethodobject.c`

`PsycoCFunction_Call` keeps `_PyMethodDef_RawFastCallDict`'s flag switch, but which calls go direct, the fixed-arity FASTCALL helpers and the inline `_Py_CheckFunctionResult` are Ni's own **[read]**. `PyCFunction_NewEx` is called `CfPure` when the object escapes **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 15 | glue | none |  |  |  |
| compute_cfunction | 30 | core | none | P18, P12 | EFFECT: PyCFunction_NewEx called CfPure (an allocator: constant m_ml and m_self would build one shared object at compile time [inferred]); DRIFT: m_module NULL, lost | the '#ifdef PyCFunction_New' branch is the live one on 3.7 |
| direct_compute_cfunction | 15 | core | none | P18 |  |  |
| psyco_computed_cfunction, section comment | 5 | glue | none |  |  |  |
| cf_tstate_slot_t, CF_TSTATE_* descriptors, CF_UNCHECKED | 18 | core | none | P15 |  | thread-state read for check_result; ni_tstate_current_address in Python/pyruntime.c |
| cimpl_null_result | 4 | translated | partial: Objects/call.c:_Py_CheckFunctionResult |  |  | the SystemError for NULL without an exception |
| cimpl_result_with_error | 6 | translated | partial: Objects/call.c:_Py_CheckFunctionResult |  |  | the SystemError for a result with an exception; None stands for the released result |
| check_result | 38 | mixed | Objects/call.c:_Py_CheckFunctionResult | P05, P15, P12 | SHORTCUT: PyErr_Occurred inline as a load of tstate->curexc_type; LIKELY: non-NULL result (_t), no exception (_f); EFFECT: SystemError paths CfNoReturnValue\|CfPyErrAlways | core = inline thread-state read (C25) |
| call_fast forward declaration | 3 | glue | none |  |  |  |
| CALL_METH | 9 | core | none | P09, P12 | EFFECT: direct ml_meth call CF_UNCHECKED, then check_result | meta if registered, else checked residual call |
| PsycoCFunction_Call | 63 | mixed | Objects/call.c:PyCFunction_Call, _PyMethodDef_RawFastCallDict | P03, P09, P22, P12 | MEGA: forgotten or megamorphic callable goes to residual PyCFunction_Call, m_ml not promoted (D43); PROMOTE: m_ml (psyco_atcompiletime); DRIFT: direct calls skip Py_EnterRecursiveCall (C25); METH_O/NOARGS of unknown or wrong arity and kwargs not known NULL go residual | core = meta lookup and the residual choice; the flag switch and its mask are _PyMethodDef_RawFastCallDict's |
| psyco_stat_cfunc_fast, psyco_stat_cfunc_proxy | 3 | core | none |  | OTHER: ni._stats() counters |  |
| cimpl_call_fast | 7 | translated | partial: Objects/call.c:_PyMethodDef_RawFastCallDict |  |  | METH_FASTCALL case with the tuple's items |
| cimpl_call_fast_kw | 6 | translated | partial: Objects/call.c:_PyMethodDef_RawFastCallDict |  |  | METH_FASTCALL\|METH_KEYWORDS case, kwnames NULL |
| FAST_MAX, FAST_FN cimpl_fastN/cimpl_fastkwN, cimpl_fast table | 28 | core | none |  | SHORTCUT: arguments passed one by one to fixed-arity helpers, no tuple (F13) |  |
| call_fast | 28 | core | none | P22, P12 | EFFECT: CfNoBlock when psyco_cannot_block(meth) (D53); CF_UNCHECKED when vfunc given; SHORTCUT: up to FAST_MAX (5) arguments without a tuple | counts psyco_stat_cfunc_fast |
| PsycoCFunction_CallFast | 6 | core | none |  |  | METH_FASTCALL meta fallback (D36); result not checked |
| psy_methodobject_init | 6 | glue | none |  | VIRTUAL: INIT_SVIRTUAL, weights 1, 1 |  |

### `ni/Objects/pfuncobject.h`

`PyFunctionObject` descriptors and the pending-genexpr layout **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard and includes | 10 | glue | none |  |  |  |
| FUNC_* descriptors and indices | 23 | core | none | P15 |  | PyFunctionObject fields; none FMUT or FPYREF (compute_function no longer stores through them) |
| declarations | 13 | glue | none |  |  |  |
| iGENEXPR_FUNC, iGENEXPR_ARGS, PsycoGenexpr_* declarations | 9 | core | none | P18 |  | layout of a pending generator expression |
| header end | 2 | glue | none |  |  |  |

### `ni/Objects/pfuncobject.c`

Almost all core: virtual functions, pending genexprs, the call policy (promotion, megamorphic cut-off, closures) and the `func_code`/`func_defaults` guards. Only `pfunc_descr_get` is a translation **[read]**. The file comment's three suspected bugs are already fixed **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes, psyco_computed_function | 28 | glue | none |  |  | header's 'suspected bugs' are stale: SET_FIELD uses && and CPython's setters (F7), keyword-only goes to tp_call, closures of virtual functions are read in psyfunc.c |
| cimpl_func_set | 10 | core | none |  |  | dispatches to PyFunction_Set*; ceval's MAKE_FUNCTION stores the fields directly |
| compute_function | 52 | core | none | P18, P12 | EFFECT: setters CfNoReturnValue\|CfPyErrIfNonNull |  |
| PsycoFunction_NewWithQualName | 26 | core | none | P18 |  | virtual PyFunction_NewWithQualName; MAKE_FUNCTION (pycompiler.c) fills the optional fields |
| psyco_pending_genexpr, compute_genexpr | 16 | core | none | P18, P12 | VIRTUAL: a called genexpr stays pending until fused or escaping | residual tp_call makes the generator |
| PsycoGenexpr_New | 13 | core | none | P18 |  |  |
| PsycoGenexpr_Check | 6 | core | none |  |  |  |
| compiled_defaults | 30 | core | none | P29 | GUARD: defaults first seen on f, held for good (F52) |  |
| compiled_code | 21 | core | none | P29 | GUARD: code first seen on f, held for good (F57) |  |
| field_changed | 20 | core | none | P29, P05 | GUARD: emits 'field != expected'; LIKELY: unchanged (_f) |  |
| FUNC_code_now, FUNC_defaults_now | 3 | core | none | P15 |  | FMUT views for the guards |
| pfunction_simple_call | 70 | core | none | P29, P22 | GUARD: func_code vs compiled_code; func_defaults vs compiled_defaults unless every positional argument is given; OTHER: is_inlining forced when !allow_inline; DRIFT: keyword-only parameters take residual tp_call | replaces funcobject.c:function_call -> call.c:_PyFunction_FastCallDict; binding is psyfunc.c:psyco_call_pyfunc |
| pfunction_call | 77 | core | none | P04, P03 | MEGA: forgotten callable residual (D43), psyco_pyobj_atcompiletime_mega on the code and on the function; PROMOTE: a run-time closure promotes only its code (psyco_call_closure); DRIFT: kw not known NULL, or keyword-only parameters, take residual tp_call | call-path policy; GENERATION step 7 keeps it core |
| pfunc_descr_get | 11 | translated | Objects/funcobject.c:func_descr_get | P18 | VIRTUAL: bound method virtual (PsycoMethod_New); DRIFT: obj None/NULL case not handled | generic_getattr passes only instances, so unreachable in practice [inferred] |
| psy_funcobject_init | 13 | glue | none |  | VIRTUAL: function and pending genexpr INIT_SVIRTUAL_NOCALL, weight 1 |  |

### `ni/Objects/pclassobject.h`

`PyMethodObject` descriptors **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard and includes | 10 | glue | none |  |  |  |
| METHOD_im_func, METHOD_im_self, indices | 7 | core | none | P15 |  | PyMethodObject fields (Py2 'im_' names) |
| declarations | 15 | glue | none |  |  |  |

### `ni/Objects/pclassobject.c`

Virtual bound methods and a `method_call` meta that prepends self in a virtual tuple **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 8 | glue | none |  |  |  |
| PsycoMethod_New, compute_method, direct_compute_method | – | generated | Objects/classobject.c:PyMethod_New | P18, P12 | EFFECT: PyMethod_New from effects.py (CfNoPython, CfNoBlock; was CfPure); SHORTCUT: constant func and self built at compile time | **Generated** into `pclassobject_virtual_gen.h` (D80); `PsycoMethod_New` now takes func as a vinfo (`pfunc_descr_get` makes the constant) |
| psyco_computed_method, section comment | 5 | glue | none |  |  |  |
| pinstancemethod_call | 54 | mixed | Objects/classobject.c:method_call | P15, P05, P22, P18, P12 | LIKELY: im_self non-NULL (_t); VIRTUAL: self prepended in a virtual tuple (_PyObject_Call_Prepend uses a stack array); unknown arity goes residual; DRIFT: NULL im_self branch is a Py2 unbound-method leftover (residual raises BadInternalCall as CPython) | core = virtual-tuple prepend; then PsycoObject_Call re-dispatches |
| psy_classobject_init | 7 | glue | none |  | VIRTUAL: INIT_SVIRTUAL, weights 0, 0 |  |

### `ni/Objects/pdescrobject.c`

`member_get` and `method_get` without `descr_check`; `pobject.c:generic_getattr` makes that check before using them **[read]**. The file comment says they are unreachable, but `generic_getattr` dispatches to them **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 14 | glue | none |  |  | comment is stale: pobject.c:generic_getattr dispatches tp_descr_get through Psyco_META3 'lvl' |
| pmember_get | 12 | translated | partial: Objects/descrobject.c:member_get | P09 | DRIFT: no descr_check (obj NULL, wrong type); pobject.c:generic_getattr uses it only for a subtype of d_type, so the check lives in core | meta signature 'lvl' |
| pmethod_get | 8 | translated | partial: Objects/descrobject.c:method_get | P09, P18 | VIRTUAL: virtual builtin method (PsycoCFunction_New); DRIFT: no descr_check, as pmember_get |  |
| psy_descrobject_init | 32 | glue | none |  | OTHER: finds PyMemberDescr_Type and PyMethodDescr_Type by making dummy descriptors, though 3.7 exports both (Include/descrobject.h) |  |

### `ni/Objects/pdictobject.h`

Declarations only.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard, includes, declarations, PsycoDict_NewPresized macro | 26 | glue | none |  |  | guard's end comment says _PSY_LISTOBJECT_H |

### `ni/Objects/pdictobject.c`

Residual-call wrappers with a known result type, plus `gen_dictobject_init` registering the generated `dict_length`, a field read of `ma_used` (read as `int`, M11) (D84) **[read]**. `PsycoDict_Copy` and `PsycoDict_SetItem` have no callers **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| include | 3 | glue | none |  |  |  |
| DICT_ma_used, iDICT_MA_USED | 4 | core | none | P15 | DRIFT: ma_used read as int, CPython Py_ssize_t (low 32 bits) |  |
| PsycoDict_New | 14 | translated | Objects/dictobject.c:PyDict_New | P12, P08 | RESTYPE: dict | no virtual dicts |
| PsycoDict_NewPresized | 19 | translated | Objects/dictobject.c:_PyDict_NewPresized | P12, P08 | SHORTCUT: minused <= 5 calls PyDict_New (3.7's _PyDict_NewPresized has no such branch; equivalent dict); RESTYPE: dict | BUILD_MAP, BUILD_CONST_KEY_MAP, keyword dicts |
| PsycoDict_Copy | 14 | translated | Objects/dictobject.c:PyDict_Copy | P12, P08 | RESTYPE: dict | no callers in the repo |
| PsycoDict_SetItem | 9 | translated | Objects/dictobject.c:PyDict_SetItem | P12 | EFFECT: CfCommonIntZeroOk | no callers in the repo |
| PsycoDict_SetItemC | 9 | translated | Objects/dictobject.c:PyDict_SetItem | P12 | EFFECT: CfCommonIntZeroOk | key a raw constant ('l') |
| psyco_dict_length | – | removed | Objects/dictobject.c:dict_length | P15 | DRIFT: 32-bit read of ma_used (M11, unchanged: the descriptor is the same) | **Removed** (D84): `dict_length` is generated into `pdictobject_gen.h` through `TYPES`' descriptor `DICT_ma_used`. |
| psy_dictobject_init | 6 | glue | none |  |  |  |

### `ni/Objects/pbytesobject.c`

`bytes_subscript`: an index is read inline as a byte and gives a virtual int; a virtual slice goes to a step-1 helper that folds for a constant exact bytes **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| includes | 5 | glue | none |  |  |  |
| BYTES_sval | 2 | core | none | P15 |  | ob_sval as an unsigned byte array |
| bytes_subscript (static) | 2 | glue | none |  |  |  |
| cimpl_bytes_slice | 16 | translated | partial: Objects/bytesobject.c:bytes_subscript | P18a |  | step-1 slice case after PySlice_Unpack, identities as CPython |
| pbytes_subscript | 64 | mixed | Objects/bytesobject.c:bytes_subscript | P01, P18a, P13, P14, P05, P06, P07, P15, P24, P12, P08 | PROMOTE: key type; SHORTCUT: slice of a constant exact bytes folds (CfPure, D56); RESTYPE: bytes for a run-time slice; VIRTUAL: the byte as a virtual int (PsycoLong_FROM_LONG); LIKELY: negative and out-of-range unlikely (_f) | core = virtual-slice unpack. Only int subtypes inline; other __index__ keys residual |
| psy_bytesobject_init | 7 | glue | none |  |  | sq_length is the generated `bytes_length` (`pbytesobject_gen.h`, D84) |

### `ni/Objects/punicodeobject.c`

The PEP 393 str metas: compare, length, index, slice and `in`. They read the layout inline only for compact ASCII or constants, through a one-byte read of `state` **[read]**. The CPython counterpart for compare is `PyUnicode_RichCompare`, not `unicode_richcompare` as MAP says **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 17 | glue | none |  |  |  |
| UNICODE_length, UNICODE_hash, UNICODE_state, STATE_* | 10 | core | none | P15 | OTHER: state read as its low byte (bit layout of PyASCIIObject.state assumed) |  |
| statics (latin1[], slot pointers) | 5 | glue | none |  |  |  |
| both_str | 10 | translated | partial: Objects/unicodeobject.c:PyUnicode_RichCompare |  | PROMOTE: uses known types only, no promotion | the PyUnicode_Check pair |
| cimpl_unicode_eq | 4 | translated | partial: Objects/unicodeobject.c:PyUnicode_RichCompare |  | DRIFT: no PyUnicode_READY before _PyUnicode_EQ (a not-ready legacy str would compare wrongly [inferred]) | identity then unicode_compare_eq |
| punicode_richcompare | 37 | mixed | Objects/unicodeobject.c:PyUnicode_RichCompare | P25, P14, P13, P23, P12 | SHORTCUT: two constants compare at compile time (P14); EFFECT: cimpl_unicode_eq CfPure\|CfReturnTypeInt; VIRTUAL: ==/!= result a virtual bool (PsycoBool_FromCondition) | core = helper split and virtual bool. MAP says unicode_richcompare: no such function in 3.7, tp_richcompare is PyUnicode_RichCompare |
| state_at_least | 8 | core | none | P15, P07 |  | state byte compared unsigned |
| punicode_length | 16 | mixed | Objects/unicodeobject.c:unicode_length | P15, P05, P12 | LIKELY: ready (_t); EFFECT: CfCommonPySSizeTResult | core = PyUnicode_READY as an inline state-byte test |
| cimpl_unicode_item | 13 | translated | partial: Objects/unicodeobject.c:unicode_subscript, unicode_getitem |  |  | integer case, any str |
| punicode_item | 69 | mixed | Objects/unicodeobject.c:unicode_getitem | P15, P05, P06, P07, P24, P12, P08 | SHORTCUT: inline only for compact ASCII at run time or a compact constant, else the helper; latin1[] table for 1-byte results; LIKELY: compact ASCII (_t), negative and out of range (_f); EFFECT: PyUnicode_FromOrdinal CfPure (a constant non-Latin-1 char folds to one shared str [inferred]); RESTYPE: str | core = inline layout reads and the latin1[] table |
| cimpl_unicode_slice | 14 | translated | partial: Objects/unicodeobject.c:unicode_subscript | P18a |  | step-1 slice case; PyUnicode_New(0,0) is the shared empty str |
| punicode_subscript | 36 | mixed | Objects/unicodeobject.c:unicode_subscript | P01, P18a, P13, P14, P12, P08 | PROMOTE: key type; SHORTCUT: slice of a constant exact str folds (CfPure, D56); RESTYPE: str for a run-time slice | core = virtual-slice unpack. Only int subtypes inline; other __index__ keys residual |
| punicode_contains | 13 | translated | Objects/unicodeobject.c:PyUnicode_Contains | P14, P12 | SHORTCUT: two constants fold, an error is left to run time; EFFECT: CfCommonIntBoolOrError |  |
| psy_unicodeobject_init | 21 | glue | none |  | OTHER: checks the compact-ASCII layout and fails loudly; fills latin1[] from CPython's cache |  |

### `ni/Objects/pstructmember.h`

Declaration only.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard, includes, PsycoMember_GetOne declaration | 16 | glue | none |  |  |  |

### `ni/Objects/pstructmember.c`

One function: `PyMember_GetOne`, one field descriptor per `T_*` type. Several `T_*` types go to the residual call **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 13 | glue | none |  |  |  |
| PsycoMember_GetOne | 144 | translated | Python/structmember.c:PyMember_GetOne | P15, P05, P12, P08, P17, P24 | REF: need_reference on borrowed T_OBJECT/T_OBJECT_EX loads; LIKELY: non-NULL pointer (_t); RESTYPE: str for T_STRING, T_STRING_INPLACE; OTHER: every member read as FMUT; DRIFT: READ_RESTRICTED goes residual (3.7 ignores the flag; same result); T_BOOL, T_CHAR, T_LONGLONG, T_ULONGLONG, T_PYSSIZET, T_NONE, T_ULONG residual | the T_ULONG test in the integer tail is dead (T_ULONG goes to fallback) |

### `ni/Objects/pboolobject.h`

Virtual bools as virtual ints of type bool (P23) **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard, includes, psyco_computed_bool | 12 | glue | none |  |  |  |
| PsycoBool_FROM_WORD | 10 | core | none | P18, P23 |  | virtual int of type bool; computed by plongobject.c (PyBool_FromLong) |
| PsycoBool_FromCondition | 6 | core | none | P23 |  | condition code to virtual bool |
| header end | 1 | glue | none |  |  |  |

### `ni/Objects/psliceobject.h`

Declarations only. No MAP row; the layer would be `glue`.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header guard, includes, declarations | 18 | glue | none |  |  |  |

### `ni/Objects/psliceobject.c`

Virtual slices (D56): core construction and forcing, with `PySlice_Unpack` and `_PyEval_SliceIndex` translated for step None and None/int bounds only **[read]**. It has no MAP row; its layer would be `core + translated`.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 18 | glue | none |  |  |  |
| SLICE_* descriptors | – | generated | none | P15 |  | **Generated** into `virtual_fields_gen.h` (D80) |
| psyco_computed_slice | 2 | glue | none |  |  |  |
| compute_slice, direct_compute_slice, PsycoSlice_New | – | generated | Objects/sliceobject.c:PySlice_New | P18, P12 | VIRTUAL: BUILD_SLICE always virtual (D56); SHORTCUT: constant bounds built at compile time (new); forced: fields dropped (`TYPES` `forced: drop_fields`) | **Generated** into `psliceobject_virtual_gen.h` (D80); stands for ceval.c TARGET(BUILD_SLICE)'s PySlice_New |
| is_none | 5 | core | none |  |  |  |
| slice_index | 43 | mixed | Python/ceval.c:_PyEval_SliceIndex | P01, P12 | SHORTCUT: constant ints clip at compile time; a virtual int gives its word; a run-time int its one-digit value (PsycoLong_MediumValue), more digits a residual PyNumber_AsSsize_t on a respawn; PROMOTE: bound type; EFFECT: CfReturnTypeNative\|CfPyErrCheckMinus1 | core = virtual-int and one-digit paths; non-int bounds answer 0 (the real slice) |
| PsycoSlice_Unpack | 17 | mixed | partial: Objects/sliceobject.c:PySlice_Unpack | P18a | VIRTUAL: virtual slices with step None only | core = the virtual-slice test; defaults 0 and PY_SSIZE_T_MAX as PySlice_Unpack for step 1 |
| psy_sliceobject_init | 5 | glue | none |  | VIRTUAL: INIT_SVIRTUAL_NOCALL (identity, C17), weight 4 |  |

## Call path, merge points, frames, run time

### `ni/psyfunc.c`

Ni's compile-time Python-to-Python call: binds positional arguments and defaults the way `ceval.c:_PyEval_EvalCodeWithName` does, then inlines the callee or compiles it and emits a direct call **[read]**. Almost all of it is core (frame building, inlining, closures, genexpr fusion, the D53 `cannot_block` list); the translated parts are small: `Py_EnterRecursiveCall`/`Py_LeaveRecursiveCall` as emitted code, the builtins choice of `_PyFrame_New_NoTrack`, and the eval_breaker test **[read]**. The PsycoFunction proxy (about 300 lines) has no caller and is dead **[read]**; its fallback is Py2's `function_call`.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes, section banner | 34 | glue |  |  |  | Header comment describes the call path and says the proxy type is dead. |
| fix_run_time_args | 27 | core | none |  |  | Rewrites run-time argument sources to stack slots of the callee frame (Psyco calling convention). |
| struct fncall_arg_s | 8 | core | none |  |  |  |
| fncall_init | 17 | core | none |  | OTHER: supported-callee filter (no **kwargs, no own cells, nfrees must match) | Refusals send the call to fail_to_default; merge points None means not compilable. |
| fncall_bind | 37 | core | none |  | REF: owns a reference to every bound value; OTHER: no keywords passed (pfunction_call makes keyword calls real ones), no kwdefs (kw-only callees refused by psyco_call_pyfunc) | Calls the generated binder on an array shaped as vlocals, then makes unbound locals known NULL (psyco_vi_Zero) and appends the genexpr slots. Replaces fncall_collect_arguments (D85, GENERATION step 7). |
| _PsycoEval_EvalCodeWithName (psyfunc_gen.h) | generated | translated | Python/ceval.c:_PyEval_EvalCodeWithName (from the first use of f_localsplus to the generator test) | P18, P22 | VIRTUAL: *args a virtual tuple within CT_TUPLE_LOAD_SIZE_LIMIT; OTHER: anything without a compile-time form returns -1, the real call (D85) | Generated by tools/metac/gen_call.py; runs in the compiler, emits nothing. |
| fncall_finish_store | 14 | core | none |  | REF: co borrowed (C14) | Stores pr.co and builds pyc_data from merge points. |
| fncall_store_arguments | 5 | core | none |  |  | Now a move of the bound array into vlocals; its translated part (positional copy, *args tuple, free variables after the locals) is the generated binder (D85). |
| psyco_build_frame | 52 | core | none |  |  | Builds a PsycoObject for a callee; counterpart in spirit is Objects/frameobject.c:_PyFrame_New_NoTrack but nothing is translated. |
| call_with_inline_frame | 30 | core | none |  |  | EInline pseudo-exception; inlining mechanism. |
| psyco_save_inline_po | 29 | core | none |  |  | Caller's co/next_instr/serial as fixed constants (D49, C30). |
| psyco_restore_inline_po | 46 | core | none |  |  | Deferred decref of the inlined frame's locals (B6). |
| recursion section: comment, tstate_slot_t/int_slot_t, TSTATE_* and INT_value descriptors, tstate_current | 29 | core | none | P15 |  | Field descriptors for PyThreadState.recursion_depth/overflowed and a fixed-address slot; generator would need them from Include/pystate.h. |
| cimpl_leave_overflowed | 8 | translated | partial: Include/ceval.h:Py_LeaveRecursiveCall | P13 |  | The watermark test of _Py_MakeEndRecCheck; run-time helper. |
| load_tstate | 8 | translated | Include/pystate.h:PyThreadState_GET | P15 |  | Py_BUILD_CORE form: load of _PyRuntime.gilstate.tstate_current via pyruntime.c's address. |
| add_to_depth | 16 | translated | partial: Include/ceval.h:_Py_MakeRecCheck | P15 |  | ++/-- recursion_depth as emitted load/add/store. |
| enter_recursive_call | 23 | translated | Include/ceval.h:Py_EnterRecursiveCall | P05,P15,P12 | EFFECT: CfCommonIntZeroOk on _Py_CheckRecursiveCall; LIKELY: overflow is runtime_condition_f | Passes "" as 'where' (ceval passes "" too). |
| psyco_leave_recursive_call | 24 | translated | Include/ceval.h:Py_LeaveRecursiveCall | P05,P15,P13 | DRIFT: tests overflowed first, then the watermark in C (CPython tests the watermark and clears overflowed unconditionally); same effect; EFFECT: CfNoReturnValue |  |
| psyco_check_eval_breaker | 19 | mixed | partial: Python/ceval.c:_PyEval_EvalFrameDefault (eval_breaker test at loop top) | P05,P12,runtime-poll | EFFECT: CfCommonIntZeroOk on ni_eval_breaker_handler; LIKELY: set is runtime_condition_f | Core: where it is emitted (loop heads, after calls; C24, D45). |
| cannot_block table, psyco_cannot_block | 26 | core | none |  | EFFECT: hand list of C functions after which no eval_breaker test is needed (D53) | Policy table candidate; the property is not in any signature. |
| add_cannot_block | 5 | core | none |  |  |  |
| add_method | 7 | core | none |  |  | Looks up a builtin type's method descriptor at start-up. |
| add_builtin | 7 | core | none |  |  |  |
| cannot_block_init | 33 | core | none |  | EFFECT: the D53 list itself (constructors, int/float richcompare, list.append, dict views, 18 str methods, ord/chr/callable) | Policy data written as code. |
| tracing_on | 19 | mixed | partial: Python/ceval.c:_PyEval_EvalFrameDefault (tstate->use_tracing test at frame entry) | P05,P15 | LIKELY: tracing is runtime_condition_f; OTHER: when tracing, make the real call (C23, D46) | Core: the fallback decision and ni_trace_fallback switch. |
| psyco_tracing_on | 14 | core | none |  |  | Wrapper for genexpr fusion. |
| COMPUTE_DEFCOUNT | 12 | mixed | partial: Objects/call.c:_PyFunction_FastCallDict (argdefs != NULL) | P05,P22 | LIKELY: defaults present is runtime_condition_t | Core: needs a compile-time tuple length (PsycoTuple_Load). |
| callee_builtins | 22 | translated | partial: Objects/frameobject.c:_PyFrame_New_NoTrack (builtins choice) | P28 | DRIFT: a non-shared globals' __builtins__ read at compile time, later replacement unseen (F49) | The __builtins__ lookup itself is pycompiler.c:psy_get_builtins (translated twice in effect). |
| psyco_call_pyfunc (and MAX_CALL_FREES) | 207 | mixed | partial: Objects/call.c:_PyFunction_FastCallDict, Objects/call.c:function_code_fastcall | P04,P05,P12,P18,P22 | PROMOTE: none here (callee promoted by pfuncobject.c); FORGET: Psyco_ForgetClasses before a direct call (D59); REF: need_reference on cells of a run-time closure when inlining (C49); VIRTUAL: genexpr becomes PsycoGenexpr_New; OTHER: inlining rule (not into itself, MP_FLAGS_INLINABLE; C12), MAX_CALL_FREES 64, kw-only and unknown arity fall back; EFFECT: fallback PyFunction_Type.tp_call with CfCommonNewRefPyObject | Mostly core (inline/compile decision, recursion, tracing). Translated part: the fallback is funcobject.c:function_call. Proxy branch is dead. |
| psyco_call_closure | 37 | core | none | P05,P15 | GUARD: run-time compare func_globals == caller globals, else real call; OTHER: defaults wanted means real call (D58) |  |
| psyco_call_genexpr | 72 | core | none |  | FORGET: Psyco_ForgetClasses before the call | Genexpr fused with its consumer (D46); fatal on states the consumer is meant to have checked. |
| psyco_genexpr_fusable | 32 | core | none | P22 | VIRTUAL: fuse only a temporary (refcount <= 2), one argument, <= 64 frees |  |
| psyco_genexpr_fuse | 10 | core | none |  |  |  |
| section banner, psyco_PsycoFunction_NewEx | 41 | core | none |  |  | Obsolete proxy (psyco.bind); no caller in the tree [read]. |
| psycofunction_dealloc | 9 | core | none |  |  | Obsolete proxy. |
| psycofunction_call | 183 | mixed | partial: (Py2) Objects/funcobject.c:function_call |  | DRIFT: fallback is Py2's function_call (PyEval_EvalCodeEx with no closure, kwdefaults or name), 3.7's goes through _PyFunction_FastCallDict | Obsolete proxy; dead under the eval hook. Core: compile cache per arg count, psyco_processor_run. |
| psycofunction_traverse | 19 | core | none |  |  | Obsolete. Doesn't visit psy_code (code objects aren't GC-tracked, harmless). |
| psycofunction_clear | 19 | core | none |  |  | Obsolete. |
| PsycoFunction_Type | 28 | core | none |  |  | Obsolete; Py2 slot comments (tp_compare, tp_print). |
| psycofunction_init | 9 | glue |  |  |  | Also the check that pyruntime.c's tstate address matches PyThreadState_GET(), and cannot_block_init. |

### `ni/psyfunc.h`

Declarations for psyfunc.c plus the obsolete proxy type and `is_proxycode`, which `psyco_call_pyfunc` still tests on every call **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| header banner, guard, includes | 10 | glue |  |  |  |  |
| call-path declarations (psyco_call_pyfunc, psyco_call_closure, genexpr, inline po) | 24 | glue |  |  |  |  |
| PsycoFunctionObject, PsycoFunction_Type, PsycoFunction_Check | 20 | core | none |  |  | Obsolete proxy. |
| psyco_PsycoFunction_NewEx declaration | 5 | glue |  |  |  | No caller. |
| is_proxycode | 5 | core | none |  |  | Obsolete; still tested on every psyco_call_pyfunc. |
| psycofunction_init declaration, guard end | 4 | glue |  |  |  |  |

### `ni/mergepoints.c`

Bytecode analysis with no CPython counterpart: the opcode whitelist, merge-point placement, liveness of locals, inlinability, and where compiled code tests eval_breaker **[read]**. Only `NEXTOPARG` is a copy of ceval, and `BREAKER_CHECK` applies ceval's skip list; everything else is core and policy (MAX_MP_DISTANCE, the inlining weight, the frame-reading builtins list) **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes | 34 | glue |  |  |  | Header describes the whitelist, placement rules and liveness. |
| co_report_t | 15 | core | none |  |  |  |
| report_free | 16 | core | none |  |  |  |
| report_destructor | 6 | core | none |  |  |  |
| NEXTOPARG | 8 | translated | Python/ceval.c:NEXTOPARG (macro) |  |  | Copy of ceval's decoder. |
| _ENTER_INC, ENTER_NEXT, ENTER_JUMP_TO, NEXT_OFFSET | 27 | core | none |  | OTHER: every backward-jump target is a merge point and gets an eval_breaker test | Jump semantics (absolute vs relative, byte offsets) follow Python/ceval.c JUMPTO/JUMPBY. |
| MAX_MP_DISTANCE | 2 | core | none |  | OTHER: at most 170 instructions between merge points |  |
| INLINE_MAXIMUM_WEIGHT, IS_LIGHT_INSTR | 15 | core | none |  | OTHER: inlining limit 6 non-light instructions, hand list of light opcodes (upstream's rule) |  |
| BREAKER_CHECK | 13 | mixed | partial: Python/ceval.c:_PyEval_EvalFrameDefault (eval_breaker skip list) | runtime-poll |  | Skip list is mergepoints.h:BREAKER_SKIPS, a copy of ceval's; core: moving the test to the next unit. |
| MP_HERE | 5 | core | none |  |  |  |
| LIVE_WORDS, LIVE_TEST, _live_union | 16 | core | none |  |  |  |
| psyco_class_cell | 11 | core | none | P30 |  | Index of a __class__ free var (zero-arg super). |
| _find_dead_locals | 149 | core | none | P30 | OTHER: conservative successors (every enclosing SETUP handler, BREAK_LOOP to every loop exit, END_FINALLY to those and every CONTINUE_LOOP target); first argument kept live when there is a __class__ cell | Liveness (D15). No CPython counterpart; the successor rules are 3.7 block-stack semantics (Python/ceval.c), not read from any CPython analysis. |
| _find_merge_points | 172 | core | none |  | OTHER: merge-point placement (D10), MP_FLAGS_INLINABLE (one merge point, no block, no LOAD_GLOBAL, weight <= 6), eval_breaker after CALL_* only before a skipped opcode (D53) | Nearest CPython: Python/peephole.c:markblocks (jump-target marking). VLAs of 3 x code length on the C stack [inferred risk for very large code]. |
| frame_locals_builtins, loads_frame_locals_builtin | 17 | core | none | P30 | OTHER: code loading locals/vars/dir/eval/exec as a global is never compiled (C15) | By name only, so an alias or a shadowing global is not seen [inferred]. |
| _build_merge_points | 184 | core | none |  | OTHER: the opcode whitelist (any other opcode, module code, generators other than genexprs: not compiled); code rebinding arg 0 with a __class__ cell is not compiled | Opcode set from Include/opcode.h. DELETE_NAME and IMPORT_STAR are listed though unreachable in function code (no LOAD_NAME/STORE_NAME; import * in a function is a SyntaxError). |
| psyco_is_genexpr | 26 | core | none |  | OTHER: a genexpr is recognised by name "<genexpr>", one arg, no cells, one YIELD_VALUE |  |
| psyco_get_merge_points | 25 | core | none |  |  | Cached in co_extra; OOM returns None uncached (F27). |
| psyco_next_merge_point | 12 | core | none |  |  |  |
| psyco_first_merge_point | 7 | core | none |  |  |  |
| psyco_exact_merge_point | 10 | core | none |  |  |  |
| psyco_join_point | 25 | core | none |  |  | D44. |
| psyco_mp_flags | 7 | core | none |  |  |  |
| psyco_breaker_check_at | 6 | core | none |  |  | C24. |

### `ni/Python/frames.c`

Makes compiled frames visible to `sys._getframe` by walking the finfo chains; all core except `ni_getframe`, a partial copy of `sysmodule.c:sys_getframe` **[read]**. A negative depth behaves differently from CPython (DRIFT) **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, includes, banner | 17 | glue |  |  |  | Comment is accurate: no frame-getter hook, no f_lasti/f_lineno updates. |
| FRAME_STACK_ALLOC_BY, psyco_finfo | 37 | core | none |  |  | Bump allocator, never freed (by design). |
| frame_runtime_capsule_dealloc | 9 | core | none |  |  |  |
| frame_runtime_capsule_new | 22 | core | none |  |  |  |
| psyco_frame_start_register | 20 | core | none |  |  | Thread dict keyed by the real frame. |
| psyco_frame_start_get | 15 | core | none |  |  |  |
| psyco_frame_start_unregister | 12 | core | none |  |  |  |
| psyco_build_pyframe | 18 | core | none |  |  | Calls Objects/frameobject.c:PyFrame_New with tstate->frame cleared; f_lasti -1 marks emulated frames. |
| psyco_emulate_frame | 21 | core | none |  |  |  |
| struct sfitmp_s | 5 | core | none |  |  |  |
| pvisitframes | 108 | core | none |  |  | Walks tstate->frame and the finfo chains; comment still describes proxies. |
| visit_get_globals | 7 | core | none |  |  |  |
| psyco_get_globals | 8 | core | none |  |  | Fatal if no frame has globals. |
| visit_nth_frame | 10 | core | none |  |  |  |
| nth_frame | 5 | core | none |  |  |  |
| linked_frame | 15 | core | none |  |  | Sets f_back on emulated frames; quadratic in depth (re-walks per frame). |
| psyco_running_real_frame | 14 | core | none |  |  | Used by frame-reading metas (P30). |
| ni_getframe | 13 | mixed | partial: Python/sysmodule.c:sys_getframe |  | DRIFT: a negative depth raises ValueError, CPython returns the top frame (its loop is 'while (depth > 0 ...)') | Translated: argument parsing and error message; core: counting compiled frames. KNOWN_BUGS F100. |
| ni_getframe_def | 6 | glue |  |  |  |  |
| psyco_frames_init | 12 | glue |  |  |  | Replaces sys._getframe (C11). |

### `ni/Python/frames.h`

Declarations and the frame-info record; the comment on `psyco_emulate_frame` is stale (describes upstream's `psyco_get_frame`) **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| banner, guard, includes | 13 | glue |  |  |  |  |
| psyco_emulate_frame, psyco_running_real_frame, psyco_get_globals declarations | 15 | glue |  |  |  | The comment on psyco_emulate_frame describes upstream's psyco_get_frame (integer argument), which no longer exists: stale. |
| struct stack_frame_info_s | 7 | core | none |  |  |  |
| finfo_last | 8 | core | none |  |  |  |
| psyco_finfo declaration | 3 | glue |  |  |  |  |
| FrameRuntime | 8 | core | none |  |  |  |
| frame-start registry declarations, guard end | 6 | glue |  |  |  |  |

### `ni/Python/pyruntime.c`

The one Py_BUILD_CORE file: two addresses inside `_PyRuntime` (version-specific mechanisms) and a hand copy of ceval's eval_breaker block **[read]**. The handler is translated code a generator must reproduce from `ceval.c`; the skip list is copied a second time here (also in `mergepoints.h:BREAKER_SKIPS`) **[read]**.

| function | lines | class | CPython 3.7 counterpart | patterns | policy | notes |
|---|---|---|---|---|---|---|
| file comment, Py_BUILD_CORE, includes | 10 | glue |  |  |  |  |
| ni_tstate_current_address | 6 | glue | Include/pystate.h:PyThreadState_GET (Py_BUILD_CORE form) |  |  | Version-specific mechanism (3.7 _PyRuntime.gilstate). |
| eval_breaker _Static_assert, ni_eval_breaker_address | 8 | glue | Include/internal/ceval.h:struct _ceval_runtime_state.eval_breaker |  |  | Version-specific mechanism. |
| ni_eval_breaker_handler | 29 | translated | partial: Python/ceval.c:_PyEval_EvalFrameDefault (eval_breaker block) | runtime-poll | DRIFT: GIL drop via PyEval_SaveThread/PyEval_RestoreThread instead of drop_gil/take_gil with ceval's tstate mix-up checks (same finalising exit) | UNSIGNAL_ASYNC_EXC/COMPUTE_EVAL_BREAKER expanded by hand. The skip list is not here (callers). |
| ni_eval_breaker_at_entry | 18 | mixed | partial: Python/ceval.c:_PyEval_EvalFrameDefault (eval_breaker block, skip list, error: PyTraceBack_Here) | runtime-poll | DRIFT: on error only PyTraceBack_Here, not call_exc_trace (no tracer while compiled code runs, D46) | Skip list duplicated from mergepoints.h:BREAKER_SKIPS. Core: called for NiCode_Run entry. |
