---
upstream_ref: 9bd5834
head_ref: e1fd808
written: 2026-11-16
---

# Removed, disabled and weakened mechanisms (surviving files)

Scope: what was removed, disabled, stubbed or weakened *inside files that still exist in HEAD*. For files and subsystems that were deleted outright (psyco.c, profile.c, stats.c, alarm.c, pintobject.c, ptypeobject.c, compactobject.c, pcompactobject.c, parray.c, prangeobject.c, cstruct.c, ppsyco.c, the i386/ivm/dummybackend backends, py-support/psyco), see `doc/history/removed-files.md`. They are named here only where they are cross-referenced.

Path mapping: upstream `c/X` maps to `ni/X`, and `c/i386/X` maps to `ni/x64/X`. `c/x64` was forked from i386 in 4b45ec8 (2018-01-18) and moved to `ni/` in 58aff23.

Evidence labels:
- **[read]**: seen directly in the source or a diff.
- **[inferred]**: deduced, not run or tested.

Reason labels:
- **stated**: from a commit message or code comment.
- **inferred**: not given anywhere.

Relevance labels:
- **core**: part of the specialiser design; keep and tidy.
- **translated**: a meta-compiler reading CPython source would naturally regenerate it.
- **backend**: machine-code backend.
- **obsolete**: Py2-only, or not wanted.

The two biggest commits:
- **a880444** (2020-04-12, "Start of Python3"). Message: "Many optimization have been removed, some would be good to put back with Python3 versions, but requires analysis". Also: "The mergepoints has all been stripped out and replaced with a very simple version".
- **58aff23** (2019-02-14, "Cleanup for Py3"). Message: "Most of the code removed was related to debugging ... or dynamic profiling ... not an immediate goal".

## Summary table

| # | Item | Cat | State in HEAD | Commit | Relevance |
|---|---|---|---|---|---|
| A1 | LOAD_GLOBAL compile-time constant + dict-slot guard | A | gone; always a run-time `cimpl_load_global` call | a880444 | core |
| A2 | Merge-point placement heuristics (confluence weight) | A | replaced by "every instruction is a merge point" | a880444 | core |
| A3 | Early deletion of dead locals (liveness analysis) | A | analysis gone; consumer is a no-op | a880444 | core |
| A4 | Inlining of small Python functions | A | machinery present, effectively never triggered | a880444 | core |
| A5 | Virtual int, inline int arithmetic, int compare | A | gone; generic `PyLong` slot calls | a880444 | core |
| A6 | Virtual bool from condition code | A | gone; `PyBool_FromLong` call | a880444 | core |
| A7 | Virtual strings and string metas | A | compiled out on Py3; init not called | 7441415, a880444 | translated |
| A8 | Virtual range / `for i in range` | A | **restored** (`da1539f`, `prangeobject.c`) | a880444, ee1161b | core |
| A9 | Compile-time attribute lookup (MRO/descriptor) | A | stubbed to generic call | a880444 | core |
| A10 | Instance creation / `type_call` / `__init__` inlining | A | gone (ptypeobject.c deleted) | a880444 | core/translated |
| A11 | Compact objects | A | gone; V2 history in `compact-objects.md` | a880444 | core |
| A12 | Float specialisation | A | disabled then "hacked" back; partial | d57d3c8, 04023e1, 04826fb | core |
| A13 | Builtin metas (len, abs, id, chr, ord, range...) | A | all silently dead (`"__builtin__"` import fails) | a880444, ee1161b | translated |
| A14 | Run-time type switches / fixed switches | A | gone (already disabled upstream) | a880444 | obsolete |
| A15 | PROMOTION_TACTIC 0 | A | gone; tactic 1 kept | a62789d | core (low loss) |
| A16 | Compile-time defaults/globals at top-level entry | A | gone; all args run-time | 37d0f1e, a880444 | core |
| A17 | Register pool (7 i386 → 5 of 16 x64) | A | shrunk | d472b47, 56ed029 | backend |
| A18 | `POP`-based reload in `psyco_unify` | A | removed | c55b4d2, 3cb656c | backend |
| A19 | Compact immediate encodings (`COMPACT_ENCODING`) | A | flag dead; `movabs` everywhere | 2e2c05d | backend |
| A20 | SETcc materialisation | A | `PUSHF/XOR/POPF` "temp fix" | 5865ebc | backend |
| A21 | ALL_STATIC single-unit build | A | off | 47ff60c | backend |
| B1 | Opcode coverage (LOAD_METHOD, POP_EXCEPT, CALL_FUNCTION_KW/EX, ...) | B | see opcode table | a880444, 70882a6 | translated |
| B2 | Frame emulation (`_PyThreadState_GetFrame` hook, f_back chain) | B | hook commented out | 58aff23, a880444 | core |
| B3 | Compile a running frame (`PsycoCode_CompileFrame`) | B | gone | 58aff23 | core |
| B4 | `locals()/eval/vars/dir` safety (`NoControlFlowIfBuiltin`, ppsyco metas) | B | gone, with no replacement guard | 58aff23, a880444 | core |
| B5 | Proxy functions / `psyco.bind` | B | dead code | 58aff23, 7441415 | obsolete |
| B6 | Code-object filter hook, memory accounting, `_psyco` API | B | gone | 58aff23 | translated/core |
| B7 | Kw-only args, closures, module code, generators | B | rejected | e1fd808, a880444 | translated |
| B8 | METH_FASTCALL C calls | B | residual generic call | 04826fb (stated) | translated |
| B9 | `psyco_call_var` asm glue → libffi | B | replaced | 04826fb | backend |
| C | Debug machinery (PSYCO_DEBUG, CODE_DUMP, TRACE_EXECUTION, BREAK_ON, ...) | C | removed for nianalyser | Feb 2019 | obsolete |
| D1 | Handled-exception state mapped to `curexc_*` | D | wrong fields | a880444, e1fd808 | translated |
| D2 | BUILD_MAP, RAISE_VARARGS, END_FINALLY, CALL_FUNCTION oparg keep Py2 meaning | D | miscompiles | a880444 | translated |
| D3 | Float division / math errors not raised | D | silent inf/nan | 04023e1 | translated |
| D4 | `T_INT` read as 8 bytes | D | garbage upper bits | inherited | backend |
| D5 | Alignment-based unification rejection | D | more duplicate code | 5fb5142 | backend/core |
| D6 | 32-bit immediates → `movabs R10` for every known pointer | D | pervasive cost | x64 port | backend |
| D7 | `psyco_call_code_builder` `restore` ignores flags | D | possible lost ccreg | b51f062 era | backend |
| D8 | `MAX_ARGUMENTS_COUNT` 16 → 7 | D | cap | ed8c51a | backend |
| D9 | No hotness heuristic; compile on first call | D | speed/compile-time cost | 37d0f1e | core |

