---
written: 2026-11-16
head_ref: e1fd808
---
# Architecture

How the pieces fit together. Definitions are in `GLOSSARY.md`, and the file-by-file classification is in `MAP.md`. Function names are given as `file:function` so they can be grepped. **[read]** means checked in the code; **[inferred]** means interpretation.

## Layers

```
            CPython 3.7 (unmodified)
                 | PEP 523 eval_frame hook
                 v
 glue      ni/eval_hook.c, ni/ni.c, ni/initialize.h
                 |
 core      ni/vcompiler.*   value model, field access, psyco_compile*, coding pauses
           ni/dispatcher.*  snapshots, compatibility, respawn, promotion
           ni/codegen.*     residual calls, condition codes, integer ops   (+ some backend)
           ni/codemanager.* code buffers
           ni/mergepoints.* where states are cached; which opcodes are allowed
           ni/psyfunc.*     Python->Python calls, inlining
                 |
 translated ni/Python/pycompiler.c   ceval main loop, compile-time version  (+ core at loop end)
           ni/Objects/p*.c, ni/Modules/pmath.c, ni/Python/pbltinmodule.c
                 |  Psyco_META*, psyco_generic_call, psyco_get_field, runtime_condition_*
                 v
 backend   ni/x64/*  instruction encoding, register allocation, unification moves,
                     promotion switch code, C calling convention
```

Dependencies point downward in principle. In practice the translated files also use backend macros directly (`BEGIN_CODE`/`END_CODE` blocks), and `ipyencoding` puts CPython object layout knowledge (refcount fields) into the backend. Both leaks need closing in a tidied design. [read; leak extent: see backend section]

## Lifecycle of a compiled function

1. **Import.** `ni/ni.c:PyInit_ni` calls `initialize_all_files` (`ni/initialize.h`). Every translated file's `*_init` registers its meta functions (`Psyco_DefineMeta(c_slot_or_fn, psyco_version)`) and initialises its virtual sources (`INIT_SVIRTUAL`). `ni/eval_hook.c:ni_eval_hook_init` installs `NiEval_EvalFrame` as the interpreter's frame evaluator. [read]
2. **First call of a function.** `NiEval_EvalFrame` [read]:
   - It skips module-level code (`f_globals == f_locals`).
   - `psyco_get_merge_points(co)` returns `Py_None` if the code uses any opcode not supported in `ni/mergepoints.c`. The frame then runs in CPython as normal.
   - Otherwise, if the code object has no entry point yet (its `co_extra` record, `ni/ni.c:ni_code_extra`), it builds a `PsycoObject` with `ni/vcompiler.c:PsycoObject_FromCode`: arguments become run-time stack slots, globals become a compile-time constant, unbound locals become compile-time zero, and `*args`/`**kw` are run-time with a known type.
   - It then calls `PsycoObject_Ready` (first merge point, drop dead vars) and `psyco_compile_code`, and caches `(codebuf, globals)`.
   - **There's no hotness check: every eligible function is compiled on its first call**, with auto-recursion depth `DEFAULT_RECURSION` = 10.
3. **Running.** `NiCode_Run` registers the frame (`ni/Python/frames.c:psyco_frame_start_register`) and jumps into the machine code via `psyco_processor_run(codebuf, f_localsplus, finfo)` (backend). [read]
4. **Compiling: `ni/Python/pycompiler.c:psyco_pycompiler_mainloop`.** A `switch(opcode)` mirroring `ceval.c`. Each opcode works on the virtual value stack (`vinfo_t*`) by calling the compile-time object protocol (`PsycoNumber_Add`, `PsycoObject_GetAttr`, … in `ni/Objects/`). Those in turn [read]:
   - dispatch through type slots with `Psyco_META*`: a meta function if registered, else a residual call;
   - fall back to `psyco_generic_call`, which folds at compile time when `CfPure` and every argument is constant;
   - read and write struct fields with `psyco_get_field`/`psyco_put_field`;
   - branch on run-time facts with `runtime_condition_t/f`, which may emit a conditional jump to a coding pause or respawn stub;
   - ask for a constant with `psyco_atcompiletime`/`Psyco_NeedType`, which raises the promotion pseudo-exception.
