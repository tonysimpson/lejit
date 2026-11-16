---
written: 2026-11-16
head_ref: e1fd808
sources: reviews of ni/Objects, ni/Modules, ni/Python, ni/x64 (2026-11-16)
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

All examples are **[read]** unless marked otherwise.

## A. Type and value knowledge

**P01 promote-type** (M)
`Py_TYPE(o)` → `tp = Psyco_NeedType(po, o); if (tp == NULL) return NULL;`. If the type is run-time, this raises a promotion pseudo-exception, so the opcode is restarted once the type is known.
Example: `ni/Objects/pabstract.c:PsycoNumber_Add` and nearly every `PsycoNumber_*`/`PsycoObject_*`. `Psyco_FastType` is the variant for when promotion has already happened.

**P02 verify-type** (M)
`if (PyTuple_Check(x))` → `switch (Psyco_VerifyType(po, x, &PyTuple_Type)) { case true: …; case false: …; default: return NULL; }`. It's three-way: true, false, or error/promotion.
Example: `ni/Objects/pabstract.c:PsycoEval_CallObjectWithKeywords`, `ptupleobject.c:PsycoTuple_Concat`.

**P03 promote-value** (M/J)
Any C value used to choose a code path (a `PyMethodDef*`, a flag) → `psyco_atcompiletime(po, vi)` or `psyco_pyobj_atcompiletime`, where -1/NULL means promotion or error. Which values are worth promoting is the judgement.
Example: `ni/Objects/pmethodobject.c:PsycoCFunction_Call` promotes `m_ml` and then switches on `ml_flags` as in CPython.

**P04 promote-callee** (J)
The callable is promoted with a megamorphic cut-off (`psyco_pyobj_atcompiletime_mega`), and the call is then specialised. After `MEGAMORPHIC_MAX` distinct values it becomes a residual call.
Example: `ni/Objects/pfuncobject.c:pfunction_call`.

**P05 runtime-branch** (M; likelihood hint J)
`if (i < 0)` on a run-time value → `cc = integer_cmp_i(po, i, 0, Py_LT); if (cc == CC_ERROR) …; if (runtime_condition_f(po, cc)) { … }`. The unlikely branch is compiled lazily. `_t`/`_f` names the expected outcome, which is the judgement part; a generator could default it or take a hint.
Example: `ni/Objects/pabstract.c:PsycoSequence_GetItem`, `pstructmember.c` NULL tests (`integer_non_null` + `runtime_condition_t`).

**P06 assert-range** (M)
On the branch where a sign test failed, record the fact: `assert_nonneg(i)`. Later folds use it, and it enables the `RunTime_NonNeg` flag.
Example: `ni/Objects/pabstract.c:PsycoSequence_GetItem`.

**P07 unsigned-bounds** (M when it mirrors a `(size_t)` cast; J otherwise)
`(size_t)i >= (size_t)n` → `integer_cmp(po, i, n, Py_GE | COMPARE_UNSIGNED)`. `CHEAT_MAXINT` (clamping slice bounds) is a Psyco-only trick.
Example: `ni/Objects/ptupleobject.c:ptuple_item`, `pstringobject.c:pstring_slice` (Py2 only).

**P08 assert-result-type** (J)
A residual call whose result type is known → `Psyco_AssertType(po, r, &PyX_Type)`, or the `DEF_KNOWN_RET_TYPE_n` macros. The knowledge isn't in the callee's C signature; a generator could get some of it by analysing the callee's return statements.
Example: `ni/Objects/pobject.c:PsycoObject_Repr`, `plongobject.c` (`RETLONG`/`RETFLOAT`), `pdictobject.c`.

## B. Dispatch

**P09 meta-dispatch** (M, given a per-slot table)
`m->nb_add(v, w)` → `Psyco_META2(po, m->nb_add, CfCommonCheckNotImplemented, "vv", v, w)`. If a meta function is registered for that C pointer it's called directly, and the flags and format are ignored. Otherwise it becomes a residual call (P12).
- **Constraint (from review):** the meta function's C signature is defined by the format string at the *call site*. `v` means a `vinfo_t*` and `l` means a raw compile-time value. Every call site for a given slot must agree, and nothing checks this. Example: `tp_setattro` is called `"vlv"`, so the name arrives as a raw `PyObject*`; `tp_descr_get` metas are written for `"lvl"`.
- **The meta-compiler needs, as data, one canonical meta signature (v or l per argument) and one `Cf*` combination per slot.**
- **Doubles can't pass through META**: arguments are cast to `long`. [inferred]