## A. Optimisations and specialisations removed or disabled

**A1. LOAD_GLOBAL tricks** [read]
- *What it did:* `load_global` looked the name up at compile time and folded the result to a compile-time constant. It emitted an inline check that the globals dict slot was unchanged, using the backend macros `DICT_ITEM_KEYVALUE`, `DICT_ITEM_CHECK_CC` and `DICT_ITEM_UPDINDEX` in `c/i386/ipyencoding.h`. If the slot changed, `do_changed_global` either patched the code (the value had only moved within the table) or respawned. If a global changed repeatedly, `mark_varying` demoted it to run-time.
- *Upstream:* `c/Python/pycompiler.c` (`load_global`, `do_changed_global`, `mark_varying`, `case LOAD_GLOBAL`).
- *Now:* `case LOAD_GLOBAL` (also reached from LOAD_NAME) always emits `psyco_generic_call(cimpl_load_global)`. `mark_varying` survives but is only called from STORE_GLOBAL and nothing reads it.
- *Commit:* a880444.
- *Reason:* inferred; Py3 changed the dict layout. The code comment "XXX tony: Add back a fast implementation ... could use PyDictObject's ma_version_tag" is stated.
- *Restore notes:* this is what made calls to global functions and classes resolve at compile time. Every downstream specialisation (calling a known function, inlining, known types) depended on it, so its loss cascades.
- *Relevance:* core.

**A2. Merge-point placement** [read]
- *What it did:* upstream `psyco_build_merge_points` built a control-flow graph (`instrnode_s`) and placed a merge point only at confluence points. Merge points could be delayed by up to `CONFLUENCE_TOTAL_DELAY`=3 "heavy" instructions, light opcodes (`IS_LIGHT_INSTR`) were skipped, merge points were forced after context-dependent opcodes (`IS_CTXDEP_INSTR`: LOAD_GLOBAL, LOAD_NAME), and at least one was placed every `MAX_UNINTERRUPTED_RANGE`=170 instructions. Lookup by binary search.
- *Now:* `ni/mergepoints.c:_find_merge_points`, where `default: MP_HERE()` makes almost every instruction a merge point. The comment "Just making everything a mergepoints works but I think it causes lots of unnecessary reentry points" is stated. Lookup by linear scan (`psyco_next_merge_point`). Storage changed from a PyString to a PyCapsule.
- *Commits:* a880444, 04826fb, 7dfb2c0.
- *Restore notes:* each merge point costs a snapshot (`fz_build`) and a `psyco_compatible` scan. Merge-point density also decides inlinability (A4). The respawn rule (a merge point after context-dependent instructions) must be preserved.
- *Relevance:* core.

**A3. Early deletion of dead locals** [read]
- *What it did:* `FULL_CONTROL_FLOW` liveness analysis (`mark_var_uses`, `back_propagate_mask`, `forward_propagate`, `find_unused_vars`) called `psyco_ge_unused_var`, which stored variable numbers at the front of each merge point's `fatlist`. `psyco_delete_unused_vars` then dropped those locals before snapshotting, which gave smaller snapshots and more unification. It was disabled for code using the builtins in `NoControlFlowIfBuiltin`, sets `MP_FLAGS_CONTROLFLOW`.
- *Now:* the analysis is gone. `psyco_ge_unused_var` (`ni/dispatcher.h`) is never called. `psyco_delete_unused_vars` (`ni/dispatcher.c`, called from `vcompiler.c` and `pycompiler.c`) iterates an empty list.
- *Commit:* a880444. Reason stated: "I don't think we need or want to keep the early deletion of vars, but I'm not 100% sure it is what I think."
- *Trap:* e1fd808 removed the `if (!CodeBuffer_Check(o1)) break;` guard in `psyco_compatible`. That guard is what separated the leading PyLongs from the CodeBuffers in `fatlist`. Restoring A3 needs that guard back.
- *Restored* in `458ef80` (D15), with a liveness pass written for 3.7.
- *Relevance:* core.