5. **At each merge point** (`fine:` label in the main loop) [read]:
   - `psyco_limit_nested_weight` forces over-nested virtuals.
   - `psyco_delete_unused_vars` drops dead locals.
   - `psyco_compile(po, mp, true)` (`ni/vcompiler.c`) compares the state against every snapshot stored at this merge point (`ni/dispatcher.c:psyco_compatible`, a linear scan):
     - **exact match**: `psyco_unify` emits moves and a jump, and compilation of this path ends;
     - **partial match**: un-fix the differing constants and retry;
     - **no match**: register a new snapshot and carry on compiling inline.
   - If the code buffer is nearly full (`BUFFER_MARGIN`), it stops and emits a coding pause.
6. **End of the main loop: pseudo-exceptions** [read]:
   - **Promotion**: `psyco_finish_promotion` emits a run-time switch on the value (backend `ipromotion_finish`). Compilation of this path stops, and the instruction is restarted later with the value known.
   - **`EInline`**: swap in the callee frame's `vlocals` and keep compiling (inlining).
   - **Python exception, `EBreak` or `EContinue`**: unwind `po->pr.blockstack` exactly like `ceval.c` (`SETUP_LOOP`/`SETUP_EXCEPT`/`SETUP_FINALLY`).
   - **`EReturn`, or an uncaught exception**: `exit_function` and then `codegen.c:psyco_finish_return`.
7. **Lazy continuation at run-time** [read]:
   - Reaching a **coding pause** calls `vcompiler.c:do_resume_coding`, which compiles the continuation and patches the jump.
   - Reaching a **promotion switch** calls `dispatcher.c:do_promotion_long/pyobj/pyobj_mega`, which looks the value up in a move-to-front list. If it's not found, it duplicates the frozen `po`, makes the value a fixed constant, and compiles.
   - Reaching a **respawn stub** calls `dispatcher.c:do_respawn`, which re-runs compilation from the last snapshot to rebuild the state, then compiles the rare branch.
8. **Calls to Python functions** (`CALL_FUNCTION`… go to `ni/psyfunc.c:psyco_call_pyfunc`) [read]:
   - If the argument count and defaults are known at compile time, it binds arguments at compile time.
   - Small loop-free callees can be **inlined** (`MP_FLAGS_INLINABLE`, the `EInline` pseudo-exception). In practice this almost never fires now, because nearly every instruction is a merge point (`doc/history/removed-mechanisms.md` A2/A4).
   - Others are compiled separately and called directly (`psyco_call_psyco`), passing a `stack_frame_info_t` for frame reconstruction.
   - Anything unsupported (keyword-only args, closures, unknown arity, recursion depth used up) becomes a residual call to `cimpl_call_pyfunc`.

## The mixed files, by section

### `ni/Python/pycompiler.c` (≈2.3k lines)
| Section | Class | Note |
|---|---|---|
| `Psyco_DefineMeta`, `Psyco_DefineMetaModule`, `Psyco_DefineModuleFn/C`, `Psyco_Meta1x/2x/3x` | core | Meta-function registry and dispatch. |
| `PycException_*`, `psyco_virtualize_exception`, `clear_pseudo_exception` | core | Pseudo-exception machinery. |
| `cimpl_pyerr_*`, `cimpl_set_exc_info`, `cimpl_finalize_frame_locals`, `PycException_Fetch*` | translated | Run-time helpers mirroring `ceval.c`/`errors.c` exception state; Py3 rework. |
| `cimpl_new_frame`, `cimpl_rt_traceback`, `cimpl_vt_traceback`, `PsycoTraceBack_Here` | translated + core | Traceback construction for compiled frames. |
| `psyco_ext_do_calls`, `cimpl_load_global`, `cimpl_unpack_*`, `cimpl_do_raise`, `cimpl_import_*` | translated | Compile-time or residual copies of `ceval.c` helpers (`call_function`, `do_raise`, `import_name`, `unpack_iterable`). |
| `psyco_pycompiler_mainloop`, the opcode `switch` | translated | Mechanical in shape: one case per `ceval.c` opcode. |
| `psyco_pycompiler_mainloop` after the `switch` (merge points, promotion, inline, unwind) | core | The specialiser's control loop, mixed with `ceval` block unwinding. |
| `exit_function`, `compute_and_raise_exception` | core + translated | |

