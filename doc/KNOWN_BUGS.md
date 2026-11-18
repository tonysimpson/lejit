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
- **F27** (remainder): running out of memory mid-compile abandons the compile (D103), but a compile entered from compiled code (a pause, respawn or promotion) that runs out on every one of its tries still aborts, as does `OUT_OF_MEMORY` outside any compile. [read], [run]
- **F34** (accepted): instance-dict lookups by an exact str name are called `CfNoPython` (`pobject.c`, `cf_flags.h`; `policy_table.py` `ASSUMPTIONS` for `PyDict_GetItem`), so a str-subclass key whose `__eq__` changes the class during the lookup isn't seen. [read]
- **F36** (live): holding a class's shared keys (`pobject.c:shared_key_slot`, D31) can stop CPython re-sharing them when the class's only live instance grows its keys after a compile (`_PyObjectDict_SetItem` re-shares only if the class holds the sole reference). Results stay right; that class's instances use ordinary dicts (go's `Board`). Fixes tried cost more than the bug. [read], [run]
- **F72** (accepted, D64): a direct compiled call into another module reads that module's `__builtins__` when the call site compiles, where CPython reads it at each frame start (`psyfunc.c:callee_builtins`). Exact would need a guard at every cross-module call. [inferred]
- **F105** (out of memory only): `pycompiler.c:PsycoTraceBack_Here` calls `cimpl_vt_traceback` as `CfCommonNewRefPyObjectNoError`, but when `PyFrame_New` or `PyTraceBack_Here` fails it returns `Py_None` with a MemoryError still set. [read]

### Backend
- **B10** (environment): executable memory is mapped RWX (no W^X); hardened kernels may refuse it. [read]
- **B17** (dormant): a function with exception handlers still reserves three stack words in its prologue (`codegen.c:psyco_emit_header`, `INITIALIZE_FRAME_LOCALS`) that nothing reads; a push and a stack correction per entry. Removing them changes `LOC_CONTINUATION`'s array and the return's stack arithmetic. [read]

