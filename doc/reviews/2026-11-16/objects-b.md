---
reviewed_at: e1fd808
written: 2026-11-16
status: point-in-time review record; not maintained. The code and doc/MAP.md are authoritative.
---
# Review: objects-b (tuple, list, dict, iter, func, method, class, long, float, math, builtins)

## Map rows

| path | layer | CPython counterpart | role (one line) |
|---|---|---|---|
| ni/Objects/ptupleobject.h | translated + core | Include/tupleobject.h | TUPLE_ob_item descriptor, PsycoTuple_GET/SET_ITEM, virtual tuple API |
| ni/Objects/ptupleobject.c | core + translated | Objects/tupleobject.c (PyTuple_New, tupleitem, tupleconcat) | virtual tuples, PsycoTuple_Load/Concat, sq_item meta |
| ni/Objects/plistobject.h | translated + core | Include/listobject.h | LIST_* descriptors, VLIST_LENGTH_MAX, list API |
| ni/Objects/plistobject.c | core + translated | Objects/listobject.c (list_item, list_ass_item, list_concat, list_iter) | virtual short lists, item get/set/concat metas, tp_iter redirect |
| ni/Objects/pdictobject.h | translated | Include/dictobject.h | dict creation/setitem API |
| ni/Objects/pdictobject.c | translated | Objects/dictobject.c (PyDict_New, dict_length, PyDict_Copy, PyDict_SetItem) | residual wrappers + mp_length meta |
| ni/Objects/piterobject.h | translated + core | Objects/iterobject.c (seqiterobject) | seqiterobject re-declaration, descriptors, virtual ctor |
| ni/Objects/piterobject.c | core + translated | Objects/iterobject.c (PySeqIter_New, iter_iternext) | virtual seq-iterators, tp_iter/tp_iternext metas |
| ni/Objects/pfuncobject.h | translated + core | Include/funcobject.h | FUNC_* descriptors (Py3.7 layout subset), call API |
| ni/Objects/pfuncobject.c | core + translated | Objects/funcobject.c (PyFunction_NewWithQualName, function_call, func_descr_get) | virtual functions, inlined Python calls |
| ni/Objects/pmethodobject.h | translated + core | Include/methodobject.h | CFUNC_* descriptors, PsycoCFunction_New (inline) |
| ni/Objects/pmethodobject.c | core + translated | Objects/methodobject.c (PyCFunction_NewEx, PyCFunction_Call) | virtual builtin-method objects, tp_call meta by ml_flags |
| ni/Objects/pclassobject.h | translated + core | Include/classobject.h | METHOD_* descriptors, PsycoMethod_New |
| ni/Objects/pclassobject.c | core + translated | Objects/classobject.c (PyMethod_New, method_call) | virtual bound methods, tp_call meta |
| ni/Objects/plongobject.h | translated | Include/longobject.h | PyLong conversion API |
| ni/Objects/plongobject.c | translated | Objects/longobject.c (long_* number slots) | residual metas with known result type |
| ni/Objects/pfloatobject.h | translated + core | Include/floatobject.h | FLOAT_ob_fval, PsycoFloat_FROM_DOUBLE (inline virtual ctor) |
| ni/Objects/pfloatobject.c | translated + core | Objects/floatobject.c (float_add.., float_richcompare, CONVERT_TO_DOUBLE) | virtual floats, arithmetic via pure C helpers |
| ni/Modules/pmath.c | translated | Modules/mathmodule.c (FUNC1/FUNC2, math_1/math_2) | math metas returning virtual floats |
| ni/Python/pbltinmodule.h | glue | Python/bltinmodule.c | empty header |
| ni/Python/pbltinmodule.c | translated | Python/bltinmodule.c (builtin_id/len/abs/divmod; Py2 apply/range) | builtin metas (currently never registered) |

## Virtual-object summary (cross-file)