**A4. Inlining** [read]
- *What it did:* small functions were inlined. The upstream test was one merge point, `bytecodeweight <= INLINE_MAXIMUM_WEIGHT` (6), and no block stack.
- *Now:* `psyco_call_pyfunc`, `call_with_inline_frame`, `EInline` and `psyco_save_inline_po`/`psyco_restore_inline_po` remain. `_find_merge_points` sets `MP_FLAGS_INLINABLE` only when `mergepoint_count == 1 && flags == 0`, which almost never holds when every instruction is a merge point. There is no weight test any more.
- *Commit:* a880444. Reason inferred: a side effect of A2.
- *Restored* in `085392a` (D10 step 2), with B6 fixed.
- *Relevance:* core.

**A5. Integers** [read, via the Objects pass]
- *What it did:* upstream `pintobject.c` had the virtual `psyco_computed_int` and inline overflow-checked arithmetic (`pint_add`, `sub`, `mul`, `neg`, `abs`, `invert`, `and`, `or`, `xor`, `lshift`, `rshift`, `div`, `mod`, `pow`, `nonzero`). `pobject.c:PsycoObject_RichCompare` had the `PsycoIntInt_RichCompare` path, which produced a condition code with no call.
- *Now:* the file is deleted. In `ni/Objects/plongobject.c:psy_longobject_init`, `RETLONG` metas are only `psyco_generic_call`s to `PyLong_Type` slots. `PsycoLong_AsLong` and `PsycoLong_AsDouble` are "XXX implement me", not `CfPure`, so constant indices are never folded. `PsycoObject_RichCompare`, added back in 07e3cb7, calls `long_richcompare` at run time and then `IsTrue`. `PsycoObject_IsTrue` has no known-bool shortcut ("XXX tony: Need some way to check Py_True/Py_False quickly").
- *Commit:* a880444. Reason stated: "Many optimization have been removed".
- *Restore notes:* Py3 int is a bignum. Needs a virtual one-word small int that falls back to `PyLong` on overflow. `integer_add` and friends in `codegen.c` still exist.
- *Relevance:* core.

**A6. Virtual bool** [read]
- *What it did:* upstream `pboolobject.c` had `psyco_computed_bool` and `PsycoBool_FROM_LONG`, which built a bool from a condition code without materialising it, plus `pbool_and`, `or` and `xor`.
- *Now:* `psy_boolobject_init` is empty and not in `initialize_all_files`. `PsycoBool_FromCondition` (`ni/Objects/pboolobject.h`) calls `PyBool_FromLong` ("XXX tony: reimplement an optimised version").
- *Commit:* a880444. 07e3cb7 states "A virtual bool object is require to make this really fast again."
- *Relevance:* core.

**A7. Virtual strings** [read]
- *What it did:* `psyco_computed_char`, `strslice` and `bufstr` (`USE_BUFSTR`), and the `pstring_item`, `slice`, `concat`, `richcompare` and `mod` metas.
- *Now:* the whole of `ni/Objects/pstringobject.c/.h` is under `#ifdef IS_PY3K #else`, i.e. compiled out on Py3, and `psy_stringobject_init` is not called.
- *Commits:* 7441415 ("Probably need an implementation of PsycoUnicode", stated); the init call went in a880444.
- *Restore notes:* the layout matches `PyBytesObject`. A `str` version needs PsycoUnicode.
- *Relevance:* translated.

**A8. Virtual range** [read]
- *What it did:* `prangeobject.c` (`psyco_computed_range`, `prange_item`, `prange_length`), `plistobject.c:plist_item`'s listrange fast path, and `pbltinmodule.c:pbuiltin_range` and `prange_new`. Together these made `for i in range(n)` a pure register loop.
- *Now:* all gone. `parse_range_args` and `get_len_of_range` are left unused in `ni/Python/pbltinmodule.c`.
- *Commits:* a880444 (listrange), ee1161b (2020-04-24, "prangeobject needs fixing", stated).
- *Restore notes:* the Py3 `rangeobject` holds PyObject start, stop and step. Needs A5's small int.
- *Relevance:* core.

**A9. Attribute lookup** [read]
- *What it did:* upstream `pobject.c:PsycoObject_GenericGetAttr` resolved the MRO and descriptors at compile time and used `getdictoffset`. `PsycoObject_GetAttr` dispatched to the `tp_getattro` meta.
- *Now:* both are plain generic calls in `ni/Objects/pobject.c` ("XXX tony: Optimize this but allow changes to type dict").
- *Knock-on [inferred]:* `pfunc_descr_get`, `pmember_get` and the method-descriptor metas are still registered but unreachable from LOAD_ATTR, so no virtual bound methods are built.
- *Commit:* a880444.
- *Restore notes:* guard on `tp_version_tag`.
- *Relevance:* core.

**A10. Instance creation** [read]
- *What it did:* `ptypeobject.c` (deleted) had `ptype_call`, `ptype_genericnew`, `ptype_genericalloc`, `pslot_tp_init` and `psyco_pobject_new`.
- *Now:* gone; type calls are generic.
- *Commit:* a880444.
- *Relevance:* core/translated.

**A11. Compact objects** [read]
- *Now:* `compactobject`/`pcompactobject` removed in a880444. Stated: "removed mainly because I don't understand it ... It is very fast when used so analysis should be done". 58aff23 had already said "Compact object is broken!".
- *Relevance:* core (design question).

**A12. Floats** [read]
- *History:*
  - d57d3c8 (2018-06-25, "Removed float specialisation because its broken on x64", stated) set `HAVE_FP_FN_CALLS 0` in `iencoding.h`.
  - 04023e1 (2020-03-14, "Hacked in float support") set it back to 1, rewrote `pfloatobject.c` and `pmath.c`, and added XMM argument and return macros (`CALL_SET_ARG_FROM_RT_DOUBLE`, `MOVQ_*`).
  - 04826fb (2020-04-15) added libffi for compile-time double calls. Stated: "deliberately skipped adding compile time support for doubles ... on the london > frankfurt train".