### `ni/dispatcher.c` (≈1.9k lines): all core
Snapshot compression (`fz_*`, two implementations selected by `VLOCALS_OPC`); compatibility (`compatible_vinfo`, `compatible_array`, `psyco_compatible`); dead-variable removal (`psyco_delete_unused_vars`, `psyco_simplify_array`); respawn (`do_respawn`, `psyco_respawn_detected`, `psyco_prepare_respawn*`); promotion (`rt_promotion_t`, `do_promotion_*`, `psyco_finish_promotion`); un-promotion (`psyco_unfix`). Ni additions: `compare_return_address_in_a_stupid_way`, which rejects matches whose return-address stack slot differs mod 16 (for x64 call alignment). Ni's own comment calls it a kludge. [read]

### `ni/codegen.c` (≈1k lines): core + backend
`psyco_generic_call` (residual calls, `CfPure` folding), condition-code virtual sources, `integer_*` arithmetic and comparison emitters (these contain x64 encoding: backend), `psyco_finish_return`, rich-compare helpers. [read; split detail: see backend section]

### `ni/vcompiler.h`/`.c`: all core
The value model (time tags, `vinfo_t`, arrays, field descriptors), `PsycoObject`, `psyco_compile`/`psyco_compile_cond`/`psyco_compile_code`, `psyco_coding_pause`, `PsycoObject_FromCode`. [read]

## Backend boundary

The backend is chosen at build time: `setup.py` puts `ni/x64` on the include path, and the core includes `<iencoding.h>`, `<ipyencoding.h>`, `<idispatcher.h>` and `<iinitialize.h>`. x64 is the only backend left (i386 and ivm were deleted; see `doc/history/removed-files.md`). [read]