Common mechanics [read, vcompiler.h `INIT_SVIRTUAL`/`INIT_SVIRTUAL_NOCALL`]: `source_virtual_t` = {compute_fn, direct_compute_fn, pyobject_mask, nested_weight[NORMAL, FUNCALL]}. `INIT_SVIRTUAL_NOCALL(sv, fn, w)` = no direct_compute, mask 0, FUNCALL weight `NW_FORCE` (=NESTED_WEIGHT_END, 15) → `SVIRTUAL_MUTABLE`: always materialised when passed across a Psyco function call. Every compute_fn follows the same shape: read sub-vinfos → residual-call the CPython constructor → optional `psyco_put_field` of extra fields → `vinfo_move(po, v, newobj)`. Slot 0 is always `iOB_TYPE` = CompileTime type pointer (so `Psyco_NeedType` on a virtual object is free).

| type / sv | array layout (index: content) | compute | direct_compute | pyobject_mask | weights N/F | materialisation triggers |
|---|---|---|---|---|---|---|
| tuple `psyco_computed_tuple` | 0 `iOB_TYPE`=&PyTuple_Type CT; 1 `iFIX_SIZE`=CT count; 2.. `iTUPLE_OB_ITEM+i`=items | `compute_tuple`: if all items CT → builds a real tuple at compile time (CT result); else residual `PyTuple_New(len)` + `psyco_put_nth_field(FMUT(FPYREF(TUPLE_ob_item)))` per item | `direct_compute_tuple` | `-1 << iTUPLE_OB_ITEM` (all items, sign-extended) | 4 / 4 (`NW_TUPLES_*`) | generic escape (any residual call taking it as `v`, storage); nesting weight overflow. Survives across calls (immutable) |
| list `psyco_computed_vlist` | 0 `iOB_TYPE`=&PyList_Type; 1 `iVAR_SIZE`=CT size (SkFlagFixed); 2 `LIST_ob_item` (unset while virtual); 3.. `VLIST_ITEMS+i`=items (`VLIST_ITEMS == LIST_TOTAL == 3`) | `compute_vlist`: residual `PyList_New(len)`, load ob_item, `psyco_put_nth_field(FPYREF(LIST_itemsarray))`, then `vinfo_array_shrink(LIST_TOTAL)` and `psyco_forget_field(VAR_size)` (size becomes mutable) | none | 0 | 5 / force | length > `VLIST_LENGTH_MAX` (3) at creation (`list_maybe_compute`, called from `PsycoList_New` incl. concat result); any Psyco call boundary (NOCALL); non-CT index in item/ass_item; recursive store (`vinfo_would_be_recursive`); escape |
| seq-iterator `psyco_computed_seqiter` | 0 `iOB_TYPE`=&PySeqIter_Type; 1 `iSEQITER_IT_INDEX` = run-time copy of 0 (deliberately not CT); 2 `iSEQITER_IT_SEQ` = seq (ref consumed) | `compute_seqiter`: residual `PySeqIter_New(seq)`, store it_index unless known 0, `psyco_forget_field(SEQITER_it_index)` | none | 0 | 1 / force | call boundary; escape. Created by `PsycoSeqIter_New`, which is also the meta for `PyList_Type.tp_iter` |
| function `psyco_computed_function` | 0 type=&PyFunction_Type; `iFUNC_CODE`, `iFUNC_GLOBALS`, `iFUNC_DEFAULTS`, `iFUNC_KWDEFAULTS`, `iFUNC_CLOSURE`, `iFUNC_ANNOTATIONS`, `iFUNC_QUALNAME` (index order = DEF_FIELD chain order, not struct order; defaults etc. init to `psyco_vi_Zero()`, overwritten by MAKE_FUNCTION in pycompiler.c) | `compute_function`: residual `PyFunction_NewWithQualName(code, globals, qualname)` then `SET_FIELD` closure/annotations/kwdefaults/defaults | none | 0 | 1 / force | call boundary; escape; but `pfunction_call` on a virtual function reads components directly and never materialises |
| C method `psyco_computed_cfunction` | 0 type=&PyCFunction_Type; 1 `iCFUNC_M_ML`=CT PyMethodDef*; 2 `iCFUNC_M_SELF`=self | `compute_cfunction`: residual `PyCFunction_NewEx(ml, self, NULL)` with CfPure | `direct_compute_cfunction` | `1 << iCFUNC_M_SELF` | 1 / 1 | escape only (created by pdescrobject.c method-descriptor get; `PsycoCFunction_Call` uses fields directly) |
| bound method `psyco_computed_method` | 0 type=&PyMethod_Type; 1 `iMETHOD_IM_FUNC`=CT func (SkFlagPyObj); 2 `iMETHOD_IM_SELF`=self | `compute_method`: residual `PyMethod_New(func, self)` CfPure | `direct_compute_method` | im_func\|im_self | 0 / 0 | escape only (created by `pfunc_descr_get`; `pinstancemethod_call` unpacks it) |
| float `psyco_computed_float` | 0 type=&PyFloat_Type; 1 `iFLOAT_OB_FVAL` = one vinfo holding the 64-bit double | `compute_float`: residual `PyFloat_FromDouble` ("d", CfPure) | `direct_compute_float` (buggy, see Leftovers) | 0 | 0 / 0 | escape only; every float op returns a fresh virtual float |
| int (PyLong) | **none** — no virtual type | — | — | — | — | — |
| dict | **none** ("XXX no virtual dicts yet") | — | — | — | — | — |