- *Still missing vs upstream:* `pfloat_nonzero`, `pfloat_pow`, the mixed float/int compares (`cimpl_fp_*_int`), and floor-divide and remainder. Arithmetic is out-of-line `CfPure` C helpers, as upstream; there is no inline SSE.
- *Stale:* the "XXX hack remove float support" comment in `ni/x64/iencoding.h`.
- *Relevance:* core.

**A13. Builtin metas** [read]
- *What happened:* upstream registered `range`, `xrange`, `chr`, `ord`, `id`, `len`, `abs`, `apply` and `divmod`. HEAD `psyco_bltinmodule_init` registers only `id`, `len`, `abs`, `apply` and `divmod`, and looks them up with `Psyco_DefineMetaModule("__builtin__")`. That module does not exist on Py3 (it is `builtins`), so the error is cleared and nothing is registered.
- *Also:* `apply` is gone in Py3, and `divmod` is `METH_FASTCALL` in 3.7 [inferred], so its flags check would reject it anyway.
- *Commits:* a880444 removed chr, ord and xrange; ee1161b removed range.
- *Relevance:* translated.

**A14. Run-time switches** [read]
- *What it was:* `USE_RUNTIME_SWITCHES` (`fixed_switch_t`, `psyco_finish_fixed_switch`, `psy_object_init` switches such as `psyfs_int` and `int_long`). Already 0 upstream ("DISABLED, no longer maintained").
- *Now:* deleted in a880444.
- *Relevance:* obsolete.

**A15. PROMOTION_TACTIC 0** [read]
- *What it was:* a dict-keyed cache of promoted values.
- *Now:* removed in a62789d (2016-01-22). Stated: "make the code simpler - see VC history". Tactic 1 (chained list, `PROMOTION_FAST_COMMON_CASE`, `MEGAMORPHIC_MAX`) is intact in `ni/dispatcher.c`. Tactic 2 (a binary tree) was never implemented.
- *Relevance:* core, small loss.

**A16. Top-level entry specialisation** [read]
- *What it did:* upstream `psyfunc.c:psycofunction_call` compiled with the globals and defaults as compile-time constants and cached one entry per argument count (`psy_fastcall`).
- *Now:* `ni/eval_hook.c:NiEval_EvalFrame` (PEP 523) uses `PsycoObject_FromCode` with all arguments as run-time values. There is one entry per code object (`co_to_entry_point`). If the globals differ it falls back to the interpreter permanently. Psyco→Psyco calls through `psyco_call_pyfunc` still get compile-time defaults.
- *Commit:* 37d0f1e (2019-02-11, "Py27 approach to running JIT code like Py3 will").
- *Relevance:* core.

**A17. Register pool** [read]
- *Upstream:* `c/i386/iencoding.c:RegistersLoop` cycled 7 registers (all except ESP) and spilled EAX/ECX/EDX around calls (`SAVE_REGS_FN_CALLS`).
- *Now:* `ni/x64/iencoding.c:RegistersLoop` cycles R13→R14→R15→RBX→RBP, 5 of 16 registers. R12 is used only by the glue, R10/R11 are transients, and RAX, RCX, RDX, RSI, RDI, R8 and R9 are never allocated.
- *Commits:* d472b47 (`NEW_REGISTERS`), 56ed029 (dropped the alternative).
- *Reason:* inferred; it avoids caller-saved spills.
- *Relevance:* backend.

**A18. `psyco_unify` POP reloads** [read]
- *Upstream:* `c/i386/idispatcher.c:psyco_unify` built a `pops[]` table and loaded values near the stack top with `POP reg`.
- *Now:* `ni/x64/idispatcher.c` does reg moves/`XCHG` (better than upstream), then `RTVINFO_FROM_STACK_TO_REG` loads, then `STACK_CORRECTION`.
- *Commits:* c55b4d2 (stated: "unsafe playing with the stack"), 3cb656c ("Simplify unification code").
- *Relevance:* backend (minor).

**A19. Compact encoding** [read]
- *Upstream:* `COMPACT_ENCODING` was tested in about 12 i386 macros.
- *Now:* `#define COMPACT_ENCODING 1` in `ni/x64/iencoding.h` with no users. imm8/imm32 selection is hard-wired for arithmetic, but `MOV_R_I`, `LOAD_REG_FROM_IMMED`, `PUSH_I`, `MOV_O_I` and `SAVE_IMMED_TO_EBP_BASE` always use the 10-byte `movabs`, via R10 where needed.
- *Restored* (`9d48399`) [run]: `MOV_R_I`, which the others go through, now picks a 5-byte zero-extending MOV, a 7-byte sign-extending `C7 /0`, or `movabs` only when needed. `MOV_R_UI` (patched later) stays 10 bytes. The `COMPACT_ENCODING` flag itself is still unused.
- *Commit:* 2e2c05d ("will remove COMPACT_ENCODING in future", stated).
- *Relevance:* backend.

**A20. SETcc** [read]
- *Upstream:* `SETcc r8` + `MOVZX`.
- *Now:* `NEED_FREE_BYTE_REG` emits `PUSHF; XOR r,r; POPF` every time.
- *Commit:* 5865ebc ("temp fix for NEED_FREE_BYTE_REG", stated).
- *Relevance:* backend.