### Core
- **C1** (cost): `dispatcher.c:compare_return_address` rejects a match whose return-address slot differs mod 16, which is needed (call alignment derives from it) but duplicates some blocks (1.8% over 23 benchmarks). Fixing it means aligning at calls instead. [read], [run]
- **C9** (accepted, D64; look again on 3.12): compiled code doesn't own a reference per value-stack slot, so C code testing `Py_REFCNT(x) == 1` can behave differently; fixed where seen (`**kwargs` in `CALL_FUNCTION_EX`). SETUP_WITH's manager holds one reference more while `__enter__` runs. [run]
- **C11** (partly fixed, D119): compiled frames are only partly visible. Missing: a real frame entered through C from compiled code (a keyword call, a class's `__init__`, a generator) has an `f_back` that skips the compiled frames between, so `logging`'s `%(funcName)s` can name the wrong caller and a traceback frame's `f_back` is wrong (`frames.c:linked_frame`, `tests/cases/frames_seen_from_c.py`); C code other than `_warnings.warn` walking `tstate->frame` from a compiled callee skips it (`PyEval_GetFrame`/`PyEval_GetGlobals`; a freshly imported `_warnings`); locals in emulated frames; the line of a compiled frame stopped in a call outside a `CALL_*` (an operator's or attribute's Python method) or in an inlined frame, which is its first line; a real frame running compiled code keeps `f_lasti` -1, so its first line; emulated frames are new objects each time. [run] Other projects' suites hit it (pyparsing, Django, sympy; FINDINGS.md), measured before the line-number part of the fix landed.
- **C14** (remainder): compiled code is freed with its code object (D102), cycles back to a code object through what its code holds are collected at a full collection or once enough records are made (D122), and guard records die with the last code that used them; but live compiled code holds what it was compiled with (constants, the classes it promoted, the methods it looked up), so they die only with that code (`test_weakref`, `test_gc`). So code shared by classes made per call grows with every class while it lives: namedtuple's `_make` and `__repr__` have `Psyco_NeedType` promotion sites with no megamorphic cut-off and compile a block for each new class (about 15 KB per namedtuple). Code compiled while no owner is current is kept for good (`ni._stats()['code_live']`'s second count). [run]
- **C15** (remainder; to fix on 3.7): code loading `locals`, `vars`, `dir`, `eval`, `exec` as globals isn't compiled (`mergepoints.c:_build_merge_points`), but the same builtins reached another way (`f = locals; f()`, `builtins.eval`) and `sys._getframe(n).f_locals` of a compiled frame see only the arguments. [run]
- **C17** (remainder accepted, D64): virtual floats, ints and bound methods stay virtual across a compiled call, so `f(x) is x` can be False for them (tuples are forced, `NW_TUPLES_FUNCALL`). [run]
- **C18** (partly fixed, D118): a compiled frame's locals go into its traceback frame when an exception passes (`pycompiler.c:traceback_locals`). Left: a local the liveness analysis (D15) dropped before the raise is missing there, so `tb_frame.f_locals` lacks it and the traceback doesn't keep it alive. [run]
- **C19** (accepted, by design, D64): `sys.getrefcount` sees one reference fewer for an argument compiled code passes to C (borrowed); a constant's count includes one reference per block that uses it. [run]
- **C20** (accepted, D64): `len(gc.get_objects())` can grow by a few around code compiled at that moment (GC-tracked objects per compile, 7 for a class's first `__init__`; not a leak; sortedcontainers' `test_ref_counts`). [run]
- **C29** (partly fixed, D117): a frame already running compiled code when tracing or profiling starts goes on interpreted after its next call (`pycompiler.c:resume_if_tracing`). Left: `f_trace` set on a compiled callee's frame is lost, since that frame is emulated, a new object each time (C11), so `pdb.set_trace()` in a compiled callee stops only at the next call event; tracing started from an operator's or attribute's Python method is seen at the frame's next call; locals the liveness analysis dropped are unbound in the resumed frame. [run]
- **C57** (live, CPython 3.7): a code object with a Ni record that a subinterpreter's collection frees calls that interpreter's `co_extra` free functions, which `PyInterpreterState_New` leaves uninitialised, and can crash. The owner scan takes the record off code it finds dead first (`ni.c:ni_code_extra_detach`), but code that became garbage after the last scan isn't covered (a crash in `tests/cases/freeing.py:freed_in_subinterpreter` before it cleared its other garbage). [run]
- **C60** (live): `tracemalloc` counts the compiler's own allocations, so the memory traced around a function's first call is larger: about 4 kB in more-itertools' `test_memory_in_order` (`tests/cases/thirdparty_found.py:traced_peak`). [run]
- **C61** (cost): compiled code and what Ni keeps for each code buffer take far more memory than the program: about 4.6 kB of RSS per buffer, of which some 300 bytes are code. sympy's tests (9 packages) reach 3 GB by `simplify/tests/test_hyperexpand.py` (plainly 250 MB) and at the 4 GB limit abort in a compile (F27's remainder; `tools/thirdparty`). Repeating the same work compiles almost nothing more, so it is the amount of distinct code, not a runaway. [run]