[inferred] Weight 0/0 for floats and bound methods means they never count against nesting depth and survive across calls.

---

## ni/Objects/ptupleobject.h / .c

**Sections**

| group | class | note |
|---|---|---|
| `TUPLE_ob_item`, `iTUPLE_OB_ITEM`, `PsycoTuple_GET/SET_ITEM` | translated | FARRAY descriptor; macros index the vinfo_array directly |
| `check_ituple` | core | ALL_CHECKS consistency: array count == 2 + CT size |
| `compute_tuple`, `direct_compute_tuple`, `PsycoTuple_New` | core | virtual tuple |
| `PsycoTuple_Load` | core | "unpack" a virtual or CT tuple into the array; refuses CT tuples > `CT_TUPLE_LOAD_SIZE_LIMIT` (15); never sets exception; -1 for run-time tuples |
| `PsycoTuple_Concat` | translated + core | builds a virtual concatenation when both lengths known, else residual sq_concat + `Psyco_AssertType` |
| `ptuple_item` | translated | bounds check + `psyco_get_field_array` |
| `psy_tupleobject_init` | glue | metas: sq_length→`psyco_generic_immut_ob_size`, sq_item, sq_concat, mp_subscript→`psyco_generic_subscript` |

**Patterns**
- virtual-object: `ptupleobject.c:PsycoTuple_New` — CPython `PyTuple_New(n)` + SET_ITEM → Psyco array of vinfos. *judgement* (choice to virtualise, weights).
- ct-fold: `compute_tuple` all-CT branch builds the tuple at compile time. *mechanical-ish* (rule "all inputs CT → evaluate"), but the decision that tuple construction is foldable is *judgement*.
- runtime-branch: `ptuple_item` — `if ((size_t)i >= Py_SIZE(a))` → `integer_cmp(i, vlen, Py_GE|COMPARE_UNSIGNED)` + `runtime_condition_f`. *mechanical* (note the unsigned trick mirrors CPython's `(size_t)` cast).
- exception-set: `PycException_SetString(po, PyExc_IndexError, "tuple index out of range")`. *mechanical*.
- field-access: `psyco_get_const(po, a, FIX_size)` (immutable size, borrowed), `psyco_get_field_array(..., FPYREF(TUPLE_ob_item), i)`. *mechanical* given descriptors.
- verify-type: `PsycoTuple_Concat` tri-state switch. *mechanical*.
- residual-call: fallback `PyTuple_Type.tp_as_sequence->sq_concat`. *mechanical*.
- NEW: `assert-result-type` — after a residual call whose result type is known, `Psyco_AssertType(po, r, &PyX_Type)` (also plist, pdict, plong). *judgement* (requires knowing return type; a meta-compiler could derive it from `PyTuple_New`-style returns only with analysis).
- NEW: `load-sequence` — `PsycoTuple_Load`/`PsycoList_Load` to get a compile-time length and item vinfos, then specialise on arity (used by pmethodobject, pclassobject, pmath, pbltinmodule). *judgement*.

**Leftovers**
- `compute_tuple` passes `int tuple_len` to `"l"` format; `psyco_generic_call` reads `va_arg(vargs, long)` (codegen.c) → UB on x86-64 [inferred; works in practice for small non-negative values]. `PsycoTuple_New(int count)`, `PsycoTuple_Load` return `int`, `check_ituple` int — 32-bit sizes.
- `(-1 << iTUPLE_OB_ITEM)` left-shift of a negative is UB in C [read].
- `PsycoTuple_Concat`: `if (Psyco_VerifyType(v1) != true) return NULL;` returns NULL without an exception if `false` [read]; harmless only because it is registered on tuple's own slot.
- `PsycoTuple_Concat` returns `v2`/`v1` unchanged when the other is empty, without CPython's `PyTuple_CheckExact` test [inferred semantic diff for tuple subclasses].
- "XXX a virtual tuple concatenation would be cool"; "if (mm) /* Python >= 2.3 */" guard is Py2 cruft.

## ni/Objects/plistobject.h / .c

**Sections**

| group | class | note |
|---|---|---|
| `LIST_ob_item`, `LIST_TOTAL`, `VLIST_ITEMS`, `LIST_itemsarray`, `VLIST_LENGTH_MAX` | translated + core | `LIST_itemsarray` is a pure array descriptor for the separately-allocated item block |
| `vlist_length`, `compute_vlist`, `list_maybe_compute`, `PsycoList_NEW/New/SingletonNew`, `PsycoList_Load` | core | virtual short lists |
| `plist_item`, `plist_ass_item` | translated + core | bounds check; virtual fast path when list virtual and index CT |
| `psyco_plist_concat` | translated + core | virtual result when both lists virtual |
| `PsycoList_Append` | translated | residual `PyList_Append` |
| `psy_listobject_init` | glue | metas incl. `PyList_Type.tp_iter → PsycoSeqIter_New` |

**Patterns**
- virtual-object (`PsycoList_New`), field-access (`psyco_get_field(..., VAR_size)`, `psyco_put_field_array(FPYREF(LIST_itemsarray))`), runtime-branch (`plist_item`), exception-set, residual-call (`plist_ass_item` delete path, `PsycoList_Append`).
- refcount: `plist_ass_item` mirrors `list_ass_item`'s `old = items[i]; items[i] = v; Py_DECREF(old)` via `psyco_get_field_array` + `psyco_put_field_array` + `psyco_decref_v`. *mechanical*.
- NEW: `virtual-fast-path` — inside a translated function, `if (a->source == VirtualTime_New(&sv) && is_compiletime(i->source))` operate on the vinfo_array instead of emitting loads/stores (`plist_item`, `plist_ass_item`). *judgement*.
- NEW: `slot-redirect` — register a meta on a type slot that returns a *different* object type than CPython would (`tp_iter` → generic seqiter). *judgement*; observable semantic change: `type(iter([]))` would be `iterator` not `list_iterator` if materialised [inferred].
- `need_reference(po, result)` in `plist_item` virtual path: item returned from a mutable container must own a real reference [read].

**Leftovers**
- `psyco_plist_concat`: `else if (blen == 1) { /* XXX - fallback to an append ? */ }` empty; `/*psyco_empty_list*/` commented-out; "Python >= 2.3" guards.
- `plist_ass_item` "XXX implement item deletion"; passes `(long) NULL` correctly.
- `int` sizes throughout (`PsycoList_New(int size)`).
- Note: `PsycoList_SingletonNew` skips `list_maybe_compute` (fine, length 1).

## ni/Objects/pdictobject.h / .c

**Sections**: `PsycoDict_New/NewPresized/Copy/SetItem/SetItemC` → translated (residual + assert-result-type); `psyco_dict_length` → translated (field-access `DICT_ma_used`); init → glue.

**Patterns**: residual-call (all), assert-result-type, field-access (`dict_length` = `mp->ma_used`, *mechanical*). `NewPresized` threshold `minused > 5` is *judgement*.

**Leftovers**
- `DICT_ma_used` declared `int`; Py3 `ma_used` is `Py_ssize_t` → 4-byte load on x86-64 [read+inferred]; also not NONNEG.
- `HAVE_DICT_NEWPRESIZED` gated on `PY_VERSION_HEX >= 0x02060000` (pyver.h) — Py2 version test, true on 3.7. `_PyDict_NewPresized` takes Py_ssize_t, passed as "l" — OK on LP64.
- Header guard trailing comment `_PSY_LISTOBJECT_H` (copy-paste).
- `PsycoDict_SetItemC` passes the key as a raw constant ("l") — relies on caller keeping it alive [inferred].

## ni/Objects/piterobject.h / .c

**Sections**: `seqiterobject` re-declaration + `SEQITER_*` → translated (hand-copied private struct); `PsycoSeqIter_NEW/New`, `compute_seqiter` → core; `piter_getiter`, `piter_iternext` → translated; init → glue.

**Patterns**
- promote-type: `piter_iternext` `Psyco_NeedType(po, seq)`.
- `piter_iternext` mirrors `iter_iternext`: `PySequence_GetItem` → `PsycoSequence_GetItem`; `PyErr_ExceptionMatches(IndexError) → StopIteration` → `PycException_Matches` + `runtime_NON_NULL_t` + `PycException_SetVInfo`. *mechanical* except the list special-case (`plist_item` directly, ignoring overridden `__getitem__`) which is *judgement*.
- `integer_add_i(po, index, 1, true)` + `psyco_put_field(SEQITER_it_index)`. *mechanical*.
- Initial it_index made run-time (`make_runtime_copy`) to avoid per-iteration specialisation — *judgement* ("very likely to be unpromoted anyway").
- NEW: `forget-mutable-field` — after materialising, `psyco_forget_field` removes cached knowledge of fields others may now mutate (`compute_seqiter`, `compute_vlist`). *mechanical* rule: any FMUT field cached in a virtual object.

**Leftovers / notes**
- `Psyco_DefineMeta(PySeqIter_Type.tp_iter, piter_getiter)`: in Py3 that slot is `PyObject_SelfIter`, shared by many types; metas are keyed by C pointer, so this meta applies to all of them (correct behaviour, returns self) [inferred].
- CPython 3.7 `iter_iternext` clears `it_seq` on exhaustion; Psyco does not (comment admits index divergence) [read comment + inferred].
- `SEQITER_it_index` typed `long` (CPython `Py_ssize_t`) — same size on LP64 only.
- Comment `/* for plist_getitem */` refers to old name; actual is `plist_item`.

## ni/Objects/pfuncobject.h / .c

**Sections**

| group | class | note |
|---|---|---|
| `FUNC_*` descriptors | translated | subset of Py3.7 PyFunctionObject; not FMUT, not FPYREF |
| `compute_function`, `PsycoFunction_NewWithQualName` | core | virtual function from MAKE_FUNCTION (pycompiler.c) |
| `pfunction_simple_call` | core | promote function to CT, then `psyco_call_pyfunc` (inline/recursive compile of the callee) |
| `pfunction_call` | core | tp_call meta: kw must be known NULL; CT/RT → promote (`psyco_pyobj_atcompiletime_mega`, megamorphic → residual); virtual → read fields |
| `pfunc_descr_get` | translated | returns virtual bound method |

**Patterns**
- NEW: `promote-callee` — `psyco_pyobj_atcompiletime_mega` promotes the callable with a megamorphism cut-off, then specialises the call. *judgement*.
- virtual-object (`PsycoFunction_NewWithQualName`, `pfunc_descr_get` → `PsycoMethod_New`).
- residual-call fallback via `PyFunction_Type.tp_call`.

**Leftovers / bugs**
- `compute_function` `SET_FIELD`: `if (other != NULL || !psyco_known_NULL(other))` — should be `&&`; as written it writes every non-NULL field including known-NULL ones, and would pass NULL to `psyco_put_field` if a slot were empty [read].
- Same macro calls `psyco_put_field` on fields without `FMUT` (vcompiler.c `psyco_put_field` has `extra_assert(df & FIELD_MUTABLE)`) and without `FPYREF` → new function object holds no reference to closure/defaults/kwdefaults/annotations → refcount underflow when it dies [inferred from vcompiler.c].
- `pfunction_call` virtual branch passes only code/globals/defaults to `psyco_call_pyfunc`; closure and kwdefaults set by MAKE_FUNCTION are ignored, whereas `pfunction_simple_call` falls back when `PyCode_GetNumFree(co) > 0` [inferred: closures / kw-only defaults wrong on the virtual path].
- `pfunction_simple_call` ignores `func_kwdefaults` too [inferred].
- "XXX we capture the code object" (func.__code__ mutation not tracked); "XXX could do better" for megamorphic; `pfunc_descr_get` "XXX obj is never Py_None".
- No direct_compute → a virtual function cannot appear in a snapshot needing `direct_xobj_vinfo` (would Py_FatalError) [inferred].

## ni/Objects/pmethodobject.h / .c

**Sections**: `CFUNC_*` + `PsycoCFunction_New` → translated + core; `compute_cfunction`, `direct_compute_cfunction` → core; `PsycoCFunction_Call` → translated + core; init → glue.

**Patterns**
- `PsycoCFunction_Call` mirrors CPython's `ml_flags` switch: `psyco_atcompiletime` promotes `m_ml` (promote-value), then meta-dispatch `Psyco_META2/3(po, ml->ml_meth, ...)`. METH_O / METH_NOARGS require `PsycoTuple_Load` to give 1 / 0 (load-sequence). *mechanical* for the switch, *judgement* for arity specialisation.
- NEW: `promote-value` — `psyco_atcompiletime(po, vml)` for a non-type value (pointer) to make it CT; -1 means promotion/error.

**Leftovers**
- METH_FASTCALL / METH_FASTCALL|METH_KEYWORDS (Py3.7) not handled → always residual `PyCFunction_Call` [read; inferred that many 3.7 builtins use it].
- `#ifdef PyCFunction_New /* Python >= 2.3 */` Py2 guard; in 3.7 the macro exists so NewEx branch is used with module NULL → `__module__` lost [inferred].
- `direct_compute_cfunction` reads `m_ml` as long via `direct_read_vinfo` — fine on LP64.
- Mixed formatting style (2-space clang-format) vs tabbed files.

## ni/Objects/pclassobject.h / .c

**Sections**: `METHOD_*`, `PsycoMethod_New` → translated + core; `compute_method`, `direct_compute_method` → core; `pinstancemethod_call` → translated + core.

**Patterns**: `pinstancemethod_call` mirrors `method_call`: prepend `im_self` to arg tuple — done as a *virtual* tuple of len+1 then `PsycoObject_Call(im_func, ...)` (meta re-dispatch). *mechanical* in shape; virtualising the new tuple is *judgement*. `object_non_null` + `runtime_condition_t` = runtime-branch.

**Leftovers**
- Unbound-method branch ("XXX implement me") is Py2-only; Py3 `PyMethod_New` rejects NULL self.
- Stray `if (PycException_Occurred(po)) return NULL;` after a successful condition [read; purpose unclear].
- Name `pinstancemethod_call` and header "Instance methods" are Py2 terminology (Py3 also has a separate `instancemethod` type, unrelated).

## ni/Objects/plongobject.h / .c

**Sections**: `PsycoLong_AsLong/AsDouble/FromLong/FromUnsignedLong/FROM_LONG` → translated (residual); `RETLONG`/`RETFLOAT` slot metas → translated; `plong_pow` → translated; init → glue.

**Patterns**
- residual-call + assert-result-type via `DEF_KNOWN_RET_TYPE_1/2` (pobject.h). Unary ops use `CfCommonNewRefPyObject`; binary ops use `CfCommonCheckNotImplemented` (result may be NotImplemented, so type assert only when not). *mechanical* if return types are annotated.
- `plong_true_divide` → RETFLOAT (known float result). `plong_pow` deliberately not typed (can return float for negative exponent) [read comment; correct for Py3].

**Py2 int specialisation (from these files only)**
- Upstream (9bd5834 `c/Objects/pintobject.[ch]`) had `INT_ob_ival`, virtual ints `PsycoInt_FROM_LONG`, `PsycoInt_AS_LONG`, `PsycoIntInt_RichCompare` with inline machine arithmetic. **None of this exists here**: no virtual PyLong, no inline add/sub with overflow check, no richcompare, no CT folding of small ints [read]. `XXX tony: We should implement a virtual-time computed version of PyLong` (plongobject.h).
- Consequences visible elsewhere in this group: `pbltinmodule.c:longobj_as_long` replaces `intobj_as_long` with a residual `PyLong_AsLong`; `pbuiltin_len` boxes via residual `PyLong_FromLong`; `psyco_convert_to_double` treats ints via residual `PyLong_AsDouble`. So every int operation is a C call [read].
- Upstream plongobject had `nb_divide` (`plong_classic_div`) and typed `nb_power`; both dropped/changed for Py3 [read].
- `PsycoLong_FROM_LONG` decrefs `v` only on success → leaks on error [read].

**Leftovers**: `plong_div` is floor_divide (naming); "XXX implement me" on AsLong/AsDouble.

## ni/Objects/pfloatobject.h / .c

**Sections**

| group | class | note |
|---|---|---|
| `cimpl_fp_*` | translated | tiny C helpers called with CfPure (ct-fold when args CT) |
| `PsycoFloat_FROM_DOUBLE/FromDouble/FromFloat/AS_DOUBLE/AsDouble` | translated + core | virtual float builders, `PyFloat_AsDouble` analogue |
| `compute_float`, `direct_compute_float` | core | |
| `CONVERT_TO_DOUBLE*`, `psyco_convert_to_double` | translated | mirrors floatobject.c `CONVERT_TO_DOUBLE` (long or float, else NotImplemented) |
| `pfloat_richcompare` | translated | float/float only; float/long → residual |
| `pfloat_pos/neg/abs/add/sub/mul/div` | translated | |

**Patterns**
- NEW: `pure-helper-call` — express an operator as a tiny static C function called with `CfPure|CfReturnTypeDouble`, so the call folds at compile time when args are CT and is a plain call otherwise (`cimpl_fp_add`). *mechanical* in principle (CPython's own expression), but needs the backend's FP call ABI (`"d"` format, `HAVE_FP_FN_CALLS`).
- virtual-object for every arithmetic result (`PsycoFloat_FROM_DOUBLE(x)`); a chain `a*b+c` never boxes until escape. *judgement*.
- ct-fold via CfPure.
- Richcompare: op swap (GE→LE swapped, GT→LT swapped) then residual `PyBool_FromLong` — *judgement*; could instead return a condition code.

**Leftovers / bugs**
- `direct_compute_float`: `double value = direct_read_vinfo(...)` converts the raw 64-bit word *numerically* to double; upstream reinterpreted bits through `long*` (two halves on i386). Should be a bit-cast (memcpy) [read upstream + current].
- `pfloat_div` has no zero-divisor check: `1.0/0.0` gives inf instead of `ZeroDivisionError` (CPython `float_div` checks) [read].
- No metas for nb_floor_divide, nb_remainder, nb_power, nb_bool, nb_int → residual via generic paths [read].
- `CF_NONPURE_FP_HELPER` / `CF_PURE_FP_HELPER` / `WANT_SIGFPE_HANDLER` unused; `cimpl_fp_from_long` unused; commented-out `float_cmp` and prototypes in .h; header comment "not-yet-computed integers" stale; "Python >= 2.4" comment.
- `pfloat_richcompare` passes `int op` to "l" (va_arg long) [inferred UB, benign].
- `PsycoFloat_AsDouble` "silently assumes the result is a float object" after nb_float.

## ni/Modules/pmath.c

**Sections**: `PMATH_CONVERT_TO_DOUBLE` → translated; `PMATH_FUNC1/2` generators → translated (generated per function); `psyco_initmath` → glue.

**Patterns**: pure-helper-call (calls libm directly with CfPure), load-sequence (FUNC2 needs a 2-tuple), residual-call fallback to the original `PyCFunction` captured from `Psyco_DefineModuleFn`. Highly *mechanical* — the macro is itself a hand-written meta-compiler.
- NEW: `module-fn-meta` — `Psyco_DefineMetaModule(name)` + `Psyco_DefineModuleFn(md, name, flags, meta)` registers a meta on a builtin module function's C pointer, **only if `ml_flags` match exactly** (pycompiler.c); returns the original C function for fallback.

**Leftovers / bugs**
- No errno/domain/range checks (header TODO): `math.sqrt(-1)` → nan instead of ValueError [read TODO + code].
- `ceil`/`floor` return a float; Py3 returns int and honours `__ceil__/__floor__` [inferred from CPython 3.7].
- Functions whose 3.7 ml_flags differ (Argument-Clinic METH_FASTCALL for e.g. pow/fmod/hypot [inferred]) silently get no meta; `fallback_*` then NULL but unused.
- `MATHMODULE_USES_METH_O` is a Py2.6 version test; `funcname` macro arg unused; K&R `extern double fmod(...)` block for non-STDC compilers.

## ni/Python/pbltinmodule.h / .c

**Sections**: `get_len_of_range`, `longobj_as_long`, `parse_range_args`, `cimpl_range` → obsolete (unused; range moved to prangeobject per git log); `pbuiltin_id/len/abs/apply/divmod` → translated; init → glue.

**Patterns**: `get_len_of_range` is an explicit hand-translation ("translated from bltinmodule.c") using runtime-branch + `integer_sub`; `pbuiltin_apply` uses load-sequence, verify-type, promote-type; `pbuiltin_divmod` → `PsycoNumber_Divmod`.

**Leftovers / bugs**
- `Psyco_DefineMetaModule("__builtin__")`: Py3 module is `builtins`; the import fails, error cleared, `md == NULL`, so **no builtin meta is registered** [read pycompiler.c].
- `apply` does not exist in Py3.
- `parse_range_args`: `PsycoLong_AsLong` returns a new ref, then `vinfo_incref` again → leak (upstream `PsycoInt_AS_LONG` was borrowed) [inferred]; `longobj_as_long` returns NULL without exception when not a long. Dead code anyway; `cimpl_range` never assigned.
- Includes pstringobject.h / plistobject.h unnecessarily.

## Glossary terms

- `FIELD_INDEX(df)`, `FIELDS_TOTAL(lastdf)`, `FARRAY`, `FMUT`, `FPYREF`, `DEF_FIELD`, `NONNEG_FIELD`, `DEF_ARRAY` — vcompiler.h (field descriptor encoding: index in vinfo_array, C offset, size, flags).
- `iOB_TYPE`, `iFIX_SIZE`, `iVAR_SIZE`, `OB_type`, `FIX_size` (immutable ob_size), `VAR_size` (= FMUT(FIX_size)) — Objects/pobject.h.
- `INIT_SVIRTUAL`, `INIT_SVIRTUAL_NOCALL`, `SVIRTUAL_MUTABLE`, `NESTED_WEIGHT_END`, `NW_*` — vcompiler.h.
- `psyco_get_const` (borrowed load of an immutable field, cached) vs `psyco_get_field` (new ref) — vcompiler.h.
- `psyco_forget_field` — vcompiler.h; `vinfo_move`, `vinfo_array_shrink`, `vinfo_would_be_recursive`, `need_reference` — vcompiler.h/.c.
- `direct_read_vinfo` / `direct_xobj_vinfo` — vcompiler.c (evaluate a vinfo from raw saved stack data, without emitting code; used by direct_compute_fn).
- `DEF_KNOWN_RET_TYPE_n` — Objects/pobject.h (residual call + type assertion).
- `psyco_known_NULL`, `psyco_knowntobe` — Python/pycompiler.h.
- `Psyco_DefineMetaModule`, `Psyco_DefineModuleFn` — Python/pycompiler.c.
- `HAVE_FP_FN_CALLS` — x64/iencoding.h (backend supports double args in calls).
- `SkFlagFixed`, `SkFlagPyObj` — compile-time source flags (vcompiler.h): fixed = not to be un-promoted; PyObj = holds a ref.

## Questions

- Is `psyco_put_field` on a not-yet-materialised-then-moved object intended for `compute_function` (it targets `newobj`, a run-time vinfo, so the FMUT assert would fire under ALL_CHECKS)? Should FUNC_* fields be FMUT|FPYREF?
- How are CT doubles stored in `source_known_t.value` on x86-64 (bit pattern in a long)? Determines the correct fix for `direct_compute_float`.
- Should Py3 `range` support live entirely in prangeobject.c, making the range helpers here deletable?
- Is `iter(list)` → generic seqiter still wanted in Py3 given `list_iterator` exposes `__length_hint__`/`__setstate__`?
