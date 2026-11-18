# Known bugs

Open bugs only. IDs: **M** memory access (`ni/x64/v_mem_mov.h`, field functions in `ni/vcompiler.c`), **F** front end (translated CPython), **B** backend (`ni/x64`), **C** core, **G** generator (`tools/metac`). IDs are never reused. A fixed bug leaves this list; its story is in git (`git log --grep F42`). Fixed bugs still cited from code, tools or other docs keep one line at the end so the reference resolves.

**Live** means current Python 3 code can reach it; **dormant** means no current caller triggers it. Evidence: **[read]** seen in code, **[inferred]** reasoned, **[run]** observed.

## Open

### Memory access
- **M7** (live, minor): `psyco_put_field_array` passes `(df & FIELD_UNSIGNED)` as `signed_` and `psyco_internal_putfld` passes `== 0`, the opposite sense. Stores use it only for the immediate-range test, so the cost is inefficiency. [read]
- **M11** (live, benign): `pdictobject.c` declares `DICT_ma_used` as `int`; it is `Py_ssize_t` in Python 3. Right while counts stay below 2^31. [read]

### Front end
- **F12** (latent): `PsycoObject_IsTrue` doesn't normalise lengths to 0/1 and `PsycoObject_RichCompareBool` returns the raw value. Every caller today only tests against zero; a new caller that uses the value as 0/1 must normalise it. [read]
- **F18** (dormant): the Py2 per-frame saved exception (`FINALIZE_FRAME_LOCALS`, `cimpl_finalize_frame_locals`, a prologue slot) is never set, so it is dead code that would write `curexc_*` if reached. See B17. [read]
- **F27** (to fix on 3.7): running out of memory mid-compile aborts ("psyco: out of memory"): `PsycoObject_New`/`array_grow1` (`vcompiler.h`), the code buffer object (`codemanager.c`), `Psyco_Lookup` during a meta call (`pycompiler.h`). Raising MemoryError would mean unwinding a half-built compilation; the merge-point analysis and frame registration already fail cleanly. [run]
- **F34** (accepted): instance-dict lookups by an exact str name are called `CfNoPython` (`pobject.c`, `cf_flags.h`; `policy_table.py` `ASSUMPTIONS` for `PyDict_GetItem`), so a str-subclass key whose `__eq__` changes the class during the lookup isn't seen. [read]
- **F36** (live): holding a class's shared keys (`pobject.c:shared_key_slot`, D31) can stop CPython re-sharing them when the class's only live instance grows its keys after a compile (`_PyObjectDict_SetItem` re-shares only if the class holds the sole reference). Results stay right; that class's instances use ordinary dicts (go's `Board`). Fixes tried cost more than the bug. [read], [run]
- **F72** (accepted, D64): a direct compiled call into another module reads that module's `__builtins__` when the call site compiles, where CPython reads it at each frame start (`psyfunc.c:callee_builtins`). Exact would need a guard at every cross-module call. [inferred]
- **F97** (live): UNPACK_SEQUENCE (`pycompiler.c`) takes its tuple and list fast paths for subclasses (`PyType_TypeCheck`, where ceval uses `PyTuple_CheckExact`/`PyList_CheckExact`), so a subclass's `__iter__` is skipped; `cimpl_unpack_list` likewise. [read]
- **F100** (live): `sys._getframe(-1)` raises ValueError where CPython returns the top frame: `frames.c:visit_nth_frame` stops only when its counter reaches exactly 0. [read]
- **F103** (live): a negative index on a tuple or list subclass defining `__len__` calls it: `pabstract.c:psyco_generic_subscript` and `psyco_generic_ass_subscript` go through `PsycoSequence_GetItem`/`SetItem` (`sq_length`), where 3.7 adds `Py_SIZE`. [read]
- **F105** (out of memory only): `pycompiler.c:PsycoTraceBack_Here` calls `cimpl_vt_traceback` as `CfCommonNewRefPyObjectNoError`, but when `PyFrame_New` or `PyTraceBack_Here` fails it returns `Py_None` with a MemoryError still set. [read]

