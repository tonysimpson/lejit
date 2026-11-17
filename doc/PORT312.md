---
written: 2026-11-17
status: feasibility study; nothing ported
---
# Porting Ni's core to CPython 3.12: feasibility

The question (agreed after the bytecode desk read, METACOMPILER.md): the meta-compiler side looks tractable on 3.12, so is porting the *core* feasible? That means how compiled code is entered, frames, exceptions, the bytecode front end, and object layouts. Sources: the 3.12.9 source tarball, the installed pyenv 3.12.9 (headers and `libpython3.12.so`), and a prototype extension, `tools/proto312/` (`build.sh` builds and runs it).

**Answer: feasible, with one change of design.** Enter compiled code through `PyFunction_SetVectorcall`, not the PEP 523 frame-evaluation hook Ni uses on 3.7. Everything Ni needs from CPython is then either public API or a struct field in the installed internal headers. None of it is a hidden symbol. [run] for the entry and tracebacks, [read] for the rest.

## Entering compiled code

**The 3.7 way doesn't carry over.** On 3.7, `eval_hook.c:NiEval_EvalFrame` receives a full `PyFrameObject` and runs compiled code with `f_localsplus` as its argument words. CPython then clears and frees the frame. On 3.12 the hook receives a `_PyInterpreterFrame` that has already been pushed, and whoever evaluates it must clear and pop it. [read] (`ceval.c:_PyEvalFrameClearAndPop`, `clear_thread_frame`)
- The functions for that are `_PyFrame_ClearExceptCode`, `_PyThreadState_PopFrame` and `_PyThreadState_PushFrame`. They are local symbols in `libpython3.12.so` (`nm`: `t`), so an extension can't call them. [run]
- A hook that runs frames itself must copy CPython's internal frame code and keep it in step with each release. I believe that is what PyTorch's Dynamo does (`cpython_defs.c`). [inferred, from memory]

**The 3.12 way: per-function vectorcall.** `PyFunction_SetVectorcall` is exported (`cpython/funcobject.h`).
- A function whose `vectorcall` isn't `_PyFunction_Vectorcall` never gets a `func_version`, so the specialised calls (`CALL_PY_EXACT_ARGS`, `CALL_BOUND_METHOD_EXACT_ARGS`…) can never bypass it. [read] (`funcobject.c:_PyFunction_GetVersionForCurrentState`; `bytecodes.c:CALL`, which inlines a Python call only `if (... vectorcall == _PyFunction_Vectorcall)`)
- No frame is pushed. The arguments arrive as `PyObject *const *args`: the borrowed argument words that `psyco_processor_run` already takes (`vcompiler.c:PsycoObject_FromCode` treats them as borrowed stack words).
- Anything Ni doesn't handle (keywords, `*args`, the wrong arity, generators) falls back to `_PyFunction_Vectorcall`, which is exported.
- This is upstream Psyco's own design. A proxy replaced the function and called compiled code directly; `psyfunc.c` still carries it as dead code.

**Prototype** (`tools/proto312/proto.c`). The "compiled body" of a two-argument function is `PyNumber_Add`. [run]
- **Call routes:** a hot loop of 100,000 calls from interpreted code reached the compiled body every time, after specialisation too. So did calls as a method, through `map` and through `functools.partial`. `f(*t)` stays compiled; keyword calls fall back.
- **No leaks** over 500,000 calls (one block of growth). It runs at the same speed as the interpreted function for this one-operation body.
- **Tracebacks work** as Ni builds them now (`pycompiler.c:cimpl_rt_traceback`): `PyFrame_New`, then `PyTraceBack_Here`. The frame's `f_frame->prev_instr` (an internal field) must point at the failing instruction. The traceback then shows `f`, line 5.
- **Specialisation rewrites the bytecode in place.** The first prototype found its `BINARY_OP` gone once the interpreter had run `f`: it had become `BINARY_OP_ADD_INT` inside the code object. Ni must read bytecode with `PyCode_GetCode`, which returns it unspecialised at the same offsets. It must never read `co_code_adaptive`.
- **Changing `__code__` kept the old compiled body** (7 instead of 12). An exported function watcher (`PyFunction_AddWatcher`, on any event except creation) fixes that: it restores `_PyFunction_Vectorcall`. The `PyFunction_EVENT_CREATE` event could also install Ni's vectorcall on every new function. That would replace the 3.7 hook's "see every call" without touching the interpreter.
- **Tracing:** `sys.setprofile` doesn't see compiled functions. On 3.7 Ni now runs interpreted while tracing: the eval hook, and a `use_tracing` test at each compiled call and inlined frame (D46, KNOWN_BUGS C23). On 3.12 the same applies to `sys.monitoring` (PEP 669; QUESTIONS 32).

## The rest of the core, area by area