Example: `ni/Objects/pobject.c:PsycoObject_IsTrue` (`nb_bool`, `mp_length`, `sq_length`), `ni/Python/pycompiler.c:Psyco_Meta1x/2x/3x`.

**P10 module-fn-meta** (M)
A builtin module function's meta is registered on its C pointer with `Psyco_DefineMetaModule(name)` + `Psyco_DefineModuleFn(md, name, ml_flags, meta)`. The registration silently does nothing if `ml_flags` don't match exactly, for example after Argument Clinic changed a function to `METH_FASTCALL`.
Example: `ni/Modules/pmath.c` (the `PMATH_FUNC1/2` macros are themselves a small hand-written meta-compiler). `ni/Python/pbltinmodule.c` uses it too, but imports `"__builtin__"` and so registers nothing on Py3.

**P11 slot-redirect** (J)
A meta on a slot that returns a different, more optimisable object than CPython would.
Example: `ni/Objects/plistobject.c:psy_listobject_init` registers `PyList_Type.tp_iter → PsycoSeqIter_New` (a generic virtual seq-iterator instead of `list_iterator`). This is observable if the iterator escapes. [inferred]

## C. Calls into CPython

**P12 residual-call** (M; the default)
No translation available → `psyco_generic_call(po, c_fn, Cf…, "fmt", args…)`. It emits a real call. Argument tags are `v` vinfo, `l` raw, `d` double, `r` by-reference, and `a`/`A` output array. The `Cf*` flags give the return type and error convention (`ni/cf_flags.h`).
Example: `pabstract.c:PsycoSequence_GetSlice`, `pdictobject.c`, and every fallback path.

**P13 pure-helper-call** (M)
Express an operation as a tiny static C helper called with `CfPure | CfReturnType…`. With compile-time arguments it's evaluated at compile time (via libffi in `ni/x64/iencoding.c:compile_time_call`); otherwise it's a plain call.
Example: `ni/Objects/pfloatobject.c:cimpl_fp_add`, `ni/Modules/pmath.c` (libm directly).

**P14 ct-fold** (rule M; decision J)
If every input is compile-time, compute the result now and return a constant. It happens automatically for `CfPure` residual calls (P13). Explicit cases are hand-written.
Example: `ni/Objects/ptupleobject.c:compute_tuple` (all items constant → build the real tuple at compile time), `pstringobject.c` concat (Py2).

## D. Memory and fields

**P15 field-access** (M, given descriptors generated from CPython headers)
`o->field` / `Py_SIZE(o)` / `ob_item[i]` → `psyco_get_field(po, o, DESC)`, `psyco_get_const` (immutable, cached, borrowed), `psyco_get_field_array`, `psyco_put_field[_array]`. Descriptors (`DEF_FIELD(struct, ctype, field, prev)` + `FMUT`/`FPYREF`/`FARRAY`) can be generated from struct definitions. **Whether a field is mutable is judgement** (e.g. `FIX_size` vs `VAR_size`).
Example: `pabstract.c:psyco_generic_immut_ob_size`, `pstructmember.c:PsycoMember_GetOne` (maps each `T_*` to a descriptor).

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

Live examples: tuple, short list (`VLIST_LENGTH_MAX` 3), seq-iterator, function, C method, bound method, float (`doc/history/removed-mechanisms.md` lists removed ones: int, bool, range, strings, compact objects). Layouts are in each `ni/Objects/p*.h`.

**P19 virtual-fast-path** (J)
Inside a translated function: if the argument is a known virtual of this type and the index is compile-time, operate on the vinfo array directly instead of emitting loads and stores.
Example: `plistobject.c:plist_item`, `plist_ass_item`.

**P20 virtual-peephole** (J)
Algebraic shortcuts on virtuals: a slice of a slice, growing a concatenation buffer in place.
Example: `pstringobject.c:pstring_slice`, `cimpl_cb_grow` (Py2 only).

**P21 identity-shortcut** (J)
`a + ''` → `a`, `a[0:len]` → `a`, `vinfo_known_equal` in compares. Needs exact-type checks so it doesn't return a subclass instance.
Example: `pstringobject.c` (Py2), `ptupleobject.c:PsycoTuple_Concat` (empty operand; missing the `CheckExact` test). [inferred]

