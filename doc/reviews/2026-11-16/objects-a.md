---
reviewed_at: e1fd808
written: 2026-11-16
status: point-in-time review record; not maintained. The code and doc/MAP.md are authoritative.
---
# Review: objects-a (pobject, pabstract, pstringobject, pboolobject, pdescrobject, pstructmember)

All paths are relative to repo root. CPython references are to 3.7 unless stated.

## 0. How Psyco_META / Psyco_DefineMeta dispatch works (as seen from these files)

Defined in `ni/Python/pycompiler.h` / `ni/Python/pycompiler.c`.

- **Registry** [read]: `Psyco_Meta_Dict` is a Python dict. Key = `NiCompatInt_FromLong((long)c_function)`, value = `NiCompatInt_FromLong((long)psyco_function)`. `Psyco_DefineMeta(c_fn, meta_fn)` inserts; `Psyco_Lookup(c_fn)` allocates a temporary PyLong key and does `PyDict_GetItem` on each call (no caching).
- **Key is the C function pointer, not the type** [read]. So a meta attaches to every type whose slot holds that pointer, including subclasses that inherit the slot unchanged (e.g. a `list` subclass gets `psyco_generic_mut_ob_size` for `sq_length`). Python classes get `slot_*` wrappers, which are never registered, so they always become residual calls. [inferred]
- **Dispatch** [read]: `Psyco_META1/2/3(po, c_fn, flags, "fmt", a1..)` casts each argument to `long` and calls `Psyco_Meta{1,2,3}x`. If a meta is found, it is called as `meta(po, a1, a2, a3)`. **`flags` and `fmt` are ignored** on that path. If none is found, the call becomes `psyco_generic_call(po, c_fn, flags, fmt, a1, ...)`.
- **The meta's C signature comes from the call site's format string, not from the C function** [read]:
  - a `'v'` argument reaches the meta as a `vinfo_t*`;
  - an `'l'` argument reaches it as a raw `long` or `PyObject*`.

  Examples:
  - `PsycoObject_SetAttr` calls `tp_setattro` with `"vlv"`, so a setattro meta receives `name` as a raw `PyObject*`.
  - `try_rich_compare` calls `tp_richcompare` with `"vvl"`, so `op` arrives as a raw int.
  - `pfuncobject.c:pfunc_descr_get` and `pdescrobject.c:pmember_get`/`pmethod_get` are written as `(po, PyObject* descr, vinfo_t* obj, PyTypeObject* type)`, which implies an `"lvl"` caller.

  **Every call site that dispatches on a given slot must use the same format**, or the meta gets mistyped arguments. Nothing checks this. A meta-compiler should derive one canonical "meta signature" per slot (for each argument: vinfo or compile-time constant) and generate both the metas and the call sites from it.
- **Return conventions for metas** [read, cf_flags.h comments]:
  - error: return `NULL`, or `CC_ERROR` for the `Psyco_flag_META*` variants;
  - "not implemented": return exactly `psyco_vi_NotImplemented()`, tested with `IS_NOTIMPLEMENTED` (pointer identity with `CompileTime_NewSk(&psyco_skNotImplemented)`);
  - `void`/int-zero-ok slots: return `VINFO_OK` or any non-NULL value. Callers do `!= NULL`.
- **Residual path with `CfPyErrNotImplemented`** (`CfCommonCheckNotImplemented`) [read]: `psyco_generic_call` arranges that a run-time NotImplemented comes back as the same `psyco_viNotImplemented`. That makes `IS_IMPLEMENTED` meaningful on both paths. Other flag combinations such as `CfCommonNewRefPyObject` do **not** detect NotImplemented, so a residual result may be a real NotImplemented object (see `binary_op1` same-type branch and `PsycoNumber_Power`).
- **Doubles cannot go through META** [inferred]: `Psyco_META*` casts every argument with `(long)(a)`. A `double` would be value-converted, and the `'d'` tag of `psyco_generic_call` is unreachable through META.
- **Pointer-as-long** [read]: keys, `CompileTime_New((long)ptr)` and META arguments assume `sizeof(long) == sizeof(void*)`. That holds on LP64 Linux and fails on LLP64 Windows.
- **Registration sites in this group** [read]:
  - `pobject.c:psy_object_init`: `PyObject_GenericGetAttr`.
  - `pdescrobject.c:psy_descrobject_init`: member and method descriptor `tp_descr_get`.
  - `pstringobject.c:psy_stringobject_init`: Py2 only, dead.
  - `pabstract.c` defines generic metas (`psyco_generic_immut_ob_size`, `psyco_generic_mut_ob_size`, `psyco_generic_subscript`, `psyco_generic_ass_subscript`); `ptupleobject.c`, `plistobject.c` and `pstringobject.c` register them.
