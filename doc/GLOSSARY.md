---
written: 2026-11-16
head_ref: e1fd808
---
# Glossary

Each term gives its definition and where it lives (`file:symbol`). **[read]** means it was checked in the code; **[inferred]** means it's a reading of intent. Terms are grouped by concept, not alphabetically.

## Value representation (the heart of Psyco)

- **Binding time / "time"**: when a value becomes known. Psyco tracks three times per value. [read] `ni/vcompiler.h:Source`
  - **RunTime**: known only when the machine code runs. It lives in a register and/or a stack slot. Encoded in the `Source` bits: stack offset (`RunTime_StackMask`), register (`RunTime_RegMask`, top bits), plus flags `RunTime_NoRef` (holds no reference), `RunTime_NonNeg` (known ≥ 0) and `RunTime_Megamorphic` (stop promoting this value).
  - **CompileTime** ("known value"): a constant known while compiling. `Source` points (tag bit 1) to a refcounted `source_known_t {refcount1_flags, value}`.
  - **VirtualTime**: a value that doesn't exist yet in any form. It is described by its parts and built only if needed. `Source` points (tag bit 2) to a `source_virtual_t` holding `compute_fn`, `direct_compute_fn`, `pyobject_mask` and `nested_weight[2]`.
- **`Source`**: a `long` tagged in its low 2 bits (`TimeMask`) with one of the three times above. [read] `ni/vcompiler.h`
- **`vinfo_t`**: the compiler's descriptor of one value. It holds `refcount`, `source`, `array` (sub-values) and `tmp` (scratch used by the dispatcher). [read] `ni/vcompiler.h:struct vinfo_s`
- **`vinfo_array_t`**: a variable-length array of `vinfo_t*` sub-values. For a pointer to a C struct, item *i* describes field *i* (see field descriptors). `NullArray` is the shared empty array; arrays are never NULL. [read] `ni/vcompiler.h:struct vinfo_array_s`
- **`SkFlagFixed`**: set on a compile-time value once generated code depends on it being that exact constant. Fixed values must match exactly in compatibility checks. Non-fixed ones may be **un-promoted** back to run-time to reuse existing code. [read] `ni/vcompiler.h`, `ni/dispatcher.c:compatible_vinfo`
- **`SkFlagPyObj`**: the known value is a `PyObject*` and holds a reference. [read]
- **`psyco_vi_Zero/One/None/Py_True/Py_False/NotImplemented`**: shared compile-time constants. [read] `ni/Python/pycompiler.h:DEF_SK_AND_VI`

## Compiler state

- **`PsycoObject` (`po`)**: the complete compiler state at one point in the bytecode: output pointer `code`/`codelimit`, processor state (register map, condition-code slots, `stack_depth`), language state `pr`, and `vlocals`, the array of every live `vinfo_t`. It plays the role a `PyFrameObject` plays in the interpreter. [read] `ni/vcompiler.h:struct PsycoObject_s`, `ni/Python/pycheader.h`
- **`vlocals` / `LOC_*`**: slots in `po->vlocals`. `LOC_CONTINUATION` is the return address, `LOC_GLOBALS` the globals dict, `LOC_INLINING` the saved parent frame while inlining, and `LOC_LOCALS_PLUS` the locals, cells and value stack. **Only `vlocals` is compared by the dispatcher.** [read] `ni/Python/pycheader.h`
- **`pyc_data_t` (`po->pr`)**: language-specific state that is not compared: code object, `next_instr`, block stack, current pseudo-exception (`exc`/`val`/`tb`), merge-point table, and so on. Fields before `stack_base` are saved in snapshots. [read] `ni/Python/pycheader.h`

## Specialisation mechanics

- **Merge point**: a bytecode position where the current compiler state is compared against previously compiled states (and a snapshot is stored). Upstream Psyco placed them only where control paths join (with delays and a 170-instruction maximum gap). **Ni currently marks almost every instruction** (`default: MP_HERE()`), which multiplies snapshots and disables inlining (see `doc/history/removed-mechanisms.md` A2). [read] `ni/mergepoints.h`, `ni/mergepoints.c:_find_merge_points`
- **Snapshot / `FrozenPsycoObject` (fpo)**: a compact, frozen copy of `po->vlocals` (plus part of `pr`), stored with each code buffer at a merge point. With `VLOCALS_OPC` it is stored as a compressed pseudo-code (`fz_compress`) rather than a copied array. [read] `ni/dispatcher.h`, `ni/dispatcher.c`
- **Compatibility (`psyco_compatible`)**: compares the current `po` with each snapshot at a merge point.
  - **Exact match**: emit a jump to that code.
  - **Partial match**: some non-fixed compile-time values differ. It returns them in `diff`, the caller **un-fixes** them to run-time (`psyco_unfix`) and retries.
  - **No match**: compile new code. [read] `ni/dispatcher.c:psyco_compatible`, `ni/vcompiler.c:psyco_compile`