**P22 load-sequence** (J)
`PsycoTuple_Load`/`PsycoList_Load` gets a compile-time length and the item vinfos, so the code can specialise on arity. Returns -1 if unknown. Compile-time tuples longer than `CT_TUPLE_LOAD_SIZE_LIMIT` (15) are refused.
Example: `pmethodobject.c:PsycoCFunction_Call` (METH_O/NOARGS), `pclassobject.c`, `pmath.c`, `psyfunc.c:psyco_call_pyfunc`.

**P23 bool-select** (J)
Condition code → a virtual bool: a virtual int of type bool whose word is the condition. It is boxed with `PyBool_FromLong` only if it escapes; a jump on it uses the flags directly. Upstream had the same.
Example: `ni/Objects/pboolobject.h:PsycoBool_FromCondition` (`3702e3f`).

## F. Protocols and errors

**P24 exception-set** (M)
`PyErr_SetString(e, msg)` / `PyErr_Format` → `PycException_SetString(po, e, msg)` / `PycException_SetFormat`. It returns NULL (or `CC_ERROR`) exactly as the C does.
Example: `pabstract.c:type_error`, `ptupleobject.c:ptuple_item`.

**P25 notimplemented-protocol** (M)
`if (res != Py_NotImplemented) return res; Py_DECREF(res);` → `if (IS_IMPLEMENTED(res)) return res; vinfo_decref(res, po);`. This works on the residual path only if the call used `CfCommonCheckNotImplemented`, which maps a run-time NotImplemented to the unique `psyco_vi_NotImplemented()`.
Example: `pobject.c:try_rich_compare`, `pabstract.c:binary_op1`.

**P26 type-whitelist** (J)
Specialise only for a hand-picked set of types and make everything else residual.
Example: `pabstract.c:PsycoNumber_Power` (int/float only), `pfloatobject.c:pfloat_richcompare` (float/float only), `pobject.c:Psyco_SafelyDeleteVar` (types known to have no `__del__`).

## Core and backend mechanisms (not translations, but named for reference)

- **inline-cache-patch**: a promotion site is a self-patching `CMP imm64; JE` for the last value seen, in front of the C dispatcher. `ni/x64/idispatcher.h:ipromotion_finish`, `fix_fast_common_case`.
- **lazy-branch-proxy**: a branch to code not yet compiled goes through a stub that compiles it and then repoints the jump. `ni/x64/iencoding.c:psyco_call_code_builder`, `idispatcher.h:conditional_jump_to`/`change_cond_jump_target`. Used by coding pauses, respawn and promotion.
- **cc-virtual**: processor flags are held as virtual values in `po->ccregs` and materialised only when about to be clobbered. `ni/codegen.c:psyco_vinfo_condition`, `ni/x64/iencoding.c:psyco_compute_cc`.

## Rules any generator must respect

1. **Restart safety.** An opcode or meta function can be abandoned part-way through by a promotion and then re-run from the start. Nothing with a compile-time side effect may happen before the last point that can fail or promote ("use TOP() not POP()"; `ni/Python/pycompiler.c` main loop comment). [read] Emitted residual calls count as side effects: code emitted before a promotion has already run, so the restarted instruction runs it again. Hoist every promotion (`Psyco_NeedType` of all operands) before the first residual call. Example: `pbltinmodule.c:pbuiltin_min_max` (a user `__gt__` ran twice, `0feefaf`). Only values in the frame can be promoted, never temporaries (KNOWN_BUGS F24). [run]
2. **Error conventions are per call.** Metas return NULL / `CC_ERROR`. Residual calls get theirs from `CfPyErr*`. The generator needs the C function's error convention as data (most can be derived from CPython docs and return types). [read]
3. **Per-slot meta signatures and `Cf*` flags are data** (P09). Today they exist only implicitly in call sites. [read]
4. **Mixed values.** A translated function's arguments can be `vinfo_t*` or raw compile-time values (the `l` tag). The generator must decide this per argument and per slot. [read]
5. **Version drift is the problem being solved.** Several translations are Py2 semantics running on 3.7: `pabstract.c:binary_op1` depends on `Py_TPFLAGS_CHECKTYPES`, which doesn't exist in Py3, so mixed-type arithmetic falls to a TypeError [read + inferred]; `pbltinmodule.c` imports `__builtin__`. These are exactly the failures a generator from the 3.7 source would avoid. Use them as test cases.