- **Odd self-registration** [read]: `PyObject_GenericGetAttr` → `PsycoObject_GenericGetAttr`. That meta is itself only a residual call to `PyObject_GenericGetAttr`, so the registration has no effect apart from the dict lookup.

---

## ni/Objects/pobject.h

**Map row**: `| ni/Objects/pobject.h | core + translated | Include/object.h | ob_type/ob_size field descriptors, type promotion/verification helpers, NotImplemented tests |`

**Sections**

| Group | Class | Note |
|---|---|---|
| `OB_type`, `FIX_size`, `VAR_size`, `FIX_signed_size`, `VAR_signed_size`, `iOB_TYPE`, `iFIX_SIZE`, `iVAR_SIZE` | core (field descriptors) | Base of every object's `vinfo_array_t` layout: slot 0 = ob_type, then ob_size. `FIX_size` is `NONNEG_FIELD`. |
| `PyType_TypeCheck`, `PsycoIter_Check`, `PsycoSequence_Check` | translated | Type-object versions of `PyObject_TypeCheck`, `PyIter_Check`, `PySequence_Check`. `PsycoIter_Check` misses Py3's `_PyObject_NextNotImplemented` test. |
| `Psyco_NeedType`, `Psyco_FastType`, `Psyco_VerifyType`, `Psyco_KnownType`, `Psyco_AssertType` | core | promote-type / verify-type primitives. |
| `object_non_null` | core | Virtual objects, and run-time pointers already dereferenced (`array != NullArray`), are assumed non-NULL. |
| `DEF_KNOWN_RET_TYPE_1..4` | glue (codegen macro) | Residual call plus `Psyco_AssertType` on the result. |
| `IS_IMPLEMENTED`, `IS_NOTIMPLEMENTED` | core | Identity test against `psyco_skNotImplemented`. |
| `Psyco_SafelyDeleteVar` decl | core | Used by `dispatcher.c`. |

## ni/Objects/pobject.c

**Map row**: `| ni/Objects/pobject.c | translated + core | Objects/object.c | PyObject_IsTrue/SetAttr/RichCompare metas; type promotion; dispatcher safe-delete helper |`

**Sections**

| Function | Class | Note |
|---|---|---|
| `Psyco_NeedType` | core | Compile-time object: reads `ob_type` directly. Otherwise `psyco_get_const(OB_type)` + `psyco_pyobj_atcompiletime`, which promotes and may return NULL with a pending promotion exception. |
| `Psyco_KnownType` | core | Never promotes. |
| `PsycoObject_IsTrue` | translated | Same order as CPython: None, `nb_bool`, `mp_length`, `sq_length`, then 1. Missing the Py_True/Py_False fast tests (XXX tony). Lengths are **not normalised** to 0/1. Callers (`pycompiler.c` POP_JUMP / `integer_NON_NULL`) only test non-zero, but `PsycoObject_RichCompareBool` returns this raw value. |
| `PsycoObject_Repr` | residual + assert type | `Psyco_AssertType(NiCompatStr_Type)`, i.e. unicode on Py3. |
| `PsycoObject_GetAttr`, `PsycoObject_GenericGetAttr` | residual | Upstream Psyco specialised these. Here, because they are residual, the `tp_descr_get` metas are unreachable. |
| `PsycoObject_SetAttr` | translated | Handles only compile-time names. Interns the name and META-dispatches `tp_setattro` (`"vlv"`/`"vll"`), else `tp_setattr` with `char*`. The `#ifdef IS_PY3K` `PyUnicode_Check` branch is dead because `NiCompatStr_Check` is already `PyUnicode_Check`. Error text "attribute name must be string" differs from 3.7 ("attribute name must be string, not '%.200s'"). |
| `swapped_op`, `try_rich_compare`, `PsycoObject_RichCompare`, `PsycoObject_RichCompareBool` | translated | Faithful to 3.7 `do_richcompare` ordering (subclass reflected first). If both sides give NotImplemented it falls back to a residual `PyObject_RichCompare`, which re-runs the slots at run time. [inferred] When a residual slot returned a run-time NotImplemented, user `__eq__` etc. may therefore run twice (observable side effects). |
| `collect_undeletable_vars`, `Psyco_SafelyDeleteVar` | core | Used when a local is deleted or overwritten in the dispatcher. Keeps run-time refs whose type might have a `__del__`. A whitelist of "safe" types is hard-coded (str/unicode, bool, float, int, None, range). |
| `psy_object_init` | glue | One `Psyco_DefineMeta`. |