### What the core uses from the backend (the de facto interface) [read]
| Group | Items | Used by |
|---|---|---|
| Types and state | `reg_t`, `REG_TOTAL`, `REG_NONE`, `condition_code_t`/`CC_*`, `INVERT_CC`, `INDEX_CC`, `HAVE_CCREG`. `PROCESSOR_PSYCOOBJECT_FIELDS` adds `stack_depth`, `reg_array[]`, `ccregs[2]` and `last_used_reg` to `PsycoObject`. Also `INIT_PROCESSOR_PSYCOOBJECT`, `DUPLICATE_PROCESSOR`, `*_PROCESSOR_FROZENOBJECT`, `INITIAL_STACK_DEPTH` | vcompiler, dispatcher, codegen, psyfunc |
| Register tracking | `RTVINFO_RELEASE/MOVE/CHECK`, `REG_NUMBER`, `RSOURCE_REG*`, `SET_RUNTIME_REG_TO_NONE`, `SAVE_REG_VINFO`, `NEED_CC`, `psyco_compute_cc` | vcompiler, dispatcher, codegen |
| Integer ALU | `BINARY_INSTR_*`/`UNARY_INSTR_*` (these macros silently capture `po`, `v1` and `v2`), `bint_*_i`, `make_runtime_copy`, `psyco_int_mul_ovf` | codegen (`integer_*`) |
| Calls | `compile_time_call` (libffi, for `CfPure` folding), `run_time_call` (SysV call + `CfPyErr*` check), `psyco_call_psyco`, stubs (`psyco_write_stub`, `psyco_stub_trampoline`; D32), `ABOUT_TO_CALL_SUBFUNCTION`/`RETURNED_FROM_SUBFUNCTION`, `INITIALIZE_FRAME_LOCALS`, `function_return` | codegen (`psyco_generic_call`), psyfunc, vcompiler, dispatcher |
| Jumps and patching | `conditional_jump_to`, `change_cond_jump_target`, `setup_conditional_code_bounds`, `make_code_conditional`, `ipromotion_finish`, `fix_fast_common_case`, `psyco_unify`, `insn_code_label`, `JUMP_TO` | vcompiler, dispatcher, codemanager |
| Python-aware emitters | `psyco_incref_*`/`psyco_decref_*`, `decref_create_new_ref`, `v_mem_mov` (sized load/store behind field access) | vcompiler, Objects/*, pycompiler.h |
| Execution | `psyco_processor_run`, `RUN_ARGC`, `psyco_next_stack_frame` | eval_hook, psyfunc, frames |

**Leaks to close in a tidied design** [read]:
- The register bits of `RunTimeSource` (`vcompiler.h`) are shaped for this backend.
- The core manipulates `po->ccregs`, `po->stack_depth` and `REG_NUMBER` directly.
- `compile_time_call`/`run_time_call` are declared ad hoc in `codegen.c`.
- `ipyencoding` and `run_time_result_check` hard-code CPython object layout (`ob_refcnt`, `tp_dealloc`) and the C API error conventions. These belong in a generated "Python layer", not the backend.

### Machine model [read]
- **Registers:** only callee-saved R13, R14, R15, RBX and RBP are allocated, round-robin (`iencoding.c:RegistersLoop`). That's why C calls need no spilling. R12 is unused, R10/R11 are encoder scratch (`REG_TRANSIENT_1/2`), and RAX is the return value. Compiled-to-compiled calls push every occupied register.
- **Condition codes:** live flags are virtual values in `po->ccregs[2]`. Anything that would clobber them calls `NEED_CC*` first, which materialises them with `SETcc`. `NEED_FREE_BYTE_REG` uses a slow PUSHF/POPF "temp fix".
- **Stack:** there's no frame pointer. Values are addressed `[RSP + (stack_depth - pos)]`. Frame from the bottom up: finfo slot (`INITIAL_STACK_DEPTH`), arguments, return address (`LOC_CONTINUATION`), optional frame locals, then spills.
- **Calls:** C calls use SysV, with 16-byte alignment computed in `BEGIN_CALL`. In the Psyco-to-Psyco convention the callee pops its own arguments and returns a new reference in RAX. C-to-Psyco entry goes through glue generated at init (`iprocessor.c`).
- **Patching:** proxies are `JMP over data; PUSH RAX; CALL fn(data); JMP RAX-result`. Jumps are rel32 and get repointed once their target is compiled. A promotion site is a self-patching `CMP imm64; JE` inline cache (PATTERNS: inline-cache-patch).
- **Assumptions:** LP64 and SysV only (no Win64). Every code buffer must be within ±2 GB of every other, with no guarantee (rel32 jumps; mmap without a hint). Memory is mapped RWX (no W^X).

### Suspected backend bugs (from reading, 2026-11-16)
All five have since been confirmed and fixed: KNOWN_BUGS B1 (`CMP_I8_*` compared 32 bits), B2 (`run_time_result_check` and `stack_depth`), M2/M3 (`v_mem_mov` byte and word stores), B3 (`run_time_call` after a double) and B9 (wrong asserts hidden by `-DNDEBUG`).

Details and evidence: `doc/history/removed-mechanisms.md` (A17–A21, D4–D8) the comments at the top of the `ni/x64/*` files, and `doc/KNOWN_BUGS.md`.

## Design observations (for the tidy-up)

- **Global, non-re-entrant state**: `dispatcher.c:cmpinternal`, the static result in `psyco_compatible`, and `frames.c:psyco_finfo` statics. [read]
- **Compiled code is rarely freed** (KNOWN_BUGS C14). Two "XXX don't know what to do with reference" comments remain, both on promotion caches in `dispatcher.c`. [read]
- **Merge-point lookup is a linear scan** over snapshots (the code itself calls it "the most dummy algorithm"). [read] `psyco_compatible`
- **Encoding a `Source` in a tagged `long`** is compact but ties the core to the word size; the 64-bit masks in `vcompiler.h` were re-derived by hand. [read]
- **`pr` vs `vlocals` contract**: two states that differ only in `pr` must produce interchangeable code (`pycheader.h` comment). Any tidied design has to keep this invariant explicit. [read]
- **Meta lookup keyed by C function pointer** (`Psyco_Meta_Dict`, a Python dict). A meta therefore applies to every type whose slot holds that pointer, including subclasses that inherit the slot. Python classes get `slot_*` wrappers, which are never registered, so their operations are always residual. Every lookup allocates a temporary PyLong key. [read; subclass consequence inferred]
- **Restart-on-promotion** relies on opcode implementations having no side effects before they can fail ("use TOP() not POP()", main loop comment). A generator for translated code must respect this. [read]