### Generator
From the review against `doc/GENERATOR_DESIGN.md` (2026-11-18). Most are latent: no generated header shows them today. The redesign removes them by construction; until then, a change that reaches one must fix it.
- **G5**: `ceval_reader.cvalue` evaluates C with Python's semantics: `oparg && 4` gives 4, `!oparg + 1` gives 0 at `oparg = 0`, `1 < oparg < 3` chains. It feeds stack-effect sizes. [run]
- **G7** (latent): `gen_abstract.Walk.expr_stmt` never emits a discarded `okv` (bool-returning call) result, and leaks an owned `rt` temporary with no convention; `compare` never releases owned operands. The abstract family, now on gen2, doesn't have it (`tests/gen2_snippets.py`). [read] [run]
- **G8** (latent): `Walk.raise_` doesn't set `st.failed`, so residual calls on an error path aren't refused. The abstract family, now on gen2, doesn't have it (`tests/gen2_snippets.py`). [read] [run]
- **G9** (latent): `Walk.statement`'s rollback doesn't restore `used_likely`, `needs`, `any_call` or `_wcount`. The abstract family, now on gen2, doesn't have it (`tests/gen2_snippets.py`). [read] [run]
- **G10**: `('other', 'mutable_fields')` is in `gen_abstract.IMPLEMENTED`, but nothing implements it. The abstract family, now on gen2, doesn't have it (`tests/gen2_snippets.py`). [read] [run]
- **G12** (latent): scalar helpers (`scalars.call_helper`) never get `CfNoBlock`, and raising ones not `CfNoPython`; a known function pointer in a run is accepted unchecked; errno is assumed to stay within one helper. No unit makes a helper through `scalars` now (gen_abstract's units read doubles but copy no run); gen2's (`gen2/scalar.py`: the float slots and math) have none of these (`tests/gen2_snippets.py`). [read] [run]
- **G13** (latent): `scalars` and `ceval_inline` inline by substituting names or argument text, not values: a local copied from `*p` sees a later `*p = …`, and an argument with a call is evaluated once per use. gen2's `inline.py` inlines by value (`tests/gen2_snippets.py`). [read] [run]
- **G14** (latent): `gen_virtual` treats a store under a condition as unconditional, and misses a NULL default written another way (`if (!step)`). gen2's tuple stores are emitted where the C makes them (`tests/gen2_snippets.py`). [read] [run]
- **G15** (latent): `ceval_inline.Inliner.switch` loses a switch arm's fall-through when another arm returns. gen2 refuses an arm that falls into the next (`tests/gen2_snippets.py`). [read] [run]

## Fixed, still cited

### Memory access
- **M1–M6, M9** (`v_mem_mov` encodings, one commit; `git log --grep v_mem_mov`): M1 signed byte/word loads didn't sign-extend; M2 0x66 after REX; M3 byte stores from RBP/RSI/RDI without REX; M4 RIP-relative immediate stores missed the immediate's length; M5 unsigned loads flagged non-negative; M6 (by reading) compile-time reads of 4-byte fields as `long`; M9 negative constants forced through R10.
- **M8** guarded (`f53ef68`): a virtual `base`/`index` would be dereferenced as compile-time; `v_mem_mov` `extra_assert`s it.

### Front end
- **F2** (`5976d48`): no builtin metas registered (`"__builtin__"`).
- **F6** (`07fbb31`, by reading, unreachable): `direct_compute_float` converted the stored word numerically instead of bit-casting.
- **F7** (`06e7922`): `compute_function` stored function fields with raw puts.
- **F8** (`0b46284`, D58): calls to closures were the generic call. Still the real call: `**kwargs` callees, keyword-only (F17) and callees with cells of their own.
- **F11** (`351a4d5`): dead `PsycoEval_CallObjectWithKeywords` removed.
- **F13** (`17b2a04`, `cec9770`, `9a70e7e`, D36): METH_FASTCALL builtins without a meta were residual `PyCFunction_Call`s. Direct C calls still skip `Py_EnterRecursiveCall`.
- **F17** (`0d3e280`): calls to functions with keyword-only arguments lost `__kwdefaults__`; such calls use `tp_call`.
- **F18 (1)** (`fafd71e`): a bare `raise` added a traceback entry.
- **F23** (`cf54fed`): no METH_FASTCALL case in `PsycoCFunction_Call`.
- **F24** (`0feefaf`): `PsycoObject_RichCompareBool` promoted a temporary's type.
- **F25** (`96aaec2`, `d0821ce`): `isinstance` and `type_call` metas promoted heap classes, keeping them alive; now `psyco_atcompiletime_static_type`.
- **F26** (`1c06812`): a compile-time exception with a NULL value segfaulted at the re-raise.
- **F27** (`50318dd`, and compile guards, D103): allocation failures in the merge-point analysis and frame capsules crashed, and mid-compile ones aborted (the remainder is open, above).
- **F31** (`e657847`): `pr.changing_globals` wasn't saved in snapshots, so a respawn replay diverged.
- **F32** (`24a3af9`): `int` varargs to `psyco_generic_call`; its macro now rejects narrow arguments.
- **F33** (`df37def`): `T_INT`/`T_UINT` members read as 8 bytes.
- **F35** (`125cf3d`): `psyco_unfix` cleared `SkFlagPyObj` on a constant snapshots share.
- **F37** (`e86f8ae`): a `__setattr__` added to a class later was ignored by compiled stores.
- **F39** (`2f6eeef`): no PEP 560 `__class_getitem__` step.
- **F40** (`2f6eeef`): `PsycoTuple_Concat` returned a tuple subclass operand.
- **F42** (`4d13753`): F39's step unreached for a class with a Python metaclass.
- **F45** (`b4253fe`): `f(x, *GLOBAL_LIST)` crashed (`PsycoTuple_Load` took any constant for a tuple).
- **F47** (`0da5849`): `globals()` returned the nearest real frame's globals.
- **F48** (`8f11eee`): an exhausted sequence iterator restarted when the sequence grew.
- **F49** (`5f8b506`, D52): builtins read from `globals()['__builtins__']` at each lookup (rest: F72).
- **F50** (`50b6a3c`, `995a3ce`, D48): a heap type's compare, hash and iteration slots were folded (rest: F56).
- **F52** (`11a867b`, `9217bdb`, D48): a function's changed `__defaults__` wasn't seen.
- **F54** (`80a60ce`): a store's value was made after the type guard.
- **F55** (`56e5faf`, D59, D63): a promoted type was kept after a call assigned `__class__`. Left: a finaliser run by a collection that a `CfPure` allocation starts.
- **F56** (`d16ab6b`…`4a0d275`, D54): a heap type's other slots were folded.
- **F57** (`eca1509`, cost accepted D64): a function's changed `__code__` wasn't seen.
- **F92** (`0b46284`): wrong argument counts raised a Py2 message.
- **F96** (`3116d30`, D62): a constant's type was read at compile time and never checked. Left: the check chain is unbounded for an object given ever new classes.
- **F99** (`cddaf8d`, D87): `math` functions ignored an int subclass's `__float__`.
- **F101** (`1aa473c`, D74): an in-place operator could run `__iadd__` twice (restart rule).
- **F104** (`28ff76f`, D83): `pfloat_richcompare`'s int fallback didn't check NULL.
- **F106** (D78): a respawn mishandled a condition still in the flags.
- **F107** (D80): an `NI_CODESTATS` build read the PsycoObject after freeing it.
- **F108** (D80): `t = t + (i,)` in a loop ran the compiler out of memory.
- **F109** (D84): `PsycoObject_RichCompareBool` had no identity shortcut (a virtual is identical only to itself).
- **F111** (`caa579d`): `gen_abstract` folded a double function's `return -1.0` after an error.
- **F112** (`96eeba2`): `gen_ceval`'s label copies failed on 3.8 (`tstate` first).
- **F113** (`24dd41d`): `gen_virtual` read a header's static inline function without the calling file's macros.
- **F115** (`962af09`): a 3.8 error handler's run-time copy kept `_PyErr_X(tstate, …)`.

### Generator
- **G11** (`127d4d6`): `gen_builtins.Walk.compare` evaluated its operand twice; gen_builtins is gone, and gen2 evaluates it once (`tests/gen2_snippets.py`).

### Backend
- **B1** (`d56791d`): `CMP_I8_O`/`CMP_I8_A` compared 32 bits.
- **B2** (`36c7fdc`): `PUSH RAX` in result checks without updating `stack_depth`.
- **B3** (`e86d7ef`): `run_time_call` put an integer after a double in the wrong register.
- **B5** (`1e65d32`): rel32 jumps assumed every code buffer within ±2 GB.
- **B6** (`085392a`): `STACK_DEPTH_SINCE_CALL` was -1 when inlining.
- **B7** (`666c209`): `psyco_unify` mishandled a value wanted in two registers.
- **B8** (`457c216`): a coding pause's proxy clobbered the flags.
- **B9** (`2286ac5`): backend `assert()`s compiled out, some wrong; now `extra_assert`s.

### Core
- **C5** (`ad3cf79`): globals defined without `extern` (gcc 10+).
- **C7** (`1f46b4b`): `psyco_assert_coherent` didn't count `po->pr.tb`.
- **C12** (`c4c4ee3`, `1de8b28`): compiled code didn't count recursion depth. Not counted: inlined calls, and CPython's own checks inside operations Ni inlines.
- **C22** (`c0f5895`): another PEP 523 user's code object crashed `code_extra_free`.
- **C23** (`5f1dbf6`, `cf692d5`, D46): tracing and profiling didn't see compiled code (rest: C29).
- **C24** (`e26c2a3`, D45, D53): compiled code never tested `eval_breaker`. A signal or async exception can still arrive a few instructions later than under ceval.
- **C26** (`ff1bce8`): the stub pool assumed 4 KB left in a reused code buffer.
- **C27** (`a4b946e`): `psyco_unify` decref'd dropped values after moving kept ones.
- **C28** (`261fd83`): `canonical_int` rewrote a vinfo held by something else in place.
- **C31** (`5a39986`): the alignment test (C1) compared the wrong slot.
- **C35** (`2063657`): `IS_A_JUMP` read a `JMP rel32` one byte late.
- **C49** (`4d89eb7`, `a664895`): inlining a closure whose cells were loaded at run time crashed.
- **C53** (`6cd0c26`): `heap_setattr` set the store guard after `compute_vinfo`.
- **C54** (`a599bd2`, D66): a static-type promotion site counted its megamorphic version as a value.
- **C55** (`c33b13f`): `NI_ALL_STATIC=1` didn't compile.