**Patterns observed**
- promote-type: `pobject.c:PsycoObject_IsTrue` and `try_rich_compare` use `Psyco_NeedType`, where CPython uses `Py_TYPE(v)`. Mechanical.
- meta-dispatch: `PsycoObject_IsTrue` uses `Psyco_META1(po, tp->tp_as_number->nb_bool, CfCommonInquiry, "v", vi)`, where CPython calls `(*nb_bool)(v)`. Mechanical, given a slot→Cf-flags table (one Cf combination per slot signature).
- NEW: `notimplemented-protocol`: `try_rich_compare` / `IS_IMPLEMENTED(res)` / `vinfo_decref(res)` / `psyco_vi_NotImplemented()`, where CPython uses `if (res != Py_NotImplemented) return res; Py_DECREF(res);`. Mechanical, provided the call uses `CfCommonCheckNotImplemented`.
- NEW: `assert-result-type`: `PsycoObject_Repr`, `DEF_KNOWN_RET_TYPE_*`. This is a residual call followed by `Psyco_AssertType` on the result. Judgement: the knowledge that the result is a str/tuple is not in the callee's signature.
- exception-set: `PycException_SetString` in `PsycoObject_SetAttr`. Mechanical.
- ct-fold (partial): `PsycoObject_SetAttr` analyses only compile-time `vattrname` and goes residual otherwise. Judgement.
- core (no pattern): `Psyco_SafelyDeleteVar` (safe-type whitelist is judgement).

**Leftovers**
- `PsycoObject_IsTrue` has two `XXX tony` notes (Py_True/Py_False fast path; 0/1 normalisation).
- `PsycoObject_Repr` "XXX implement me"; `PsycoObject_GetAttr` "XXX tony: Add unicode optimization"; `PsycoObject_GenericGetAttr` "XXX tony: Optimize this but allow changes to type dict".
- `PsycoObject_SetAttr`: the comment about "Unicode to string conversion" is Py2-era and its branch is dead on Py3.
- `PsycoObject_RichCompare`: odd indentation of `vinfo_decref(res, po);` (cosmetic).
- `collect_undeletable_vars`: `vi->array->count` loop uses `int i` (fine).

## ni/Objects/pabstract.h

**Map row**: `| ni/Objects/pabstract.h | translated | Include/abstract.h | declarations of compile-time abstract protocol |`

**Leftovers**
- `PsycoNumber_Divide` and `PsycoNumber_InPlaceDivide` are declared but have no definition anywhere (Py2 `nb_divide`).
- The comment on `psyco_generic_*` refers to "sequences in Python 2.3".
- The comment on `PsycoIter_Next` matters: the meta-level StopIteration is *not* cleared, unlike `PyIter_Next`.

## ni/Objects/pabstract.c

**Map row**: `| ni/Objects/pabstract.c | translated | Objects/abstract.c | metas for PyObject_Call/GetItem/SetItem/Size, PySequence_*, PyNumber_* binops/in-place, GetIter/IterNext |`

**Sections**