**A21. ALL_STATIC** [read]
- *Now:* `ALL_STATIC 0` in `ni/psyco.h`. The include list remains in `ni/initialize.h`.
- *Commit:* 47ff60c (2016-01-22).
- *Relevance:* backend.

## B. Mechanisms and features removed

**B1. Opcodes.** See the table at the end. The main gaps:
- LOAD_METHOD/CALL_METHOD are not supported. 3.7 emits them for every `obj.m()`, so any function with a method call is rejected [inferred from the 3.7 compiler].
- POP_EXCEPT is not supported, so every function with try/except is rejected.
- CALL_FUNCTION_KW is not supported (70882a6, "needs fixing", stated), nor is CALL_FUNCTION_EX.
- Also unsupported: DUP_TOP_TWO, BUILD_CONST_KEY_MAP, BUILD_SET, BUILD_STRING/FORMAT_VALUE and LOAD_BUILD_CLASS.
- Py2-only opcodes removed in a880444: the SLICE family, PRINT_*, BUILD_CLASS, UNARY_CONVERT, BINARY_DIVIDE, ROT_FOUR, DUP_TOPX, SET_LINENO.

**B2. Frame emulation** [read]
- *What it did:* the `_PyThreadState_GetFrame = psyco_threadstate_getframe` hook with `cached_frame`, plus `visit_first_frame`, `visit_nth_frame` and `psyco_find_frame`. This made `sys._getframe()`, `f_back` and tracebacks see Psyco frames.
- *Now:* the hook is commented out in `ni/Python/frames.c:psyco_frames_init` ("XXX tony: Can't do this anymore"; "need a new way to do this, write a PEP?"). `psyco_emulate_frame` and `psyco_get_globals` are left unused. Under PEP 523 the real frame exists, but its `f_lasti`, `f_lineno` and `f_locals` are not updated, and inlined callees have no frame at all.
- *Commits:* 58aff23, a880444.
- *Relevance:* core.

**B3. Compiling a running frame** [read]
- *What it did:* `PsycoObject_FromFrame`, `PsycoCode_CompileFrame` and `PsycoCode_Run` (`c/Python/frames.c`) let profiler-driven compilation start in the middle of a loop.
- *Now:* gone. frames.c went from 736 to 364 lines.
- *Commit:* 58aff23. Stated: "dynamic profiling for code compilation which is not an immediate goal".
- *Restore notes:* depends on the deleted profile.c and stats.c.
- *Relevance:* core.

**B4. `locals()` safety** [read]
- *What it did:* the `ppsyco.c` metas for `globals`, `eval`, `execfile`, `locals`, `vars`, `dir` and `input`, together with `NoControlFlowIfBuiltin` in mergepoints.
- *Now:* both gone. Nothing stops a function that calls `locals()` or `eval()` from being compiled [inferred correctness risk].
- *Commits:* 58aff23, a880444.
- *Relevance:* core.

**B5. Proxy functions** [read]
- *What they were:* `psyco_proxycode`, `psyco_PsycoFunction_New` and `psyco.bind`/`proxy`.
- *Now:* removed. `psyco_PsycoFunction_NewEx` remains but is never called, so `PsycoFunction_Type` and `psycofunction_call` are dead code, and `is_proxycode()` is always false.
- *Commits:* 58aff23, 7441415. Replaced by the PEP 523 hook (37d0f1e).
- *Relevance:* obsolete.

**B6. `_psyco` API** [read]
- *What it was:* `psyco_codeobj_filter_fn` (`setfilter`), `psyco_memory_usage` accounting, and `profiling`, `turbo_*`, `stat*`, `charge`, `cannotcompile` and `finalizer`.
- *Now:* gone with c/psyco.c. `ni/ni.c` has an empty method table and exports only `co_to_mp`, `co_to_entry_point` and `error`.
- *Commit:* 58aff23.

**B7. Rejected code shapes** [read]
- *Where:* `ni/mergepoints.c:_build_merge_points` ("XXX tony: all these not supported/buggy currently") and `psyco_call_pyfunc`.
- *What is rejected:*
  - module code (`NiEval_EvalFrame` also requires `f_globals != f_locals`);
  - generators and coroutines;
  - `co_kwonlyargcount != 0` (e1fd808);
  - closures (no LOAD_DEREF);
  - in `pfuncobject.c:pfunction_call`, any call with `kw`, any function with free variables, and megamorphic sites; `__kwdefaults__` is never passed.
- *Relevance:* translated.

**B8. METH_FASTCALL** [read]
- *Now:* `ni/Objects/pmethodobject.c` handles only the classic flags. Fast-call builtins go to the residual `PyCFunction_Call`. 04826fb states "adding compiler support for the *FastCall* versions would be really nice".
- *Relevance:* translated.

**B9. Compile-time C calls** [read]
- *Upstream:* `glue_call_var`/`psyco_call_var` assembler glue.
- *Now:* libffi `var_call` (`ni/x64/iencoding.c`) with tags `l` and `D` only. Any other tag calls `abort()`, and a double return lands in a `long` result.
- *Commit:* 04826fb (stated: "a lot more sane").
- *Relevance:* backend.

**B10. Py2-only object features** [read]
- *What went:* `nb_divide`, `nb_nonzero`, old-style instances (`pinstance_new`), coercion (`cimpl_oldstyle_binary_op1`), `T_CHAR` and the `array` module metas (parray.c).
- *Commit:* a880444.
- *Relevance:* obsolete, except `array`, which is translated.