- **Unification (`psyco_unify`)**: emits the register and stack moves needed to make the current state's run-time layout match a compatible snapshot, then jumps there. Implemented in the backend. [read] `ni/dispatcher.h`; implementation in `ni/x64/idispatcher.c`
- **Promotion**: turning a run-time value into a compile-time one. Psyco emits a run-time switch: the code calls back into the compiler with the actual value, compiles a version specialised for it, caches it (a move-to-front chained list in `rt_promotion_t`), and jumps there. Triggered by raising the **promotion pseudo-exception** (`PycException_Promote`). The bytecode compiler restarts the current instruction once the value is known. [read] `ni/Python/pycompiler.h:psyco_atcompiletime`, `ni/dispatcher.c:psyco_finish_promotion`, `ni/Python/pycompiler.c` (loop end)
- **Megamorphic site**: a promotion site that has seen more than `MEGAMORPHIC_MAX` (5) distinct values. It stops specialising and compiles a generic version with `RunTime_Megamorphic` set. [read] `ni/codegen.h`, `ni/dispatcher.c:do_promotion_pyobj_mega`
- **Un-promotion / unfix**: turning a non-fixed compile-time value back into run-time (emitting a load of the constant), so as to merge with existing code. [read] `ni/dispatcher.c:psyco_unfix`
- **Coding pause**: a small stub emitted instead of real code. When execution reaches it, it calls back into the compiler (`resume_fn`) to compile the continuation lazily, then patches the jump. Used for not-yet-taken branches, full buffers and deep recursion. [read] `ni/vcompiler.c:psyco_coding_pause`
- **Respawn**: for a run-time condition whose rare branch wasn't compiled, Psyco doesn't keep a snapshot at the branch. When the rare branch is first taken, it re-runs the compiler from the last snapshot (`fpo_unfreeze`), throwing away the re-emitted code, until it reaches the same point (`detect_respawn`). It then compiles the rare branch. This trades compile time for memory. [read] `ni/dispatcher.c:do_respawn`, `psyco_respawn_detected`
- **`runtime_condition_t/f`**: ask "is this condition true?" about a run-time condition code. The answer drives a C `if` at compile time, with the other branch compiled lazily (via coding pause or respawn). `_t`/`_f` says which outcome is the likely one. [read] `ni/dispatcher.h`
- **Condition code (`condition_code_t`, `CC_*`)**: processor flags held as a virtual value (`psyco_vinfo_condition`). `CC_ALWAYS_TRUE`/`CC_ALWAYS_FALSE` are compile-time outcomes and `CC_ERROR` signals an exception. [read] `ni/codegen.c`
- **Forking (`psyco_forking`)**: forces mutable virtual values (and condition codes) to be materialised before control flow splits or a call is made. [read] `ni/dispatcher.h`
- **Nested weight**: a per-virtual-type budget (`NW_*`) that stops infinitely nested virtual objects (for example a virtual tuple containing a virtual tuple…) from blowing up code size. `NW_FORCE` means "always materialise". [read] `ni/vcompiler.h`

## Virtual objects

- **Virtual object**: a Python object represented as `VirtualTime` with its fields in `vinfo->array` (for example a tuple whose items are known `vinfo_t`s). It is never allocated unless it **escapes**: stored somewhere, passed to an unknown C function, or returned. On escape, `compute_fn` emits the allocation (or makes a compile-time constant if every part is known). `direct_compute_fn` builds the real object from raw data in a frame, for frame/traceback reconstruction. [read] `ni/vcompiler.h`, e.g. `ni/Objects/ptupleobject.c:compute_tuple`
- **`vinfo_move`**: replaces a virtual value's source with its computed (run-time or compile-time) result, in place. [read] `ni/vcompiler.c`

## Talking to CPython