| Function(s) | Class | Note |
|---|---|---|
| `type_error` | translated | `PycException_SetString`. Messages lack the 3.7 `%.200s` type names. |
| `PsycoObject_Call` | translated | META on `tp_call` with `"vvv"`. No `Py_EnterRecursiveCall` or `_Py_CheckFunctionResult`; acceptable at compile time. The "kw may not be NULL" comment means a vinfo of 0, not a C NULL. |
| `PsycoEval_CallObjectWithKeywords` | translated | verify-type on args/kw. **Bug [inferred]**: if `args` is not a tuple and `kw == NULL`, `use_proxy` passes a C-NULL `kw` as `'v'`, and `psyco_generic_call` dereferences it (`compute_vinfo(NULL)`). |
| `PsycoObject_GetItem`, `PsycoObject_SetItem` | translated | mp first, then sequence. A PyLong key goes through `PsycoLong_AsLong`, which raises OverflowError; 3.7 uses `PyNumber_AsSsize_t(key, PyExc_IndexError)`. Missing the 3.7 PEP 560 `__class_getitem__` path. Returns `false` from `vinfo_t*` functions (it works because false is 0, but the type is wrong). Message "unsubscriptable object" is Py2 wording. |
| `PsycoObject_Size` | translated | Same slot order as 3.7. |
| `psyco_generic_immut_ob_size`, `psyco_generic_mut_ob_size` | translated (field-access) | `sq_length` meta = read `FIX_size` / `VAR_size`. |
| `PsycoSequence_GetItem`, `PsycoSequence_SetItem` | translated | Negative index: `integer_cmp_i(i,0,Py_LT)` → `runtime_condition_f`, then add `sq_length`; otherwise `assert_nonneg(i)`. Faithful to 3.7. |
| `PsycoSequence_GetSlice`, `PsycoSequence_SetSlice`, `PsycoSequence_Tuple` | residual | XXX implement me. |
| `PsycoSequence_Contains` | translated + residual | META on `sq_contains`, else residual `_PySequence_IterSearch`. Returns `false` as a `vinfo_t*`. |
| `PsycoNumber_Positive`, `PsycoNumber_Negative`, `PsycoNumber_Invert`, `PsycoNumber_Absolute` | translated | Straight slot META plus TypeError. Mechanical. |
| `NEW_STYLE_NUMBER`, `NB_SLOT`, `NB_BINOP`, `NB_TERNOP` | translated (macros) | **`Py_TPFLAGS_CHECKTYPES` does not exist on Py3** (confirmed absent from 3.8 headers), so `NEW_STYLE_NUMBER(x)` is always 0. |
| `binary_op1` | translated (Py2 semantics) | **Major bug [read + inferred]**: with `NEW_STYLE_NUMBER` = 0, `slotv` and `slotw` are never set, so only the Py2 "coerce" tail runs. That tail calls a slot only when `vtp == wtp`, and does so with `CfCommonNewRefPyObject`, so a run-time NotImplemented is not detected. Mixed-type operations (int+float, float*int, user subclass ⊕ base) yield NotImplemented and then `binop_type_error`: a compile-time TypeError where CPython would succeed. The fix is to port 3.7 `binary_op1` verbatim: drop `NEW_STYLE_NUMBER` and the coerce tail. Unless a caller avoids `binary_op1`, this is a correctness hole for every `PsycoNumber_*` binop. |
| `binop_type_error`, `binary_op`, `BINARY_FUNC` instances | translated | Error text uses `'%s'`; 3.7 uses `'%.100s'`. |
| `PsycoNumber_Add`, `PsycoNumber_Multiply`, `psequence_repeat` | translated | Match 3.7 (`sq_concat` / `sq_repeat` fallback). `psequence_repeat` uses `Psyco_FastType(vn)`, which is safe because `binary_op1` already promoted both types. The comment block about 2.2/2.3 semantics is Py2 history. |
| `PsycoNumber_Remainder` | translated | |
| `PsycoNumber_Power` | translated (judgement) | Only long/float ** long/float. Picks `float_pow` if either side is float, else `long_pow`. Otherwise residual `PyNumber_Power`. |
| `binary_iop1`, `binary_iop`, `INPLACE_BINOP` instances, `PsycoNumber_InPlaceAdd`, `PsycoNumber_InPlaceMultiply` | translated | Faithful to 3.7, including the `mv != NULL` / `else if (mw)` quirk. Inherit the `binary_op1` bug. |
| `PsycoNumber_InPlacePower` | residual | XXX. |
| `PsycoObject_GetIter` | translated + virtual | No `tp_iter` → `PsycoSeqIter_New` (virtual seq iterator). Message "iteration over non-sequence" is Py2; 3.7 says "'%.200s' object is not iterable". |
| `PsycoIter_Next` | translated | `CfCommonIterNext`. Does not clear StopIteration (by design, see header). |
| `psyco_generic_subscript`, `psyco_generic_ass_subscript` | translated | Generic `mp_subscript` / `mp_ass_subscript` meta for list/tuple(/str). A PyLong key is routed to `PsycoSequence_*Item`; anything else (slices) becomes a residual call of the type's own `mp_*` slot. The comment "mapping item assignment ... Python >= 2.3" is stale. |