**B11. Stubs** [read]
These all fall back to generic calls:
- `ni/Objects/pabstract.c`: `PsycoSequence_GetSlice`, `SetSlice`, `Contains` (iter path), `PsycoSequence_Tuple`, `PsycoNumber_InPlacePower` and the ternary operators.
- `ni/Objects/pobject.c`: `PsycoObject_Repr`.
- `ni/Objects/pclassobject.c`: unbound methods.
- `ni/Objects/plistobject.c`: item deletion.
- `ni/Objects/pdictobject.c`: virtual dicts (also absent upstream).
Relevance: translated.

## C. Debug and diagnostic machinery removed in favour of nianalyser

All stated in commit messages, all in Feb 2019 unless noted. Relevance: obsolete.

| Item | Commit(s) |
|---|---|
| `PSYCO_DEBUG` (`ALL_CHECKS` now hard 0; `extra_assert` sites remain) | 5db1615 |
| `VERBOSE_LEVEL`, `debug_printf`, `psyco_logger`/`psyco_flog` | 58aff23 |
| `HEAVY_MEM_CHECK`, `PSYCO_NO_LINKED_LISTS`, `0xCD` fill checks in blockalloc | 5db1615, 58aff23 |
| `CODE_DUMP` (`psyco_dump_code_buffers`, codebuf `chained_list`, promotion `signature`/`zero_tag`) | 892d5a6, 63ab501, 58aff23 |
| `TRACE_EXECUTION`, `EMIT_TRACE` | 56ed029 (2018), a985a43, 58aff23 |
| `BREAK_ON` | d763a87 |
| Stack-depth check (`INITIAL_STACK_DEPTH` 16 → 8) | f6ccc81 |
| Stack-alignment check | a898292 |
| Codegen logs (`LOG_BEGIN/END_CODE_GEN`) | 63ab501, 4118b8b |
| py-utils (`xam.py`, `httpxam.py`, `ivmdump`...) | 012b48f (2018) |

The replacement is the `NI_TRACE` hooks (`ni_trace_*` in `ni/ni.c`, macros in `ni/x64/iencoding.h`, default 0), used as nianalyser breakpoints.

## D. Port changes that silently weaken correctness or speed

**D1. Handled-exception state** [read]
- *Where:* `ni/Python/pycompiler.c:cimpl_set_exc_info` and `cimpl_finalize_frame_locals`.
- *What happened:* Py2 `tstate->exc_type/value/traceback` was turned into `tstate->curexc_*`. In Py2 those fields held the *handled* exception; `curexc_*` is the *pending* error indicator. In 3.7 the handled exception lives in `tstate->exc_info->exc_*`.
- *Further damage:* e1fd808 removed the `Py_None` substitution, so `Py_INCREF(tstate->curexc_type)` may dereference NULL. `PySys_SetObject("exc_type", ...)` is meaningless on Py3.
- *Commit:* a880444. Reason inferred.
- *Effect:* `sys.exc_info()` is wrong and the error indicator is clobbered. Currently masked because try/except functions are rejected (POP_EXCEPT).

**D2. Opcodes with Py2 semantics still whitelisted** [read]
- **BUILD_MAP** still pushes an empty presized dict. In 3.5+ the oparg is a pair count to pop, so `{k: v}` with a non-constant key corrupts the virtual stack.
- **RAISE_VARARGS** 2 is treated as (type, value). In Py3 it means `raise exc from cause`.
- **END_FINALLY** assumes the 2.7 3-item layout.
- **`psyco_ext_do_calls`** decodes `oparg & 0xff` / `>>8` and computes `(opcode-CALL_FUNCTION)&3`. That is correct for CALL_FUNCTION with fewer than 256 args and wrong for CALL_FUNCTION_KW, hence 70882a6.

**D3. Floats and math** [read]
- `cimpl_fp_div` is a bare `a / b` (no `ZeroDivisionError`) and the `PyFPE` guards were dropped.
- `pmath.c` lost `CIMPL_MATH_FUNC1/2` (errno checks), so `sqrt(-1)` gives nan. The TODO is stated.
- `math.ceil`/`floor` return float, but Py3 returns int [inferred].
- `direct_compute_float` converts `long` bits numerically instead of reinterpreting them [inferred bug].
- *Commit:* 04023e1.

**D4. `T_INT`/`T_UINT` read as 8 bytes** [read]
- *Where:* `ni/Objects/pstructmember.c` ("XXX assumes sizeof(int) == sizeof(long)").
- *Effect:* the upper 32 bits are garbage on LP64. This is a latent assumption inherited from 32-bit.

**D5. Unification rejected on alignment** [read]
- *Where:* `ni/dispatcher.c:psyco_compatible`, via `compare_return_address_in_a_stupid_way`. It rejects snapshots whose return-address slot differs mod 16.
- *Commit:* 5fb5142 (stated: "kludge").
- *Effect:* more duplicate code. Tied to the 16-byte call padding in `BEGIN_CALL` (e598ca6, 557bd03).

**D6. Known pointers no longer fit in immediates** [read]
- Every known object or function pointer is above 2^31, so a compare, push or store of a compile-time object needs `movabs R10` first. On i386 these were imm32 operands. This is a pervasive cost of the port.

**D7. `psyco_call_code_builder(restore=true)`** [read]
- *Where:* `ni/x64/iencoding.c`.
- *Upstream:* `TEMP_SAVE_REGS_FN_CALLS` saved EAX/ECX/EDX and the flags.
- *Now:* only RAX is pushed and popped; no flags, no `NEED_CC`.
- *Users:* the respawn proxy and `psyco_coding_pause`.
- *Risk [inferred]:* a live condition code is lost.