| Area | Ni on 3.7 | 3.12 | Work |
|---|---|---|---|
| Frames seen by Python | Compiled callees have no frames; `ni_getframe` emulates them; `tstate->frame` is set while compiled code runs (`eval_hook.c:NiCode_Run`) | The chain is `tstate->cframe->current_frame` (`_PyInterpreterFrame.previous`). With vectorcall, compiled frames are invisible, as compiled callees are now | Port `frames.c` emulation; drop the `tstate->frame` juggling. [read] |
| Exceptions (state) | `tstate->curexc_type/value/traceback`, `exc_info` triples, `f_exc_*` | One `tstate->current_exception`; `PyErr_GetRaisedException`/`SetRaisedException`; `exc_info` holds only the value | About 15 sites (`grep curexc_`, `exc_info`). Mechanical. [read] |
| Exceptions (handlers) | Block stack (`SETUP_*`, `unwind_except_handler`, D2) | No block stack: handlers come from `co_exceptiontable` by instruction offset (`ceval.c:get_exception_handler`) | Rewrite the unwinding in `pycompiler.c`. The table is static, so at compile time the handler for any offset is a lookup: probably simpler than the block stack. [inferred] |
| Bytecode | 3.7 opcodes; `mergepoints.c` analyses `co_code` | Different opcodes (`CALL`/`KW_NAMES`, `LOAD_ATTR` method bit, `RETURN_CONST`, superinstructions), inline `CACHE` entries, specialisation rewriting | Front end from `bytecodes.c` (`gen_ceval.py`, D26), with array inputs, conditional outputs and run-time branches added (METACOMPILER). Read bytecode with `PyCode_GetCode`; cache sizes are in the DSL (`unused/1`). `mergepoints.c` needs the new jump encoding. |
| Code objects | `co_nlocals`, `co_cellvars`, `co_freevars`, `co_varnames`, `co_code` | `co_nlocalsplus`, `co_localsplusnames`/`kinds` (cells and frees inside localsplus), `co_code` is a getter, `co_framesize` | Mechanical. [read] (`cpython/code.h`) |
| Ints | One-digit fast paths reading `ob_size` and `ob_digit[0]` (`plong_medium_value`) | `long_value.lv_tag` (digit count, sign, flags); "compact" values; `_PyLong_IsCompact`/`_PyLong_CompactValue` are inline | Rewrite the representation layer. 3.12's `longobject.c` has explicit compact fast paths (`_PyLong_BothAreCompact`), which suits `gen_long.py` better than 3.7's did (METACOMPILER failures log, `long_richcompare`). [read] |
| Refcounts | Emitted `inc`/`dec` on the 64-bit `ob_refcnt` | Immortal objects (PEP 683): `ob_refcnt` = `UINT_MAX`, `Py_INCREF` is a saturating add on the low 32 bits | A 64-bit `inc` on `None` gives 2^32, which looks mortal until the next decrement brings it back. It never reaches zero, so this is safe but untidy. Better: emit no refcounting for objects known to be immortal at compile time (`None`, `True`, small ints), which also saves work. [read], [inferred] |
| Cached globals (D25) | `ma_version_tag` | Deprecated (PEP 699); dict watchers (`PyDict_AddWatcher`, exported) | QUESTIONS 29. [read] |
| Recursion | `Py_EnterRecursiveCall`; `recursion_depth` read in two places | `Py_EnterRecursiveCall` stays; the depth fields became `py_recursion_remaining`/`c_recursion_remaining` | Small. [read] |
| Build | Public headers | Internal headers are installed (`include/python3.12/internal`, needs `Py_BUILD_CORE`) for struct fields and inline functions; their hidden functions can't be linked | Use only fields, macros and inline functions from internal headers, with `static_assert`s on the layouts we rely on (stage 0, D19). Pin a 3.12.x release. [run] |

Generators and closures are unsupported on 3.7 too (B7, C11), so the port can leave them out. A generator function's vectorcall just falls back.

## Risks

- **Internal layouts change between minor releases.** `prev_instr` (3.12) became `instr_ptr` in 3.13 [inferred]. Generated layouts and `static_assert`s (stage 0) are the defence, and so is targeting one release at a time.
- **The bytecode front end is the bulk of the work.** Of the 186 instructions in 3.12's `bytecodes.c`, the generator handles 13 so far, and the rest need the vocabulary listed above.
- **No core-port code exists yet.** Everything in "the rest of the core" above is from reading, apart from the entry and tracebacks.

## Proposed order

1. A skeleton on 3.12: the core (vcompiler, codemanager, dispatcher, x64 backend) builds against 3.12 headers. The backend doesn't touch CPython [inferred].
2. The vectorcall entry and function watcher from the prototype, compiling `return a + b` through the real compiler.
3. The bytecode front end from upstream `bytecodes.c`, growing the vocabulary one family at a time, with a failures log as for 3.7.
4. The exception table, exception state, ints, then cached globals with watchers.

The 3.7 baseline stays for differential testing until step 3 runs the test suite on 3.12.