**Patterns observed**
- promote-type then meta-dispatch: every `PsycoNumber_*` and `PsycoObject_*` function. CPython `m = Py_TYPE(o)->tp_as_number; if (m && m->nb_x) return m->nb_x(o);` becomes `tp = Psyco_NeedType(po,o); if(!tp) return NULL; m = tp->tp_as_number; if (m && m->nb_x) return Psyco_META1(po, m->nb_x, Cf…, "v", o);`. Mechanical. The only non-mechanical inputs are the Cf flags per slot type and the vinfo/long format per argument.
- verify-type: `PsycoEval_CallObjectWithKeywords` uses `switch (Psyco_VerifyType(...)) {case true/false/default}`, where CPython uses `if (!PyTuple_Check(args))`. Mechanical.
- runtime-branch: `PsycoSequence_GetItem` uses `cc = integer_cmp_i(po,i,0,Py_LT); if (runtime_condition_f(po,cc)) …`, where CPython uses `if (i < 0)`. Mechanical, but the choice of `_f` (expected false) vs `_t` is a likelihood hint (judgement, or a default).
- NEW: `assert-range`: `assert_nonneg(i)` on the else-branch of a sign test records a fact for later folding. Mechanical (derivable from the branch condition).
- field-access: `psyco_generic_immut_ob_size` uses `psyco_get_field(po, vi, FIX_size)` for `Py_SIZE(o)`. Choosing FIX vs VAR (immutable vs mutable) is judgement (type knowledge).
- notimplemented-protocol: `binary_op1`, `binary_iop1`, `PsycoNumber_Power`. Mechanical.
- NEW: `type-whitelist`: `PsycoNumber_Power` specialises only a hand-chosen set of types, else residual. Judgement.
- residual-call: `PsycoSequence_GetSlice`, `PsycoSequence_SetSlice`, `PsycoSequence_Tuple`, `PsycoNumber_InPlacePower`, `use_proxy`. Mechanical (the default when no translation exists).
- virtual-object: `PsycoObject_GetIter` → `PsycoSeqIter_New`. Judgement.
- exception-set: `type_error`, `binop_type_error`. Mechanical.
- refcount: the `release_me` idiom in `PsycoSequence_GetItem`/`SetItem` (a temporary vinfo is xdecref'd after the call). Mechanical.

**Leftovers**
- `NEW_STYLE_NUMBER` / `Py_TPFLAGS_CHECKTYPES` and the "inline a bit of PyNumber_CoerceEx" block in `binary_op1` are pure Py2 and, on Py3, harmful.
- Comment "XXX tp_flags test -- not done in Python either" above FloorDivide/TrueDivide is Py2.
- Several `XXX implement me` / `XXX call mp_subscript with sliceobj_from_intint` notes.
- Py2 error messages throughout.
- `(long)PyExc_IndexError` / `(long)PyExc_OverflowError` pass pointers as long (LP64 OK).

## ni/Objects/pstringobject.h / ni/Objects/pstringobject.c

**Map rows**
- `| ni/Objects/pstringobject.h | translated + core | Py2 Include/stringobject.h (≈ Py3 Include/bytesobject.h) | virtual-string layouts and PsycoString_* accessors; empty on Py3 |`
- `| ni/Objects/pstringobject.c | translated + core | Py2 Objects/stringobject.c | Py2 str metas with virtual char/slice/bufstr; compiled out on Py3 |`

**Py2 vs Py3 status [read]**
- Both files wrap their whole body in `#ifdef IS_PY3K #else … #endif`, and `compat2to3.h` defines `IS_PY3K` unconditionally, so **on Py3 the files are empty**.
- `psy_stringobject_init` is not listed in `initialize.h` (neither the ALL_STATIC include list nor `initialize_all_files`). With ALL_STATIC=0, `setup.py` still globs and compiles the (empty) .c file.
- **No Py3 `str` (unicode) or `bytes` code exists in this file.** All of it is Py2 `str`.
- The only Py3-facing references in this group are the `NiCompatStr_Type` asserts in `pobject.c`, `PyBytes_Type` in `pstructmember.c`, and `pbltinmodule.c` including the empty header.
- **Port target**: the code relies on `PyStringObject` fields `ob_size`, `ob_shash` and `ob_sval[]`, and on `ob_sstate`. Py3 `PyBytesObject` has the same `ob_size`/`ob_shash`/`ob_sval` layout but no `ob_sstate`. So char, strslice, bufstr and richmemcmp port almost mechanically to **bytes** (PyString→PyBytes; drop the `SSTATE` block).
- They do **not** port to Py3 `str`: PEP 393 compact unicode has no `ob_sval`; length is `PyASCIIObject.length`; data width is 1/2/4 bytes by `kind`; there are hash/state bitfields. Unicode needs new field descriptors and new virtual layouts.
- Py3 has no `sq_slice` slot. `psy_stringobject_init` registers `m->sq_slice`, and Py3 slicing goes through `mp_subscript` with a slice object, so `pstring_slice` would need wiring from `mp_subscript`. `nb_remainder` (`bytes % x`) exists in Py3.5+.

**Sections**

| Group | Class | Note |
|---|---|---|
| `STR_sval`, `STR_sval2`, `STR_sval4`, `VIRTUALSTR_FIRST`, `CHARACTER_*`, `STRSLICE_*`, `CATSTR_*`, `BUFSTR_*` (h) | core (layouts) | Virtual-string vinfo_array index layouts that extend `FIX_size`. |
| `PsycoString_Check`, `PsycoUnicode_Check`, `PsycoString_AS_STRING`, `PsycoString_GET_SIZE` (h) | translated | `Py_USING_UNICODE` is Py2. |
| `cimpl_character`, `compute_char`, `direct_compute_char`, `PsycoCharacter_NEW`, `PsycoCharacter_New`, `first_character`, `PsycoCharacter_Ord` | core (virtual-object) | Virtual one-character string. |
| `compute_strslice`, `direct_compute_strslice`, `PsycoStrSlice_NEW` | core (virtual-object) | A slice that keeps a ref to its source. |
| `cimpl_concatenate`, `compute_catstr`, `PsycoCatStr_NEW`, `pstring_as_catlist`, catstr `pstring_concat` | obsolete | `USE_CATSTR 0`; contains `#error direct_compute_catstr is missing`. |
| `pstring_item` | translated + virtual | Bounds check is unsigned `>=` (`COMPARE_UNSIGNED`), returning a virtual char. |
| `pstring_slice` | translated + judgement | Clamps with `CHEAT_MAXINT`; handles slice of slice and `a[0:len]` is `a`. Comment: deliberately does not special-case `i==j`. |
| `psyco_richmemcmp`, `pstring_richcompare` | translated + judgement | Length-first compare; inlines 1/2/4-byte compares, else a residual `memcmp`. **Bug-ish [read]**: `memcmp` is called with `CfReturnTypePtr` although it returns `int`; upper 32 bits on x86-64 are undefined, then `integer_cmp_i(...,0,op)`. Py2 only. |
| `pstring_mod` | residual | `PyString_Format`. |
| `PsycoBufStr_NEW`, `stringlike_t`, `PsycoBufStr_Type`, `bufstr_dealloc`, `assert_bufstr_invariants`, `cimpl_cb_new`, `cimpl_cb_grow`, `cimpl_cb_normalize`, `compute_bufstr`, `direct_compute_bufstr`, bufstr `pstring_concat` | core (virtual-object + runtime helpers) | Over-allocated concatenation buffer. Repurposes `ob_shash` as the "used" count, then patches `ob_type` to `PyString_Type` on materialisation. Heavy judgement: overallocation constants `BUFSTR_OVERALLOCATION_MASK` and `BUFSTR_ACCEPT_OVERALLOCATED`. |
| `pstr_memory_source` | core | Gets the backing memory of a slice or bufstr without materialising it. |
| `psy_stringobject_init` | glue | Never called. |

**Patterns observed (Py2 code, but the shapes are reusable)**
- virtual-object: `PsycoStrSlice_NEW`/`compute_strslice`/`direct_compute_strslice`, a triple of constructor, emit-materialise and direct-materialise. Judgement (choosing what to virtualise).
- ct-fold: bufstr `pstring_concat` calls `PyString_Concat` at compile time when both operands are compile-time. Mechanical in principle (needs purity knowledge).
- NEW: `identity-shortcut`: `a + ''` is `a`, `a[0:len]` is `a`, and `vinfo_known_equal(v,w)` in richcompare. Judgement (requires exact-type checks to avoid returning subclasses).
- NEW: `virtual-peephole`: slice of a slice in `pstring_slice`, and a bufstr growing in place (`cimpl_cb_grow`). Judgement.
- runtime-branch with `COMPARE_UNSIGNED`/`CHEAT_MAXINT` in `pstring_item`/`pstring_slice`, folding the two-sided check `0 <= i < n` into one unsigned compare. Judgement (an optimisation not present in the C).
- field-access: `psyco_get_field_array(po, a, STR_sval, i)`. Mechanical.

**Leftovers**
- Entire file is Py2.
- `int` lengths in `cimpl_*` (`int a_size, b_size, size`); `int i, slen, count` in `cimpl_concatenate`.
- "XXX catch integer overflows here" in `cimpl_cb_grow`.
- "XXX try using PyObject_REALLOC".
- "XXX store totallength in result?".
- `#error` under `USE_CATSTR`.
- `PyObject_HEAD_INIT(NULL) 0, /*ob_size*/` is Py2 type syntax (Py3 needs `PyVarObject_HEAD_INIT`).
- `SSTATE_NOT_INTERNED` / `INTERN_STRINGS` branches.
- `INIT_SVIRTUAL` for catstr is missing its `direct_compute` argument.

## ni/Objects/pboolobject.h / ni/Objects/pboolobject.c

**Map rows**
- `| ni/Objects/pboolobject.h | translated | Objects/boolobject.c PyBool_FromLong | condition code → bool object via residual call |`
- `| ni/Objects/pboolobject.c | glue | none | empty init stub |`

**Sections**
- `PsycoBool_FromCondition`: `psyco_vinfo_condition` followed by a residual `PyBool_FromLong` (`CfCommonNewRefPyObject`, not `CfPure`).
- `psy_boolobject_init` is empty and is not called from `initialize.h`.

**Patterns**: residual-call. A better translation would be `ct-fold` when cc is compile-time, plus a select between `Py_True`/`Py_False` (NEW: `bool-select`), or a virtual bool. Judgement. XXX tony notes this.

**Leftovers**: closing `#endif /* _PSY_INTOBJECT_H */` comment mismatch (copied from Py2 intobject).

## ni/Objects/pdescrobject.h / ni/Objects/pdescrobject.c

**Map rows**
- `| ni/Objects/pdescrobject.h | glue | Include/descrobject.h | empty placeholder |`
- `| ni/Objects/pdescrobject.c | translated + glue | Objects/descrobject.c member_get, method_get | descriptor __get__ metas |`

**Sections**

| Function | Class | Note |
|---|---|---|
| `pmember_get` | translated | → `PsycoMember_GetOne(po, obj, descr->d_member)`. Skips `descr_check` (XXX: assumes obj is an instance of type). |
| `pmethod_get` | translated + virtual | → virtual `PsycoCFunction_New`. Skips 3.7's `obj == NULL → return descr` case and `descr_check`. [inferred] This would be wrong for class-level access if it were ever reached. |
| `psy_descrobject_init` | glue | Obtains the descriptor types by creating dummy descriptors. That is unnecessary on Py3: `PyMemberDescr_Type` and `PyMethodDescr_Type` are exported in `descrobject.h`. |

**Key finding [read]**: no code in the repo calls `Psyco_META*` on any `tp_descr_get`. The only other registration, `pfuncobject.c:pfunc_descr_get`, is equally unreached, because `PsycoObject_GenericGetAttr` and `PsycoObject_GetAttr` are pure residual calls. These metas are dead until a translated `PyObject_GenericGetAttr` (`_PyObject_GenericGetAttrWithDict`) exists. The meta signature `(po, PyObject* descr, vinfo_t* obj, PyTypeObject* type)` is the convention that translation must use (`"lvl"`).

**Patterns**: meta-dispatch target (mixed l/v signature); virtual-object (`PsycoCFunction_New`).

## ni/Objects/pstructmember.h / ni/Objects/pstructmember.c

**Map rows**
- `| ni/Objects/pstructmember.h | translated | Include/structmember.h | PsycoMember_GetOne decl |`
- `| ni/Objects/pstructmember.c | translated | Python/structmember.c PyMember_GetOne | read C struct members via field descriptors |`

**Sections**: one function, `PsycoMember_GetOne`, a `switch (l->type)`:
- integer types: `psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(ctype,0)), l->offset)`, then `PsycoLong_FROM_LONG` or `PsycoLong_FromUnsignedLong`;
- float/double: `PsycoFloat_FromFloat` / `PsycoFloat_FROM_DOUBLE` (virtual float);
- `T_STRING`: non-null test, then residual `NiCompatStr_FromString`;
- `T_OBJECT`/`T_OBJECT_EX`: non-null test, then `need_reference`, else None or AttributeError;
- `default`: residual `PyMember_GetOne`.

**Patterns observed**
- field-access: CPython `*(short*)addr` → `psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(signed short,0)), l->offset)`. Mechanical: a meta-compiler can map each `T_*` C type to a defield.
- runtime-branch: `integer_non_null` + `runtime_condition_t` for `if (v == NULL)`. Mechanical.
- assert-result-type: `T_STRING`, `T_STRING_INPLACE`.
- virtual-object: float results.
- refcount: `need_reference` mirrors `Py_INCREF(v)` on a borrowed field load. Mechanical.

**Leftovers / bugs**
- **[read] `T_INT`/`T_UINT` share the `long` path** ("XXX assumes sizeof(int) == sizeof(long)"). On x86-64 this reads 8 bytes for a 4-byte field: wrong value, and possibly a read past the end of the struct. `T_UINT` additionally goes through `PsycoLong_FromUnsignedLong` because `T_UINT_READS_AS_SIGNED` is 0 for Py ≥ 2.5. Fix: give `T_INT` → `DEF_ARRAY(int,0)` and `T_UINT` → `UNSIGNED_ARRAY(unsigned int,0)` their own cases.
- **[read] `T_STRING_INPLACE`** builds `PyBytes_FromString` and asserts `PyBytes_Type`; CPython 3.7 returns `PyUnicode_FromString`. A semantic difference with a wrong type assertion.
- Missing types fall back to residual (fine): `T_CHAR`, `T_BOOL`, `T_LONGLONG`, `T_ULONGLONG`, `T_PYSSIZET`, `T_NONE`.
- `READ_RESTRICTED` test: restricted mode is gone in Py3, and 3.7's `PyMember_GetOne` no longer checks it. Harmless leftover.
- Comments "read a long, turn it into a float" and "read two longs ... double" are 32-bit-era wording.

---

## Glossary terms
- `Psyco_Meta_Dict`, `Psyco_DefineMeta`, `Psyco_Lookup`, `Psyco_META1/2/3`, `Psyco_Meta1x..3x`, `Psyco_flag_META*`: `ni/Python/pycompiler.h`/`.c` (see §0).
- `IS_IMPLEMENTED`, `IS_NOTIMPLEMENTED`: `ni/Objects/pobject.h`. Test for the unique `psyco_skNotImplemented` compile-time source.
- `VINFO_OK`: `ni/cf_flags.h`. `(vinfo_t*)1`, the success value for `CfNoReturnValue` calls.
- `CfCommonCheckNotImplemented`, `CfCommonIntZeroOk`, `CfCommonInquiry`, `CfCommonPySSizeTResult`, `CfCommonIntBoolOrError`, `CfCommonIterNext`, `CfCommonNewRefPyObject(NoError)`: `ni/cf_flags.h`.
- `OB_type`, `FIX_size`, `VAR_size`, `iOB_TYPE`, `iFIX_SIZE`: `ni/Objects/pobject.h`.
- `DEF_FIELD`, `NONNEG_FIELD`, `DEF_ARRAY`, `UNSIGNED_ARRAY`, `FMUT`, `FIELD_INDEX`, `FIELDS_TOTAL`, `FIELD_RESERVED_INDEX` (index used by `psyco_get_field_offset` for un-cached loads), `psyco_get_field_offset`: `ni/vcompiler.h`.
- `psyco_get_const`: loads a field and marks it constant/promotable, used for `ob_type` and `ob_size` (ni/vcompiler.h) [inferred from use].
- `psyco_pyobj_atcompiletime`: `ni/Python/pycompiler.h`. Promote a vinfo to a compile-time PyObject*, raising a promotion pseudo-exception if it is run-time.
- `runtime_condition_t` / `runtime_condition_f`: turn a `condition_code_t` into a C bool, forking on run-time conditions; the suffix is the expected-likely outcome [inferred].
- `assert_nonneg`: record that a run-time integer is ≥ 0 [inferred from use].
- `need_reference`: ensure the vinfo owns a real reference, i.e. a Py_INCREF on a borrowed load [inferred].
- `COMPARE_UNSIGNED`, `CHEAT_MAXINT`: flags OR'd into `integer_cmp` ops (pstringobject.c use) [inferred].
- `INIT_SVIRTUAL(sv, compute, direct_compute, pyobj_mask, nw_normal, nw_funcall)`: initialises a `source_virtual_t` [read use in pstringobject.c].
- `PsycoIndex_Check(tp)`: `ni/Python/pyver.h`. `tp_as_number->nb_index != NULL`.
- `T_UINT_READS_AS_SIGNED`: `ni/Python/pyver.h` (0 for Py ≥ 2.5).
- `NiCompatStr_*` / `NiCompatInt_*`: `ni/compat2to3.h`. On Py3, `NiCompatStr` = PyUnicode and `NiCompatInt` = PyLong.

## Questions
1. Does any caller avoid `binary_op1` for int/float mixes (e.g. `pycompiler.c` BINARY_* checking types first), or is mixed arithmetic currently a compile-time TypeError? This looks like the most important functional bug in the group.
2. Is the double invocation of user `__eq__` in `PsycoObject_RichCompare` (NotImplemented from a residual slot followed by the residual `PyObject_RichCompare`) acceptable, or should the fallback be a translated tail of 3.7 `do_richcompare` (identity for EQ/NE, else TypeError)?
3. Should `Psyco_Meta_Dict` be replaced by a C hash or pointer table? Each lookup allocates a PyLong at compile time.
4. Is a slot's meta signature (`l` vs `v` per argument) documented anywhere, or does it exist only implicitly in call sites? A meta-compiler needs it as data.
5. Is the plan to port `pstringobject.c` to `bytes` and write a fresh unicode layer, or to drop virtual strings?
6. `PsycoObject_SetAttr` interns compile-time names with `NiCompatStr_InternInPlace` on a constant held by the vinfo. Is it safe to replace a compile-time constant's object (refcount and `SkFlagFixed` interplay)?