**D8. `MAX_ARGUMENTS_COUNT` 16 → 7** [read]
- *Where:* `ni/codegen.c:psyco_generic_call`.
- *Commit:* ed8c51a. Reason inferred: x64 argument registers.

**D9. Entry policy** [read]
- Compile on first call, with no hotness heuristic (the comment in the `eval_hook.c` header is stated), plus a per-call thread-dict insert in `psyco_frame_start_register`. `throwflag` is ignored.
- *Commit:* 37d0f1e.

**D10. RunTime source bit layout** [read]
- *Where:* `ni/vcompiler.h`. `RunTime_StackMask` is still `0x01FFFFFC`, the register field is 4 bits at bit 28, and the guard is relaxed to `REG_TOTAL > 255`.
- *Effect:* works for 16 registers but is fragile [inferred].

**Survives intact (not losses):**
- `VLOCALS_OPC 1`, `COMPRESS_COMPILETIME_SUBITEMS 0` (as upstream), `PROMOTION_FAST_COMMON_CASE 1`, `NEED_STACK_FRAME_HACK 1`, `DEFAULT_RECURSION 10` (moved to eval_hook.c).
- Respawn, coding pauses, freezing and compression of the compiler state, virtual tuple, list, function, method and cfunction, the seq-iterator, and the inline refcount macros in `ipyencoding.h`.
- Code buffers are never freed, as upstream.

## Opcode coverage: upstream vs now

Gate: upstream `c/mergepoints.c:psyco_build_merge_points` (tables `IS_*_INSTR`, `OTHER_OPCODE`, `SUPPORTED_COMPARE_ARG`); HEAD `ni/mergepoints.c:_build_merge_points` (explicit `case` whitelist). A code object containing any opcode not accepted is never compiled (falls back to CPython). "Compiler" = has a `case` in `psyco_pycompiler_mainloop`. Upstream is CPython 2.7 bytecode, HEAD is CPython 3.7 wordcode. All rows read from source unless marked (inferred).