### Backend
- **B10** (environment): executable memory is mapped RWX (no W^X); hardened kernels may refuse it. [read]
- **B17** (dormant): a function with exception handlers still reserves three stack words in its prologue (`codegen.c:psyco_emit_header`, `INITIALIZE_FRAME_LOCALS`) that nothing reads; a push and a stack correction per entry. Removing them changes `LOC_CONTINUATION`'s array and the return's stack arithmetic. [read]

### Core
- **C1** (cost): `dispatcher.c:compare_return_address` rejects a match whose return-address slot differs mod 16, which is needed (call alignment derives from it) but duplicates some blocks (1.8% over 23 benchmarks). Fixing it means aligning at calls instead. [read], [run]
- **C3** (open): compiled code and many code-buffer references are never freed. See C14. [read]
- **C9** (accepted, D64; look again on 3.12): compiled code doesn't own a reference per value-stack slot, so C code testing `Py_REFCNT(x) == 1` can behave differently; fixed where seen (`**kwargs` in `CALL_FUNCTION_EX`). SETUP_WITH's manager holds one reference more while `__enter__` runs. [run]
- **C11** (partly fixed): compiled frames are only partly visible. Missing: C code walking `tstate->frame` skips compiled callees (`warnings` stacklevel, `PyEval_GetFrame`/`PyEval_GetGlobals`, so `type()`'s `__module__`); locals and line numbers in emulated frames and in real frames running compiled code (`f_lasti` never updated); a real frame's `f_back` skips compiled callers. [run]
- **C14** (partly fixed; to fix on 3.7): compiled code is rarely freed. Still leaking: code buffers and states referenced only from machine code (promotion caches, records of stubs that never fire, the stub pool), Python objects in compile-time constants (`vcompiler.c:sk_release`), and machine code from a bump allocator. [read], [run]
- **C15** (remainder; to fix on 3.7): code loading `locals`, `vars`, `dir`, `eval`, `exec` as globals isn't compiled (`mergepoints.c:_build_merge_points`), but the same builtins reached another way (`f = locals; f()`, `builtins.eval`) and `sys._getframe(n).f_locals` of a compiled frame see only the arguments. [run]
- **C17** (remainder accepted, D64): virtual floats, ints and bound methods stay virtual across a compiled call, so `f(x) is x` can be False for them (tuples are forced, `NW_TUPLES_FUNCALL`). [run]
- **C18** (open; to fix on 3.7): a traceback doesn't keep a compiled function's locals alive, because they live in registers and Ni's stack, not `f_localsplus`. Same root as C11. [run]
- **C19** (accepted, by design, D64): `sys.getrefcount` sees one reference fewer for an argument compiled code passes to C (borrowed); a constant's count includes one reference per block that uses it. [run]
- **C20** (accepted, D64): `len(gc.get_objects())` can grow by one around code compiled at that moment (a GC-tracked object per compile; not a leak). [run]
- **C29** (open): a frame already running compiled when tracing or profiling starts gets no events (C23, D46 test only at entry): a tracer setting `f_trace` on running frames, `pdb.set_trace()`/`breakpoint()` on its own next line, a profiler's `c_call`/`return`. Needs a way out of compiled code mid-frame. [run]

### Generator
From the review against `doc/GENERATOR_DESIGN.md` (2026-11-18). Most are latent: no generated header shows them today. The redesign removes them by construction; until then, a change that reaches one must fix it.
- **G5**: `ceval_reader.cvalue` evaluates C with Python's semantics: `oparg && 4` gives 4, `!oparg + 1` gives 0 at `oparg = 0`, `1 < oparg < 3` chains. It feeds stack-effect sizes. [run]
- **G6** (latent): a resume inside a meta loop loses the remaining iterations; only `Walk.statement` checks `loop_ctx`, not `slot`, `subtype`, `known_type`, `load_word`, `short_circuit`, `construct` or `choice`. [read]
- **G7** (latent): `Walk.expr_stmt` never emits a discarded `okv` (bool-returning call) result, and leaks an owned `rt` temporary with no convention; `compare` never releases owned operands. [read]
- **G8** (latent): `Walk.raise_` doesn't set `st.failed`, so residual calls on an error path aren't refused. [read]
- **G9** (latent): `Walk.statement`'s rollback doesn't restore `used_likely`, `needs`, `kl_rep`, `any_call` or `_wcount`. [read]
- **G10**: `('other', 'mutable_fields')` is in `gen_abstract.IMPLEMENTED`, but nothing implements it. [read]
- **G11** (latent): `gen_builtins.Walk.compare` evaluates its operand twice and doesn't undo state from the first (a deleted `Psyco_NeedType` line leaves its variable unset). [read]
- **G12**: scalar helpers (`scalars.call_helper`) never get `CfNoBlock`, and raising ones not `CfNoPython`; a known function pointer in a run is accepted unchecked; errno is assumed to stay within one helper. [read]
- **G13** (latent): `scalars` and `ceval_inline` inline by substituting names or argument text, not values: a local copied from `*p` sees a later `*p = …`, and an argument with a call is evaluated once per use. [read]
- **G14** (latent): `gen_virtual` treats a store under a condition as unconditional, and misses a NULL default written another way (`if (!step)`). [read]
- **G15** (latent): `ceval_inline.Inliner.switch` loses a switch arm's fall-through when another arm returns. [read]

## Fixed, still cited

### Memory access
- **M1–M6, M9** (`v_mem_mov` encodings, one commit; `git log --grep v_mem_mov`): M1 signed byte/word loads didn't sign-extend; M2 0x66 after REX; M3 byte stores from RBP/RSI/RDI without REX; M4 RIP-relative immediate stores missed the immediate's length; M5 unsigned loads flagged non-negative; M6 (by reading) compile-time reads of 4-byte fields as `long`; M9 negative constants forced through R10.
- **M8** guarded (`f53ef68`): a virtual `base`/`index` would be dereferenced as compile-time; `v_mem_mov` `extra_assert`s it.
- **M10** (`df37def`, F33; generated since D89): struct members read at the wrong width.

### Front end
- **F1** (`4cf49f8`): mixed-type arithmetic raised TypeError (Py2 `binary_op1`).
- **F2** (`5976d48`): no builtin metas registered (`"__builtin__"`).
- **F3** (`ebc5cfc`, `295961f`, `ec836d0`): Py2 opcode meanings (`RAISE_VARARGS`, `BUILD_MAP`, `CALL_FUNCTION*`).
- **F5** (`9ed7e6c`, `f217df0`): float division by zero; `math` errors and int-valued `floor`/`ceil`.
- **F6** (`07fbb31`, by reading, unreachable): `direct_compute_float` converted the stored word numerically instead of bit-casting.
- **F7** (`06e7922`): `compute_function` stored function fields with raw puts.
- **F8** (`0b46284`, D58): calls to closures were the generic call. Still the real call: `**kwargs` callees, keyword-only (F17) and callees with cells of their own.
- **F9** (`3e8e07d`): `T_STRING_INPLACE` members gave bytes.
- **F10** (`0e191a3`): a user `__eq__`/`__ne__` ran twice.
- **F11** (`351a4d5`): dead `PsycoEval_CallObjectWithKeywords` removed.
- **F13** (`17b2a04`, `cec9770`, `9a70e7e`, D36): METH_FASTCALL builtins without a meta were residual `PyCFunction_Call`s. Direct C calls still skip `Py_EnterRecursiveCall`.
- **F17** (`0d3e280`): calls to functions with keyword-only arguments lost `__kwdefaults__`; such calls use `tp_call`.
- **F18 (1)** (`fafd71e`): a bare `raise` added a traceback entry.
- **F19** (`ec836d0`, `f9000ff`, `7fdf9a7`): keyword and star calls and the `BUILD_*_UNPACK` opcodes weren't compiled.
- **F20** (`12b7e4e`): a finally block entered by an exception didn't make it the handled one.
- **F21** (`0fae193`): Py2 NameError wording.
- **F22** (`20dea59`): `+x is x` False for an exact float.
- **F23** (`cf54fed`): no METH_FASTCALL case in `PsycoCFunction_Call`.
- **F24** (`0feefaf`): `PsycoObject_RichCompareBool` promoted a temporary's type.
- **F25** (`96aaec2`, `d0821ce`): `isinstance` and `type_call` metas promoted heap classes, keeping them alive; now `psyco_atcompiletime_static_type`.
- **F26** (`1c06812`): a compile-time exception with a NULL value segfaulted at the re-raise.
- **F27** (`50318dd`): early allocation failures in the merge-point analysis and frame capsules crashed (the rest is open, above).
- **F28** (`11bcf61`): Py2 TypeError messages in the abstract layer.
- **F30** (`57700fd`): Py2 UNPACK_SEQUENCE messages.
- **F31** (`e657847`): `pr.changing_globals` wasn't saved in snapshots, so a respawn replay diverged.
- **F32** (`24a3af9`): `int` varargs to `psyco_generic_call`; its macro now rejects narrow arguments.
- **F33** (`df37def`): `T_INT`/`T_UINT` members read as 8 bytes.
- **F35** (`125cf3d`): `psyco_unfix` cleared `SkFlagPyObj` on a constant snapshots share.
- **F37** (`e86f8ae`): a `__setattr__` added to a class later was ignored by compiled stores.
- **F39** (`2f6eeef`): no PEP 560 `__class_getitem__` step.
- **F40** (`2f6eeef`): `PsycoTuple_Concat` returned a tuple subclass operand.
- **F41** (`4e6c171`): `super().__getitem__` in a list subclass recursed.
- **F42** (`4d13753`): F39's step unreached for a class with a Python metaclass.
- **F43** (`bfc8d60`): `except` didn't check it names exception classes.
- **F44** (`ba6c878`): IMPORT_FROM's Py2 message and missing `sys.modules` fallback.
- **F45** (`b4253fe`): `f(x, *GLOBAL_LIST)` crashed (`PsycoTuple_Load` took any constant for a tuple).
- **F46** (`0d6ee48`): `print >> f` lacked 3.7's hint.
- **F47** (`0da5849`): `globals()` returned the nearest real frame's globals.
- **F48** (`8f11eee`): an exhausted sequence iterator restarted when the sequence grew.
- **F49** (`5f8b506`, D52): builtins read from `globals()['__builtins__']` at each lookup (rest: F72).
- **F50** (`50b6a3c`, `995a3ce`, D48): a heap type's compare, hash and iteration slots were folded (rest: F56).
- **F51** (`125cf3d`): checked build only: a respawn replay saw a new constant tuple (F35 made visible).
- **F52** (`11a867b`, `9217bdb`, D48): a function's changed `__defaults__` wasn't seen.
- **F54** (`80a60ce`): a store's value was made after the type guard.
- **F55** (`56e5faf`, D59, D63): a promoted type was kept after a call assigned `__class__`. Left: a finaliser run by a collection that a `CfPure` allocation starts.
- **F56** (`d16ab6b`…`4a0d275`, D54): a heap type's other slots were folded.
- **F57** (`eca1509`, cost accepted D64): a function's changed `__code__` wasn't seen.
- **F80** (`8e32559`): `%=`'s message said `%`.
- **F92** (`0b46284`): wrong argument counts raised a Py2 message.
- **F96** (`3116d30`, D62): a constant's type was read at compile time and never checked. Left: the check chain is unbounded for an object given ever new classes.
- **F98** (`fa3dae7`, D88): FORMAT_VALUE asserted an exact str result.
- **F99** (`cddaf8d`, D87): `math` functions ignored an int subclass's `__float__`.
- **F101** (`1aa473c`, D74): an in-place operator could run `__iadd__` twice (restart rule).
- **F102** (`1aa473c`, D74, D78): a huge int index raised OverflowError, not IndexError.
- **F104** (`28ff76f`, D83): `pfloat_richcompare`'s int fallback didn't check NULL.
- **F106** (D78): a respawn mishandled a condition still in the flags.
- **F107** (D80): an `NI_CODESTATS` build read the PsycoObject after freeing it.
- **F108** (D80): `t = t + (i,)` in a loop ran the compiler out of memory.
- **F109** (D84): `PsycoObject_RichCompareBool` had no identity shortcut (a virtual is identical only to itself).
- **F110** (`fa3dae7`, D88): `PsycoObject_Repr` asserted an exact str.
- **F111** (`caa579d`): `gen_abstract` folded a double function's `return -1.0` after an error.
- **F112** (`96eeba2`): `gen_ceval`'s label copies failed on 3.8 (`tstate` first).
- **F113** (`24dd41d`): `gen_virtual` read a header's static inline function without the calling file's macros.
- **F115** (`962af09`): a 3.8 error handler's run-time copy kept `_PyErr_X(tstate, …)`.

### Backend
- **B1** (`d56791d`): `CMP_I8_O`/`CMP_I8_A` compared 32 bits.
- **B2** (`36c7fdc`): `PUSH RAX` in result checks without updating `stack_depth`.
- **B3** (`e86d7ef`): `run_time_call` put an integer after a double in the wrong register.
- **B5** (`1e65d32`): rel32 jumps assumed every code buffer within ±2 GB.
- **B6** (`085392a`): `STACK_DEPTH_SINCE_CALL` was -1 when inlining.
- **B7** (`666c209`): `psyco_unify` mishandled a value wanted in two registers.
- **B8** (`457c216`): a coding pause's proxy clobbered the flags.
- **B9** (`2286ac5`): backend `assert()`s compiled out, some wrong; now `extra_assert`s.
- **B14** (`98f71df`): the PyErr_Occurred check's push let the allocator spill in between.
- **B16** (`29fb0a4`): `STACK_CORRECTION` clobbered the flags.

### Core
- **C5** (`ad3cf79`): globals defined without `extern` (gcc 10+).
- **C7** (`1f46b4b`): `psyco_assert_coherent` didn't count `po->pr.tb`.
- **C12** (`c4c4ee3`, `1de8b28`): compiled code didn't count recursion depth. Not counted: inlined calls, and CPython's own checks inside operations Ni inlines.
- **C13** (`e4f21aa`): per-code dicts keyed by code objects compared by value; now `co_extra`.
- **C22** (`c0f5895`): another PEP 523 user's code object crashed `code_extra_free`.
- **C23** (`5f1dbf6`, `cf692d5`, D46): tracing and profiling didn't see compiled code (rest: C29).
- **C24** (`e26c2a3`, D45, D53): compiled code never tested `eval_breaker`. A signal or async exception can still arrive a few instructions later than under ceval.
- **C25** (`3173369`): direct C calls skipped `_Py_CheckFunctionResult`.
- **C26** (`ff1bce8`): the stub pool assumed 4 KB left in a reused code buffer.
- **C27** (`a4b946e`): `psyco_unify` decref'd dropped values after moving kept ones.
- **C28** (`261fd83`): `canonical_int` rewrote a vinfo held by something else in place.
- **C30** (`63b4f95`): an inlined frame named its caller by a borrowed code object, reused at a dead one's address.
- **C31** (`5a39986`): the alignment test (C1) compared the wrong slot.
- **C35** (`2063657`): `IS_A_JUMP` read a `JMP rel32` one byte late.
- **C39** (`6192ed6`): `eval_breaker.py`'s async-exception case was timing-dependent.
- **C49** (`4d89eb7`, `a664895`): inlining a closure whose cells were loaded at run time crashed.
- **C53** (`6cd0c26`): `heap_setattr` set the store guard after `compute_vinfo`.
- **C54** (`a599bd2`, D66): a static-type promotion site counted its megamorphic version as a value.
- **C55** (`c33b13f`): `NI_ALL_STATIC=1` didn't compile.