- **Residual call (`psyco_generic_call`)**: emits a run-time call to a real C function. An argument format string tags each argument: `v` vinfo, `l` raw long, `d` double, `r` by-reference, `a`/`A` array. `Cf*` flags describe the function. If `CfPure` is set and every argument is compile-time, the function is **called now, at compile time**, and its result becomes a constant. [read] `ni/codegen.c:psyco_generic_call`, `ni/cf_flags.h`
- **`Cf*` flags**: `CfPure` (no side effects), `CfNewRef`, `CfNoReturnValue`, `CfNotNegative`, return type (`CfReturnType*`) and error convention (`CfPyErr*`: if NULL, if -1, NotImplemented, iternext…). [read] `ni/cf_flags.h`
- **Meta function**: a compile-time implementation of a C function, with the same signature but `vinfo_t*` in place of values. Registered with `Psyco_DefineMeta(c_fn, meta_fn)` in `Psyco_Meta_Dict`, keyed by C function address. `Psyco_META1/2/3(po, c_fn, flags, fmt, …)` calls the meta function if one is registered, otherwise makes a residual call. Most dispatch goes through type slots: the slot's C pointer (e.g. `tp->tp_as_number->nb_add`) is the key. **This is the interface a generated compile-time CPython would plug into.** [read] `ni/Python/pycompiler.h`, `ni/Python/pycompiler.c:Psyco_DefineMeta`
- **Field descriptor (`defield_t`)**: a packed description of a C struct field: offset, size, index in `vinfo->array`, and flags (`FMUT` mutable, `FPYREF` holds a `PyObject*` reference, `FARRAY` array, unsigned or non-negative). Built with `DEF_FIELD(cstruct, ctype, field, prevfield)`. `psyco_get_field`/`psyco_put_field` emit or fold loads and stores and cache results in `vinfo->array`. [read] `ni/vcompiler.h`
- **Pseudo-exception**: the compile-time analogue of a Python exception, held in `po->pr.exc/val`. Real Python exceptions use the exception class. Control events use special virtual sources: `EReturn`, `EBreak`, `EContinue`, `EInline` and promotion. `ERtPython` means "an exception is set at run-time". [read] `ni/Python/pycompiler.h`
- **`PycException_*`**: compile-time equivalents of `PyErr_*`. [read] `ni/Python/pycompiler.c`
- **Inlining**: small, loop-free Python functions (`MP_FLAGS_INLINABLE`) are compiled into the caller by swapping `vlocals` (`EInline` pseudo-exception, `psyco_save_inline_po`/`psyco_restore_inline_po`). [read] `ni/psyfunc.c:psyco_call_pyfunc`, `ni/Python/pycompiler.c`

## Code and run-time

- **`CodeBufferObject`**: a block of emitted machine code, plus the snapshot it was compiled from (if at a merge point). [read] `ni/codemanager.h`
- **`code_t`**: a byte of machine code. [read]
- **`GLOBAL_ENTRY_POINT(po)`**: the language compiler's entry, which for Python is `psyco_pycompiler_mainloop`. [read] `ni/psyco.h`
- **Frame-eval hook**: Ni installs `NiEval_EvalFrame` via PEP 523 (`interp->eval_frame`). It compiles a code object the first time it runs and runs the machine code with `f_localsplus` as the arguments. [read] `ni/eval_hook.c`
- **`stack_frame_info_t` (finfo)**: per-call information that maps machine stack frames back to Python code objects and globals, for frame and traceback reconstruction. [read] `ni/Python/frames.c:psyco_finfo`
- **nianalyser**: an external Python tool (`tools/nianalyser`, using `pointbreak` and `distorm3`) that sets breakpoints on the empty `ni_trace_*` hooks to log and disassemble what the JIT emits. It replaced Psyco's built-in logging. [read] `ni/ni.c`, `tools/nianalyser/nianalyser/__init__.py`

## Build switches

- `IS_PY3K`: defined unconditionally in `ni/compat2to3.h`, so every `#ifdef IS_PY3K … #else …` else-branch is dead Python 2 code (all of `pstringobject.*` is one). [read]
- `ALL_STATIC` (put all `.c` files in one unit), `NI_TRACE` (debug build with `-O0 -g3`), `ALL_CHECKS` (`extra_assert` and coherence checks). Set via environment variables in `setup.py`. [read]
- `VLOCALS_OPC` (compressed snapshots, on), `COMPRESS_COMPILETIME_SUBITEMS`, `PROMOTION_FAST_COMMON_CASE`, `HAVE_CCREG`, `REG_TOTAL`. Compile-time feature switches. [read] `ni/dispatcher.h`, `ni/dispatcher.c`, backend headers
