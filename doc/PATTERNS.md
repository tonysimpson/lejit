---
written: 2026-11-16
examples_checked: e404100
---
# Translation patterns: CPython C to compile-time C

This is the catalogue of rewrites that turn a CPython C function into its Psyco meta-function (compile-time form). It is the **specification for the meta-compiler**.

Each pattern has:
- an ID (cite it from code comments and other docs);
- the CPython form and the Psyco form;
- a real example (`file:function`);
- a **kind**:
  - **M**: mechanical. A generator could do it from the C source plus data derivable from CPython headers.
  - **J**: judgement. Needs knowledge or heuristics that aren't in the C source. These become annotations or hand-written overrides.
  - **M/J**: the rewrite is mechanical but choosing to apply it is judgement.

All examples are **[read]** unless marked otherwise. Many of the hand-written examples are now generated at build time (`tools/metac/gen_*.py`, into the build's `*_gen.h`); such an example names the CPython function the generator reads, and the generator applies the same pattern.

## A. Type and value knowledge

**P01 promote-type** (M)
`Py_TYPE(o)` → `tp = Psyco_NeedType(po, o); if (tp == NULL) return NULL;`. If the type is run-time, this raises a promotion pseudo-exception, so the opcode is restarted once the type is known.
Example: nearly every `PsycoNumber_*`/`PsycoObject_*`, generated from `abstract.c` (`PyNumber_Add` and the rest, `gen_abstract.py`); hand-written, `pabstract.c:PsycoObject_Call`. `Psyco_FastType` is the variant for when promotion has already happened.

**P02 verify-type** (M)
`if (PyTuple_Check(x))` → `switch (Psyco_VerifyType(po, x, &PyTuple_Type)) { case true: …; case false: …; default: return NULL; }`. It's three-way: true, false, or error/promotion.
Example: tuple's `sq_concat`, generated from `tupleobject.c:tupleconcat` (D80).

**P03 promote-value** (M/J)
Any C value used to choose a code path (a `PyMethodDef*`, a flag) → `psyco_atcompiletime(po, vi)` or `psyco_pyobj_atcompiletime`, where -1/NULL means promotion or error. Which values are worth promoting is the judgement.
Example: `ni/Objects/pmethodobject.c:PsycoCFunction_Call` promotes `m_ml` and then switches on `ml_flags` as in CPython.

**P04 promote-callee** (J)
The callable is promoted with a megamorphic cut-off (`psyco_pyobj_atcompiletime_mega`), and the call is then specialised. After `MEGAMORPHIC_MAX` distinct values it becomes a residual call.
Example: `ni/Objects/pfuncobject.c:pfunction_call`.

**P05 runtime-branch** (M; likelihood hint J)
`if (i < 0)` on a run-time value → `cc = integer_cmp_i(po, i, 0, Py_LT); if (cc == CC_ERROR) …; if (runtime_condition_f(po, cc)) { … }`. The unlikely branch is compiled lazily. `_t`/`_f` names the expected outcome, which is the judgement part; a generator could default it or take a hint.
Example: `PsycoSequence_GetItem`, generated from `abstract.c:PySequence_GetItem`; `structmember.c`'s NULL tests in the generated `PsycoMember_GetOne` (`integer_non_null` + `runtime_condition_t`).

**P06 assert-range** (M)
On the branch where a sign test failed, record the fact: `assert_nonneg(i)`. Later folds use it, and it enables the `RunTime_NonNeg` flag.
Example: `PsycoSequence_GetItem`, generated from `abstract.c:PySequence_GetItem`.

**P07 unsigned-bounds** (M when it mirrors a `(size_t)` cast; J otherwise)
`(size_t)i >= (size_t)n` → `integer_cmp(po, i, n, Py_GE | COMPARE_UNSIGNED)`. `CHEAT_MAXINT` (clamping slice bounds) is a Psyco-only trick.
Example: tuple's `sq_item`, generated into `ptupleobject_gen.h` (`gen_abstract.bounds_test`, `935df0b`; was `ptupleobject.c:ptuple_item`), `pstringobject.c:pstring_slice` (Py2 only).

**P08 assert-result-type** (J)
A residual call whose result type is known → `Psyco_AssertType(po, r, &PyX_Type)`, or the `DEF_KNOWN_RET_TYPE_n` macros. The knowledge isn't in the callee's C signature; a generator could get some of it by analysing the callee's return statements.
Example: `plongobject.c` (`RETLONG`/`RETFLOAT`), `pdictobject.c`.

## B. Dispatch

**P09 meta-dispatch** (M, given a per-slot table)
`m->nb_add(v, w)` → `Psyco_META2(po, m->nb_add, CfCommonCheckNotImplemented, "vv", v, w)`. If a meta function is registered for that C pointer it's called directly, and the flags and format are ignored. Otherwise it becomes a residual call (P12).
- **Constraint (from review):** the meta function's C signature is defined by the format string at the *call site*. `v` means a `vinfo_t*` and `l` means a raw compile-time value. Every call site for a given slot must agree, and nothing checks this. Example: `tp_setattro` is called `"vlv"`, so the name arrives as a raw `PyObject*`; `tp_descr_get` metas are written for `"lvl"`.
- **The meta-compiler needs, as data, one canonical meta signature (v or l per argument) and one `Cf*` combination per slot.**
- **Doubles can't pass through META**: arguments are cast to `long`. [inferred]

Example: `ni/Objects/pobject.c:PsycoObject_IsTrue` (`nb_bool`, `mp_length`, `sq_length`), `ni/Python/pycompiler.c:Psyco_Meta1x/2x/3x`.

**P29 guarded-slot** (M, given which types are mutable)
Every slot read `tp->tp_as_X->slot` on a type that may be a heap type becomes `Psyco_TypeSlot(po, tp, &tp->tp_as_X->slot, &f)` (the generator's rule `slot_guard`, D54): a static type's slot is read now; a heap type's is the value first seen, behind a run-time compare (D48). The path where it changed calls what CPython calls *from that point*: the whole function (`PyObject_GetItem`, `PyObject_IsTrue`) when nothing has been called yet, otherwise a generated C copy of the rest of the algorithm (`cimpl_NAME`, as for `binary_op1`), so no slot runs twice. `PyType_IsSubtype(a, b)` on a heap `a` is the same with `tp_mro` held (`Psyco_TypeIsSubtype`). The read is mechanical; choosing the resume point is mechanical too given the C function's control flow (every slot read before the first call resumes at the function's start). A promoted function's `func_code` and `func_defaults` are the same pattern on an object (`pfuncobject.c:compiled_code`, `compiled_defaults`). [run]
Example: generated `binary_op1` and `PsycoObject_GetItem` (F56, F57); hand-written, `pobject.c:PsycoObject_IsTrue`, `try_rich_compare`, `pabstract.c:PsycoObject_GetIter`.

**P10 module-fn-meta** (M)
A builtin module function's meta is registered on its C pointer with `Psyco_DefineMetaModule(name)` + `Psyco_DefineModuleFn(md, name, ml_flags, meta)`. The registration does nothing if `ml_flags` don't match exactly, for example after Argument Clinic changed a function to `METH_FASTCALL`, because the meta's arguments and its residual fallback follow the convention it was written for; it is then listed by `ni._unregistered_metas()` (printed in checked builds, and `tests/compile_bounds.py` fails). A `METH_FASTCALL` meta takes its arguments as a tuple and falls back through `PsycoCFunction_CallFast` (D36).
Example: `ni/Python/pbltinmodule.c:psyco_bltinmodule_init` imports `"builtins"` and registers 7 hand-written builtins this way, plus `type_call` with `Psyco_DefineMeta`; `ni/Modules/pmath.c` registers `ceil` and `floor`. `gen_builtins.py` (D86) generates the others, reading the flags from the method table and the meta's arguments and fallback from `policy_table.METH_ARGS`, so they always match the source they were generated from (the math functions on doubles likewise, D87). [read]

**P11 slot-redirect** (J)
A meta on a slot that returns a different, more optimisable object than CPython would.
Example: `ni/Objects/plistobject.c:psy_listobject_init` registers `PyList_Type.tp_iter → PsycoSeqIter_New` (a generic virtual seq-iterator instead of `list_iterator`). This is observable if the iterator escapes. [inferred]

## C. Calls into CPython

**P12 residual-call** (M; the default)
No translation available → `psyco_generic_call(po, c_fn, Cf…, "fmt", args…)`. It emits a real call. Argument tags are `v` vinfo, `l` raw, `d` double, `r` by-reference, and `a`/`A` output array. The `Cf*` flags give the return type and error convention (`ni/cf_flags.h`).
Example: `PsycoSequence_GetSlice` (generated from `abstract.c`), `pdictobject.c`, and every fallback path.
An opcode with `oparg` stack items (BUILD_*_UNPACK) passes them as one virtual tuple (`PsycoTuple_New` over the stack), and the helper is ceval's case body looping over the tuple instead of `PEEK(i)`; these cases are now generated from `ceval.c` (D88, D93).
An opcode that makes several new references returns them through an `A` output array from one helper, and the opcode then rearranges the stack with no failure point left (restart rule): `cimpl_unpack_ex` (ceval's `unpack_iterable` writing upwards instead of `*--sp`), `cimpl_setup_with` (the bound `__exit__` and `__enter__`'s result). The block opcodes are not mechanical: `with` runs on Ni's finally states (KNOWN_BUGS F18), so `WITH_CLEANUP_START` decides from the block stack which of ceval's cases it is in, and `WITH_CLEANUP_FINISH` unwinds a silenced exception itself instead of pushing `WHY_SILENCED` (`pycompiler.c`). 3.11 replaced these opcodes with `BEFORE_WITH` and exception tables, so this part is not worth generating.

**P13 pure-helper-call** (M)
Express an operation as a tiny static C helper called with `CfPure | CfReturnType…`. With compile-time arguments it's evaluated at compile time (via libffi in `ni/x64/iencoding.c:compile_time_call`); otherwise it's a plain call.
Example: float's `nb_add`, generated from `floatobject.c:float_add` with its arithmetic copied into a helper (rule `copy_through`, D87); the generated math functions (libm directly); `pmath.c:cimpl_floor_to_int`.

**P14 ct-fold** (rule M; decision J)
If every input is compile-time, compute the result now and return a constant. It happens automatically for `CfPure` residual calls (P13). Explicit cases are hand-written.
Example: `ni/Objects/ptupleobject.c:compute_tuple` (all items constant → build the real tuple at compile time), `pstringobject.c` concat (Py2).

## D. Memory and fields

**P15 field-access** (M, given descriptors generated from CPython headers)
`o->field` / `Py_SIZE(o)` / `ob_item[i]` → `psyco_get_field(po, o, DESC)`, `psyco_get_const` (immutable, cached, borrowed), `psyco_get_field_array`, `psyco_put_field[_array]`. Descriptors (`DEF_FIELD(struct, ctype, field, prev)` + `FMUT`/`FPYREF`/`FARRAY`) can be generated from struct definitions. **Whether a field is mutable is judgement** (e.g. `FIX_size` vs `VAR_size`).
Example: tuple's generated `sq_length` and `sq_item` (`FIX_size`, `FPYREF(TUPLE_ob_item)`; was `pabstract.c:psyco_generic_immut_ob_size`), `PsycoMember_GetOne`, generated from `structmember.c:PyMember_GetOne` (each `T_*` to a descriptor, D89).

**P15a inline-split-store** (J)
A store into a container whose layout is shared and only appended to (3.7's split dict on a class's keys) is done inline after checking the live layout against a cache, with the C function's order kept: incref, store, bookkeeping (`ma_used`, version tag), the old value's decref last. Which cases stay inline (the existing slot, the next slot in order) is derived from the C source (`insertdict`'s split-table branch); the cache, the GC-tracking shortcut and the version counter are judgement.
Example: `pobject.c:inline_store` (D38).

**P16 forget-mutable-field** (M)
After a virtual object is materialised, drop cached knowledge of its mutable fields (`psyco_forget_field`), because others may now mutate them.
Example: `piterobject.c:compute_seqiter`, `plistobject.c:compute_vlist`.

**P17 refcount** (M)
`Py_INCREF`/`Py_DECREF` → `vinfo_incref`/`vinfo_decref` (compiler-side ownership of descriptors) **and** emitted `psyco_incref_*`/`psyco_decref_*`/`need_reference` for run-time references. The `release_me` temporary idiom is used throughout.
Example: `plistobject.c:plist_ass_item` mirrors `list_ass_item`'s old-item decref; `pstructmember.c` uses `need_reference` on a borrowed load.

## E. Virtual objects (the main source of speed)

**P18 virtual-object** (J)
`PyX_New(…)` + initialise → build a `VirtualTime` vinfo whose array holds the fields, plus:
- `compute_fn`, which emits the real allocation when the object escapes (shape: read sub-vinfos → residual constructor → `psyco_put_field` extras → `vinfo_move`);
- optionally `direct_compute_fn`, which builds it from raw frame data;
- a `nested_weight`.

Choosing what to virtualise, and the weights, are judgement. The compute functions themselves are close to mechanical once the layout is chosen.

Live examples: tuple, short list (`VLIST_LENGTH_MAX` 3), seq-iterator, function, C method, bound method, float, int, bool, range and range iterator, slice (`doc/history/removed-mechanisms.md` lists those removed in the port: int, bool, range, strings, compact objects; int, bool and range came back since). Layouts are in each `ni/Objects/p*.h`.

**P18a virtual-argument** (helper M; choosing J)
An object built only to be unpacked by its consumer (a slice for `mp_subscript`) is virtual, and each consumer's meta unpacks its fields into words and calls a helper that is the consumer's C code from just after the unpack. The helper is mechanical: the C function's branch for the handled case (step 1), with the unpacked locals as parameters; the unpack itself (`PySlice_Unpack`, `_PyEval_SliceIndex`) becomes compile-time reads of the virtual's fields, done only for the field types it can read without side effects (None and ints, not `__index__`). Which objects are worth it, and which cases, are judgement; anything not handled computes the real object, so the generic path stays CPython's.
Example: `psliceobject.c:PsycoSlice_Unpack`, used by `punicodeobject.c:cimpl_unicode_slice`, `pbytesobject.c:cimpl_bytes_slice`, `plistobject.c:cimpl_list_slice` and `cimpl_list_ass_slice` (D56). With constant inputs and an immutable exact type the helper is CfPure and folds (P14).

**P19 virtual-fast-path** (J)
Inside a translated function: if the argument is a known virtual of this type and the index is compile-time, operate on the vinfo array directly instead of emitting loads and stores.
Example: `plistobject.c:plist_item`, `plist_ass_item`.

**P20 virtual-peephole** (J)
Algebraic shortcuts on virtuals: a slice of a slice, growing a concatenation buffer in place.
Example: `pstringobject.c:pstring_slice`, `cimpl_cb_grow` (Py2 only).

**P21 identity-shortcut** (J)
`a + ''` → `a`, `a[0:len]` → `a`, `vinfo_known_equal` in compares. Needs exact-type checks so it doesn't return a subclass instance.
Example: `pstringobject.c` (Py2); tuple's `sq_concat`, generated from `tupleconcat`'s empty-operand shortcut with its `PyTuple_CheckExact` test (the hand version missed the test, KNOWN_BUGS F40, found by CPython's `test_descr`). [run]

**P22 load-sequence** (J)
`PsycoTuple_Load`/`PsycoList_Load` gets a compile-time length and the item vinfos, so the code can specialise on arity. Returns -1 if unknown. Compile-time tuples longer than `CT_TUPLE_LOAD_SIZE_LIMIT` (15) are refused.
Example: `pmethodobject.c:PsycoCFunction_Call` (METH_O/NOARGS), `pclassobject.c`, `pmath.c`, `psyfunc.c:psyco_call_pyfunc`.

**P23 bool-select** (J)
Condition code → a virtual bool: a virtual int of type bool whose word is the condition. It is boxed with `PyBool_FromLong` only if it escapes; a jump on it uses the flags directly. Upstream had the same.
Example: `ni/Objects/pboolobject.h:PsycoBool_FromCondition` (`3702e3f`).

## F. Protocols and errors

**P24 exception-set** (M)
`PyErr_SetString(e, msg)` / `PyErr_Format` → `PycException_SetString(po, e, msg)` / `PycException_SetFormat`. It returns NULL (or `CC_ERROR`) exactly as the C does.
Example: `pabstract.c:type_error_tp`, tuple's generated `sq_item`.

**P25 notimplemented-protocol** (M)
`if (res != Py_NotImplemented) return res; Py_DECREF(res);` → `if (IS_IMPLEMENTED(res)) return res; vinfo_decref(res, po);`. This works on the residual path only if the call used `CfCommonCheckNotImplemented`, which maps a run-time NotImplemented to the unique `psyco_vi_NotImplemented()`.
Example: `pobject.c:try_rich_compare`, the generated `binary_op1`.

**P26 type-whitelist** (J)
Specialise only for a hand-picked set of types and make everything else residual.
Example: `pabstract.c:PsycoNumber_Power` (int/float only), `pfloatobject.c:pfloat_richcompare` (float/float only), `pobject.c:Psyco_SafelyDeleteVar` (types known to have no `__del__`).

**P27 control-flow-flag-to-site** (M)
A ceval case that leaves by a different label on some outcomes (`do_raise` returning 1 sends RAISE_VARARGS to `exception_unwind`, past `error:`'s `PyTraceBack_Here`) → a residual helper per outcome that does the work of the label it skips, and a compile-time test at the shared unwind code for the opcode that took the other exit. The generator must read which label each return value of a helper reaches. [read]
Example: `pycompiler.c:cimpl_reraise` (the RuntimeError path records its own traceback entry) and the unwind loop's `RAISE_VARARGS && oparg == 0` test (KNOWN_BUGS F18).

**P28 frame-field-as-slot** (J)
A frame field that CPython fixes when the frame starts and the case bodies read (`f->f_builtins`, set in `_PyFrame_New_NoTrack` from the caller or the globals) → a compile-time slot in `po->vlocals`, set where Ni builds a frame (eval-hook entry, direct calls), not recomputed from other fields at each use. Judgement: which fields are fixed per frame, and the rule for frames Ni builds itself. A field in `pr` would be wrong, since only `vlocals` is compared at merge points.
Example: `LOC_BUILTINS` (`pycheader.h`, `psyfunc.c:callee_builtins`; KNOWN_BUGS F49, D52).

**P30 frame-read-to-compiled-frame** (J)
C code that reads the running frame (`PyThreadState_GET()->frame`: its code, locals or cells) → a meta that passes those values from the compiled frame's `po->vlocals` and `po->pr.co` to a residual helper doing the rest of the C function, because a directly called compiled function has no real frame. The liveness pass must keep any local the C code reads implicitly. Which builtins read the frame is knowledge the C source shows only through the `tstate->frame` read.
Example: `pbltinmodule.c:psuper_noargs` and `cimpl_super_noargs` (typeobject.c:super_init without arguments; D58), `mergepoints.c:_find_dead_locals` keeping the first argument of code with a `__class__` cell; `pbltinmodule.c:pbuiltin_globals` (F47).

**P31 assignable-type-fact** (J)
C code that can change an object's type (`typeobject.c:object_set_class`: only between heap types, or ModuleType subtypes) → a known type is a fact that any call running Python code may falsify, so the specialiser drops it after such a call and learns it again on use; a static type's fact is kept. The generator must know which C functions run no Python code (CfPure, CfNoPython) and which types a dealloc can't reach Python from: neither is in the signatures.
Example: `pobject.c:Psyco_ForgetClasses`, `Psyco_ClassMutable` (KNOWN_BUGS F55, D59).

## Core and backend mechanisms (not translations, but named for reference)

- **inline-cache-patch**: a promotion site is an inline `CMP imm64; JE` for the first value seen, then an out-of-line chain of the same compares for up to four more, in front of the C dispatcher (D42). `ni/x64/idispatcher.h:ipromotion_finish`, `dispatcher.c:promotion_cache_add`.
- **lazy-branch-proxy**: a branch to code not yet compiled goes through a stub that compiles it and then repoints the jump. Coding pauses and respawn use the pooled 15-byte stubs (`dispatcher.c:psyco_new_stub`, D32); a promotion site's miss path is the same stub written in place after `mov rsi, reg` (`x64/idispatcher.h:ipromotion_finish`, `dispatcher.c:psyco_new_stub_at`). Jumps: `idispatcher.h:conditional_jump_to`/`change_cond_jump_target`.
- **repairable-shared-guard**: a guard whose expected values live in a struct shared by every site compiled to the same decision, keyed by what the decision depends on, so the failure path can recompute the decision and, if unchanged, reset the guard; the sites need no recompile and a replay needs no guard pointer. `ni/Objects/pobject.c:store_guard` (D39).
- **runtime-poll**: a check of interpreter state that ceval makes between instructions, emitted as an inline compare of a word read through a fixed address (RIP-relative), with a respawn point to a residual call into a C copy of ceval's handling only when it is set. A generator must find where ceval polls (`DISPATCH()` against `FAST_DISPATCH()`, the skip list at the loop top) and choose the subset compiled code keeps: loop heads and after calls. `ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h:ni_eval_breaker_handler` (D77), `ni/psyfunc.c:psyco_check_eval_breaker`, `ni/mergepoints.c:BREAKER_CHECK` (D45). The recursion depth (C12) is the same shape.
- **serial-gated-decision**: a compile-time choice that depends on run-time state which changes one way only is made a function of that state and the block serial (`codemanager.h:psyco_block_serial`), which a replay shares with its original compile, so the replay decides the same. `pobject.c:keys_inline` (D31). (`store_cache_t.ready` was one, until misses stopped respawning, D47.)
- **content-checked-cache**: a run-time cache that holds no reference and is checked against the live structure it describes, so a stale or reused address only costs a miss; the code reads the cache at run time and never depends on its contents. `pobject.c:store_cache`, `inline_store` (D38).
- **cc-virtual**: processor flags are held as virtual values in `po->ccregs` and materialised only when about to be clobbered. `ni/codegen.c:psyco_vinfo_condition`, `ni/x64/iencoding.c:psyco_compute_cc`.

## Rules any generator must respect

1. **Restart safety.** An opcode or meta function can be abandoned part-way through by a promotion and then re-run from the start. Nothing with a compile-time side effect may happen before the last point that can fail or promote ("use TOP() not POP()"; `ni/Python/pycompiler.c` main loop comment). [read] Emitted residual calls count as side effects: code emitted before a promotion has already run, so the restarted instruction runs it again. Hoist every promotion (`Psyco_NeedType` of all operands) before the first residual call. Example: `pbltinmodule.c:pbuiltin_min_max` (a user `__gt__` ran twice, `0feefaf`). Only values in the frame can be promoted, never temporaries (KNOWN_BUGS F24): where a meta chains operations, an intermediate of unknown type goes to the residual CPython function instead (`pbuiltin_sum`, a user `__add__`, `d487bbb`). A promoted constant is held as long as the compiled code, so a meta that promotes a value which may be a heap object uses `psyco_atcompiletime_static_type` (KNOWN_BUGS F25). [run]
2. **Error conventions are per call.** Metas return NULL / `CC_ERROR`. Residual calls get theirs from `CfPyErr*`. The generator needs the C function's error convention as data (most can be derived from CPython docs and return types). [read]
3. **Per-slot meta signatures and `Cf*` flags are data** (P09). Today they exist only implicitly in call sites. [read]
4. **Mixed values.** A translated function's arguments can be `vinfo_t*` or raw compile-time values (the `l` tag). The generator must decide this per argument and per slot. [read]
5. **Version drift is the problem being solved.** Hand translations drift from the CPython they mirror, and a generator reading the 3.7 source avoids that. The two old examples are fixed (`pabstract.c:binary_op1` follows 3.7, with no `Py_TPFLAGS_CHECKTYPES`; `pbltinmodule.c` imports `builtins`). Where a hand translation differed from 3.7 (eight such differences were bugs, KNOWN_BUGS F97–F104), a faithful generated translation shows in same code as a difference with a reason. What is still hand-written, and why, is `tools/metac/donecheck.py`'s list (D90). [read]