| Opcode | Upstream gate / compiler | HEAD gate / compiler | Notes |
|---|---|---|---|
| POP_TOP, ROT_TWO, ROT_THREE, DUP_TOP, NOP | yes / yes | yes / yes | |
| ROT_FOUR | yes / yes | n/a in 3.7 / case removed (a880444) | Re-added in CPython 3.8; translated. |
| DUP_TOPX (2.7) → DUP_TOP_TWO (3.x) | yes / yes | **no / no** | DUP_TOP_TWO never added; used by augmented subscript assignment (`a[i] += 1`), so such functions are rejected. |
| UNARY_POSITIVE/NEGATIVE/NOT/INVERT | yes / yes | yes / yes | |
| UNARY_CONVERT (backticks) | yes / yes | n/a / removed a880444 | Py2 only; obsolete. |
| BINARY_* / INPLACE_* (ADD, SUB, MUL, MOD, POW, SUBSCR, shifts, AND/OR/XOR, TRUE/FLOOR_DIVIDE) | yes / yes | yes / yes | Same `BINARY_OPCODE` macro; speed now depends on what `PsycoNumber_*` metas remain (see A). |
| BINARY_DIVIDE, INPLACE_DIVIDE | yes / yes (honours `-Qnew`) | n/a / removed a880444 | Py2 only. |
| BINARY/INPLACE_MATRIX_MULTIPLY | n/a | no / no | New in 3.5. |
| SLICE+n, STORE_SLICE+n, DELETE_SLICE+n | yes / yes (`psyco_apply_slice`, `psyco_assign_slice`, `_PsycoEval_SliceIndex`) | n/a / removed a880444 | 3.x uses BUILD_SLICE + BINARY_SUBSCR; the virtual fast path for int slice indices went with it (inferred loss of speed). |
| STORE_SUBSCR, DELETE_SUBSCR | yes / yes | yes / yes | |
| PRINT_EXPR, PRINT_ITEM(_TO), PRINT_NEWLINE(_TO) | yes / yes | PRINT_EXPR exists in 3.7 but no / no | PRINT_ITEM* Py2 only. PRINT_EXPR only in interactive code. |
| BREAK_LOOP, CONTINUE_LOOP | yes / yes | yes / yes | Removed from CPython in 3.8. |
| RAISE_VARARGS | yes / yes | yes / yes — **semantics wrong** | HEAD keeps Py2 meaning (oparg 2 = type,value; 3 = +traceback). In 3.x oparg 2 is `raise exc from cause` and 3 does not exist. `raise X from Y` compiles to a raise of X with value Y (read: `case RAISE_VARARGS`, `cimpl_do_raise`). |
| RETURN_VALUE | yes / yes | yes / yes | |
| RETURN_NONE, SET_LINENO | conditional (`#ifdef`) | dead `#ifdef` | Ancient Python; obsolete. SET_LINENO hook carried TRACE_EXECUTION (removed 58aff23). |
| POP_BLOCK, END_FINALLY | yes / yes | yes / yes — **semantics Py2** | 3.x `except` handlers push 6 values and end in POP_EXCEPT; HEAD END_FINALLY still assumes the 2.7 3-value layout. |
| POP_EXCEPT | n/a | **no / no** | Every 3.x `except:` clause emits it, so any function with try/except is rejected even though SETUP_EXCEPT is whitelisted (inferred from CPython 3.7 compiler; not run). |
| SETUP_LOOP, SETUP_EXCEPT, SETUP_FINALLY | yes / yes | yes / yes | |
| SETUP_WITH, WITH_CLEANUP_* | rejected explicitly / no | no / no | Never supported. |
| BUILD_CLASS | yes / yes (`cimpl_build_class`) | n/a / removed a880444 | 3.x uses LOAD_BUILD_CLASS + call; not added. |
| LOAD_BUILD_CLASS | n/a | no / no | Class bodies cannot be compiled. |
| UNPACK_SEQUENCE | yes / yes | yes / yes | |
| UNPACK_EX | n/a | no / no | Starred assignment. |
| STORE_ATTR, DELETE_ATTR, LOAD_ATTR | yes / yes | yes / yes | |
| STORE_GLOBAL, DELETE_GLOBAL | yes / yes | yes / yes | |
| LOAD_GLOBAL | yes / yes — **compile-time constant fold + dict-slot guard** | yes / yes — **always `cimpl_load_global` run-time call** | Biggest single loss; see A1. |
| LOAD_NAME, STORE_NAME, DELETE_NAME, IMPORT_STAR | gated as module-only and then disabled (`if (1) /* disabled, currently buggy */`) | LOAD_NAME/STORE_NAME no (commented out in e1fd808); DELETE_NAME and IMPORT_STAR **yes** | DELETE_NAME/IMPORT_STAR are whitelisted but only make sense at module level, which `_build_merge_points` rejects via `module`; harmless but inconsistent. |
| LOAD_CONST, LOAD_FAST, STORE_FAST, DELETE_FAST | yes / yes | yes / yes | |
| LOAD_CLOSURE, LOAD_DEREF, STORE_DEREF, DELETE_DEREF, LOAD_CLASSDEREF | no / no (`MISSING_OPCODE` comments) | no / no | Closures never supported. |
| BUILD_TUPLE, BUILD_LIST | yes / yes | yes / yes | |
| BUILD_MAP | yes / yes (oparg = presize hint) | yes / yes — **semantics wrong** | In 3.5+ oparg = number of key/value pairs popped from the stack. HEAD still pushes an empty presized dict and pops nothing, so `{k: v}` with non-constant keys corrupts the virtual stack (read: `case BUILD_MAP`). |
| STORE_MAP | yes / yes | n/a / dead `#ifdef` | Removed from CPython 3.5. |
| BUILD_CONST_KEY_MAP, BUILD_SET, SET_ADD, BUILD_STRING, FORMAT_VALUE, BUILD_*_UNPACK* | n/a | no / no | Dict literals with constant keys, sets, f-strings, `*`/`**` unpacking all reject the function. |
| LIST_APPEND, MAP_ADD | yes / yes | yes / yes | MAP_ADD key/value order changed in 3.8, not 3.7. |
| COMPARE_OP | yes, restricted by `SUPPORTED_COMPARE_ARG` | yes, **any oparg** | `PyCmp_*` redefined in `ni/Python/pycinternal.h`. |
| IMPORT_NAME, IMPORT_FROM | yes / yes | yes / yes | `VERYCONVOLUTED_IMPORT_NAME` path still Py2-shaped (inferred). |
| JUMP_FORWARD, JUMP_ABSOLUTE, JUMP_IF_{TRUE,FALSE}_OR_POP, POP_JUMP_IF_{TRUE,FALSE} | yes / yes | yes / yes | |
| GET_ITER, FOR_ITER | yes / yes | yes / yes | |
| CALL_FUNCTION | yes / yes | yes / yes | Through `psyco_ext_do_calls`, which still decodes the Py2 oparg (`na = oparg & 0xff`, `nk = oparg >> 8`). Correct for < 256 positional args only. |
| CALL_FUNCTION_KW | yes / yes (Py2 meaning: `f(**kw)`) | **no** (removed 70882a6) / case present | Py3 meaning (tuple of names on top) is not implemented; `flags = (opcode-CALL_FUNCTION)&3` gives `CALL_FLAG_KW` and would treat the names tuple as a `**` dict. Gate removed: "CALL_FUNCTION_KW needs fixing" (stated). |
| CALL_FUNCTION_VAR, CALL_FUNCTION_VAR_KW (2.7) → CALL_FUNCTION_EX (3.6+) | yes / yes | no / no | `*args`/`**kw` calls reject the function. |
| LOAD_METHOD, CALL_METHOD (3.7) | n/a | **no / no** | CPython 3.7 emits these for every `obj.meth(...)` call, so any function calling a method is not compiled (inferred from CPython 3.7 compiler). Probably the largest practical coverage gap. |
| MAKE_FUNCTION | yes / yes (+ MAKE_CLOSURE not) | yes / yes (rewritten e1fd808 for 3.x flags 0x01/0x02/0x04/0x08) | Closure flag accepted although LOAD_CLOSURE is not, so 0x08 cannot occur in compiled code. |
| BUILD_SLICE, EXTENDED_ARG | yes / yes | yes / yes | |
| YIELD_VALUE, YIELD_FROM, GET_AWAITABLE, async ops | rejected via CO_GENERATOR | rejected via CO_GENERATOR/CO_COROUTINE | Generators never supported. |
| SETUP_ANNOTATIONS | n/a | no / no | Module/class only. |

Code-object level gates in HEAD `_build_merge_points` (all "XXX tony: all these not supported/buggy currently", stated, e1fd808): `module` code, `CO_COROUTINE|CO_GENERATOR`, and `co_kwonlyargcount != 0` (also refused in `psyco_call_pyfunc`). Upstream rejected only generators, plus the user `psyco_codeobj_filter_fn` hook (removed 58aff23) and module-only opcodes.
