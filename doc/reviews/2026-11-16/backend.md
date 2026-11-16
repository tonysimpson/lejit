---
reviewed_at: e1fd808
written: 2026-11-16
status: point-in-time review record; not maintained. The code and doc/MAP.md are authoritative.
---
# Backend review: ni/x64/*, ni/processor.h, ni/platform.c/.h, tools/

Reviewer scope: x86-64 backend. Headers added to every assigned .c/.h, plus tools/test_mcg_template.c and tools/asm_microbenchmarks/*. Nothing else changed.
Tags: **[read]** = seen in code; **[inferred]** = deduced (from ISA rules, call sites or build flags).

How the backend gets selected [read]: setup.py `autodetect()` maps `uname -m` to `x64` and adds `ni/x64` to `include_dirs`. The core then picks up the backend with angle-bracket includes: `<iencoding.h>` (vcompiler.h, codegen.h, codegen.c, Objects/pfloatobject.h), `<ipyencoding.h>` (pycodegen.h, codemanager.c, Python/pycompiler.c), `<idispatcher.h>` (dispatcher.c, vcompiler.c, Python/frames.h) and `<iinitialize.h>` (initialize.h). `v_mem_mov.h` is included by vcompiler.c and Python/pycompiler.h. No other backend exists (the i386 and ivm backends are gone), but codegen.c still has `#error "this was meant for ivm..."`.

Build fact that matters [read]: Python's sysconfig CFLAGS contain `-DNDEBUG`, so every plain `assert()` in the backend is compiled out. Several of those asserts are wrong or would not even compile (see iencoding.h Leftovers).

---

## Map rows

| path | layer | CPython counterpart | role (one line) |
|---|---|---|---|
| ni/x64/iencoding.h | backend | none | x86-64 encoders, the reg/cc model inside PsycoObject, spill/reload/alloc macros, SysV C-call macros, jumps. This is the main macro API between core and backend |
| ni/x64/iencoding.c | backend + glue | none (but encodes CPython error-return conventions) | call proxies, run-time C calls, compile-time libffi calls, Psyco-to-Psyco calls, cc materialisation, integer ALU emitters |
| ni/x64/ipyencoding.h | backend (leaks CPython layout) | Include/object.h Py_INCREF/Py_DECREF | refcount emitters, frame-local prologue/epilogue |
| ni/x64/ipyencoding.c | backend | Include/object.h Py_DECREF/_Py_Dealloc | function_return, decref_dealloc_calling, decref_create_new_ref/lastref |
| ni/x64/idispatcher.h | backend | none | promotion fast-path, conditional/patchable jumps, conditional unify blocks |
| ni/x64/idispatcher.c | backend + core | none | psyco_unify: shuffles stack and registers so the live state matches a snapshot, then jumps to it |
| ni/x64/iinitialize.h | glue | none | calls psyco_processor_init |
| ni/x64/iprocessor.c | backend | none | C-to-Psyco entry glue, psyco_int_mul_ovf, frame walking |
| ni/x64/v_mem_mov.h | backend | none | generic sized load/store emitter behind field access |
| ni/processor.h | backend (interface) | none | prototypes for running code, RUN_ARGC |
| ni/platform.c | glue | none | RWX memory allocation |
| ni/platform.h | glue | none | prototype |
| tools/test_mcg_template.c | tooling | none | expands one encoder macro and prints its bytes |
| tools/asm_microbenchmarks/*.c,.h | tooling | none | time small loops built from encoder macros |

---

## 1. The core-to-backend interface (what a formal boundary would contain)

Everything below is used **outside ni/x64/** (found by grepping every macro and function name, then excluding x64/). Each item lists who uses it. Anything not listed is used only inside the backend.

### 1a. Types, constants and PsycoObject fields (iencoding.h)
- `reg_t`, `REG_TOTAL`, `REG_NONE`, and `REG_TRANSIENT_1` (codegen.c psyco_generic_call 'r' case). vcompiler.h packs `reg_t` into `RunTimeSource` bits 28 and up (`RunTime_New1`, `getreg`, `is_reg_none`, `set_rtreg_to`). So **the source encoding in vcompiler.h is backend-shaped** [read].
- `condition_code_t`, `CC_*` (core uses `CC_ALWAYS_TRUE/FALSE`, `CC_ERROR`, `CC_TOTAL`, plus `CC_E/NE/...` in places), `INVERT_CC`, `INDEX_CC`, `HAVE_CCREG` (codegen.c, dispatcher.c, vcompiler.c, pycompiler.c).
- `PROCESSOR_PSYCOOBJECT_FIELDS` (vcompiler.h) adds `stack_depth`, `reg_array[]`, `ccregs[2]` and `last_used_reg` to PsycoObject. Also `INIT_PROCESSOR_PSYCOOBJECT` (psyfunc.c, vcompiler.c), `PROCESSOR_FROZENOBJECT_FIELDS`, `SAVE_/RESTORE_PROCESSOR_FROZENOBJECT` (dispatcher.h/.c) and `DUPLICATE_PROCESSOR` (vcompiler.c PsycoObject_Duplicate).
- `INITIAL_STACK_DEPTH` (psyfunc.c, vcompiler.c, codegen.c/.h, processor.h).
- Code-buffer hooks: `insn_code_label`, `INIT_CODE_EMISSION` (dispatcher.c, vcompiler.c, codemanager.c), `ALIGN_NO_FILL` (codemanager.c, dispatcher.c), `JUMP_TO` and `MAXIMUM_SIZE_OF_FAR_JUMP` (codemanager.c psyco_emergency_enlarge_buffer), and `CHECK_STACK_SPACE` (pycompiler.c, a no-op). `POST_CODEBUFFER_SIZE` is defined but unused.
- Feature flags: `HAVE_FP_FN_CALLS` (pfloatobject.c/.h, pmath.c), `NEED_STACK_FRAME_HACK` (codegen.c `INITIAL_STACK_DEPTH_INUSE`), `PROMOTION_FAST_COMMON_CASE` and `INTERNAL_PROMOTION_FIELDS` (dispatcher.c rt_promotion_t). These are unused: `MACHINE_CODE_FORMAT`, `PENTIUM_INSNS`, `COMPACT_ENCODING`.

### 1b. Register-tracking macros the core calls
- vcompiler.c: `RTVINFO_RELEASE`, `RTVINFO_MOVE`, `RTVINFO_CHECK`, `RTVINFO_CHECKED`.
- dispatcher.c: `REG_NUMBER`, `RSOURCE_REG`, `RSOURCE_REG_IS_NONE`, `SET_RUNTIME_REG_TO_NONE`, `KNOWN_SOURCE` (fpo_find_regs_array; promotion in do_promotion_internal).
- codegen.c: `SAVE_REG_VINFO`, `NEED_CC`, `psyco_compute_cc`, `CHECK_NONZERO_FROM_RT`.

### 1c. Arithmetic API (codegen.c only, through iencoding.h)
- Macros that assume variables named `po`, `v1` and `v2` exist at the call site: `BINARY_INSTR_ADD/OR/AND/SUB/XOR/MUL/LSHIFT/RSHIFT/CMP/COND`, `UNARY_INSTR_INV/NEG/ABS`.
- Functions: `bint_add_i`, `bint_mul_i`, `bint_lshift_i`, `bint_rshift_i`, `bint_urshift_i`, `bint_cmp_i`, `make_runtime_copy` (also dispatcher.c and Objects/piterobject.c), and `psyco_int_mul_ovf` (processor.h; codegen.c uses it to fold constant multiplications at compile time).
- `bfunction_result` is declared and **never defined** [read].

### 1d. Calls (iencoding.c, declared `EXTERNFN` inside codegen.c, not in a header)
- `compile_time_call(po, fn, flags, argc, argtags[], args[])` and `run_time_call(..., total_output_array_size)`. Both are called only by `psyco_generic_call`. argtags: `l` immediate, `v` run-time vinfo, `r` stack reference, `a`/`A` output array (without/with refs), `d` run-time double, `D` immediate double.
- `psyco_call_psyco(po, codebuf, argsources[], argc, finfo)` (psyfunc.c).
- `psyco_call_code_builder(po, fn, restore, extraarg, block_size)` (vcompiler.c psyco_coding_pause, dispatcher.c psyco_prepare_respawn_ex and ipromotion_finish). **The `restore` parameter is ignored by x64** [read].
- `ABOUT_TO_CALL_SUBFUNCTION` and `RETURNED_FROM_SUBFUNCTION` (codegen.c psyco_inline_enter/exit).
- `INITIALIZE_FRAME_LOCALS` (codegen.c psyco_emit_header) and `function_return` (codegen.c psyco_finish_return).

### 1e. Dispatcher hooks (idispatcher.h)
- dispatcher.c and vcompiler.c use: `conditional_jump_to`, `change_cond_jump_target`, `resume_after_cond_jump`, `setup_conditional_code_bounds`, `make_code_conditional`, `ipromotion_finish`, `fix_fast_common_case` and `psyco_unify`.
- The reverse direction (core implements, backend calls): `fz_find_runtimes` (dispatcher.c), `psyco_resolved_cc` and `cc_from_vsource` (codegen.c), and `psyco_emergency_enlarge_buffer`, `psyco_new_code_buffer` and `SHRINK_CODE_BUFFER` (codemanager).

### 1f. Python-aware emitters (ipyencoding.h, via pycodegen.h)
- vcompiler.c and Objects/*: `psyco_incref_rt`, `psyco_incref_nv`, `psyco_decref_rt`, `psyco_decref_c`, `decref_create_new_ref`, `decref_create_new_lastref`.
- vcompiler.c and pycompiler.h: `v_mem_mov` and `V_MEM_MOV_SCALE_POINTER` (psyco_get_field/put_field and the inline PyErr_Occurred).

### 1g. Execution (processor.h)
- `psyco_processor_run` (eval_hook.c, psyfunc.c, frames.c), `RUN_ARGC` (psyfunc.c), `psyco_next_stack_frame` (frames.c), and `psyco_processor_init` (via iinitialize.h).

**Boundary observation [read]:** the core does not call raw encoders such as `MOV_R_R`. It does manipulate `po->ccregs[]` directly (codegen.c psyco_vinfo_condition / psyco_resolved_cc), as well as `po->stack_depth` (codegen.c, psyfunc.c) and `REG_NUMBER` (dispatcher.c). To formalise the boundary you would need to: move the reg and stack bits of `RunTimeSource` behind an accessor API, put `ccregs` behind functions, declare `compile_time_call`/`run_time_call` in a backend header, and stop the `BINARY_INSTR_*` macros from capturing `v1`, `v2` and `po` implicitly.

---

## 2. Registers, condition codes, stack, calls, patching

### Register allocation [read]
- `RegistersLoop` (iencoding.c) is a circular list: **R13 → R14 → R15 → RBX → RBP → R13** (RBP drops out if `RBP_IS_RESERVED`). `next_free_reg`/`NEED_FREE_REG` walk it round-robin from `po->last_used_reg`. When the chosen register is busy, `NEED_REGISTER` spills its content with `PUSH` (`SAVE_REG_VINFO`) unless the value already has a stack slot, in which case it just drops the register.
- **Key invariant [inferred, consistent with all call sites]:** only callee-saved registers ever hold tracked vinfos. That is why `run_time_call` does not spill anything before a C call. `CALL_SET_ARG_*` only evict the argument register being written (`CHECK_AND_UPDATE_TARGET_REG`), and a source living in a caller-saved register (`CHECK_AND_UPDATE_SOURCE_REG`). Both expect the value to already have a stack copy.
- R12 is callee-saved but **not** in the loop. RAX, RCX, RDX, RSI, RDI and R8–R11 are used only transiently. The encoders reserve `REG_TRANSIENT_1` = R10 and `REG_TRANSIENT_2` = R11 for large immediates and absolute addresses.
- Fixed uses: RAX holds return values (`REG_FUNCTIONS_RETURN`). RCX is the shift count (`SHIFT_COUNTER`, bininstrshift). RAX and RDX are clobbered by `unaryinstrabs` (CQO trick), which saves RDX only if it is tracked; RAX is never tracked, so the "XXX assumes RAX is free" comment holds by the invariant. RBX holds the return value across `FINALIZE_FRAME_LOCALS` (`REG_ANY_CALLEE_SAVED`).
- Psyco-to-Psyco calls (`psyco_call_psyco`) push **every** occupied register and pop them all afterwards, because compiled Psyco code does not preserve callee-saved registers for other Psyco code.

### Condition-code register [read]
- `po->ccregs[2]`: index `INDEX_CC(cc)` = `cc & 1`, so there is one slot for "positive" codes and one for their inversions. A live cc is a VirtualTime vinfo whose `source_virtual_t*` points into `cc_functions_table[cc]` (codegen.c). `HAS_CCREG(po)` is true if either slot is set.
- `NEED_CC()` is `NEED_CC_REG(REG_NONE)`. If a cc is live it calls `psyco_compute_cc`, which materialises the flag as 0/1 in a register (`SETcc`) and turns the vinfo into RunTime. `NEED_CC_REG(rg)` and `NEED_CC_SRC(src)` do the same while avoiding one register.
- Every flag-clobbering emitter must call `NEED_CC*` first. `INC_OB_REFCNT_CC` instead preserves flags with PUSHF/POPF.
- `NEED_FREE_BYTE_REG` zeroes the target with `XOR` wrapped in `PUSH_CC`/`POP_CC` (PUSHF/POPF), which is slow. `IS_BYTE_REG` is always 1.
- `CC_ALWAYS_TRUE/FALSE` (16/17) are pseudo-codes for outcomes known at compile time. `CC_ERROR` = -1.

### Stack layout [read]
- `po->stack_depth` is the number of bytes pushed since the frame base. A "stack position" `p` is the depth just after that value was pushed. Values are addressed RSP-relative as `[RSP + (stack_depth - p)]` (`STACK_POS_OFFSET`). **There is no frame pointer** unless `RBP_IS_RESERVED`; the `*_EBP_BASE` macro names are i386 leftovers.
- Frame of a compiled function, from the bottom up:
  - `glue`/caller alignment pad
  - finfo slot at position 8 (`INITIAL_STACK_DEPTH`)
  - arguments: the last argument is pushed first, so arg0 sits nearest the return address
  - return address (`LOC_CONTINUATION->source` stack position)
  - optional frame locals (`INITIALIZE_FRAME_LOCALS`: `nframelocal` slots, items[0] = f_exc_type initialised to 0)
  - spills and temporaries
- `RUN_ARGC` = (snapshot depth − 8 − 8)/8.
- `RunTime_StackMask` limits stack positions to about 32 MB (vcompiler.h).

### Calling conventions [read]
- **C calls (SysV):**
  - `BEGIN_CALL(n)`, then `CALL_SET_ARG_*(…, i)` with i from n−1 down to 0, then `END_CALL_I/R`.
  - Integer arguments go in RDI, RSI, RDX, RCX, R8, R9, and the rest are pushed.
  - Doubles go in XMM0.. (`CALL_SET_ARG_*_DOUBLE`; run_time_call asserts at most 2).
  - 16-byte alignment: `BEGIN_CALL` subtracts `(16 - (STACK_DEPTH_SINCE_CALL() + stack_args) % 16) % 16`. `STACK_DEPTH_SINCE_CALL` measures from this function's return address, or from the inlining continuation when inlined, and assumes RSP ≡ 8 (mod 16) at function entry. `END_CALL_*` restores the depth.
  - Results come back in RAX or XMM0. `run_time_result` sign- or zero-extends EAX for `CfReturnTypeInt`.
- **Compile-time C calls:** `compile_time_call` → `var_call` builds a libffi `ffi_cif` from the argtags (`l`/`D` only) and the return-type flags. `compile_time_check` then applies the `CfPyErr*` policy at compile time (and can call `PyErr_Occurred()`). This is the `ct-fold` path for `CfPure`.
- **Psyco convention (`psyco_call_psyco`; entry glue in iprocessor.c):**
  - The caller pushes a finfo slot (−1) and then the arguments.
  - The callee **pops its own arguments** (`RET n` in function_return, or POP/ADD/PUSH/RET when n ≥ 0x8000).
  - The result is a new reference in RAX, checked with `CfCommonNewRefPyObject`.
  - `glue_run_code_aligned/unaligned` are generated at init. They save RBX, RBP and R12–R15, add an 8-byte pad when argc is even, write RSP into `*finfo`, push the args from `initial_stack[]` and call.
- **Frame walking:** `ABOUT_TO_CALL_SUBFUNCTION(finfo)` writes the finfo pointer into the current frame's slot, and `RETURNED_FROM_SUBFUNCTION` writes −1 back. `psyco_next_stack_frame` steps down by `finfo_last(*finfo)->link_stack_depth`, which `psyco_call_psyco` sets.

### Patching and linking compiled code [read]
- **Proxies:** `psyco_call_code_builder` emits a short jump over a data block of `block_size` bytes (8-byte aligned), followed by `PUSH RAX; call fn(block_start[, extraarg]); MOV R10, RAX; POP RAX; JMP R10`. The C callback returns the address to continue at. The data block holds `coding_pause_t` (vcompiler.c), `respawn_t` (dispatcher.c) or `rt_promotion_t`.
- **Jumps to code not yet compiled:**
  - `psyco_coding_pause` / `psyco_prepare_respawn_ex` put the proxy in its own code buffer. They then emit `conditional_jump_to` (a `Jcc rel32`, a `JMP rel32` for `CC_ALWAYS_TRUE`, or nothing for `CC_ALWAYS_FALSE`) and keep the returned end-of-instruction address as the "tag".
  - When the proxy runs, `do_resume_coding` / `do_respawn` compile the code and then `change_cond_jump_target(tag, target)` rewrites the rel32 at `tag-4` (`CHANGE_JUMP_TO`). The proxy buffer goes to the trash.
  - Respawn re-runs the compiler from an earlier snapshot into a throwaway buffer until `detect_respawn` fires (dispatcher.c).
- **Promotion (`ipromotion_finish`, PROMOTION_FAST_COMMON_CASE=1):**
  - It emits `MOV R10, imm64(0); CMP reg, R10; JE rel32 → fallthrough`, then the proxy that calls `do_promotion_*`.
  - Each time `do_promotion_*` resolves a value (via a chained-list cache, a new compile, or the megamorphic cut-off), `fix_fast_common_case` re-emits the same three instructions in place with `ONLY_UPDATING=1`. Only `UPDATABLE_WRITE_*` fields are written, so the imm64 becomes the last value seen and the JE target becomes its code. The result is a monomorphic inline cache in front of a slow-path C dispatch.
- **Conditional unify:**
  - `setup_conditional_code_bounds` reserves 2 bytes for a short `Jcc` and gives the duplicate `po2` a 127-byte window. `psyco_unify` writes into it.
  - `make_code_conditional` then either rewrites a lone `JMP` as a far `Jcc` (`IS_A_SINGLE_JUMP` / `IS_A_JUMP`, which inspects `code[1]==0xE9` from the byte before), or emits a short `J!cc` around the block.
- **Buffer overflow:** `psyco_emergency_enlarge_buffer` emits `JUMP_TO(next buffer)`. Inside `psyco_unify`, `data_new_buffer` copies the partial code to a new buffer, and the jump back is emitted again because rel32 is not position-independent.
- **±2 GB assumption [read]:** `JUMP_TO`, `FAR_COND_JUMP_TO`, `JMP_CC_UI32` and `CHANGE_JUMP_TO` write rel32 with no range check, so every code buffer must sit within ±2 GB of every other. platform.c mmaps without a hint, so this is not guaranteed. `CALL_I` and `ADDRESS_ENCODING` do fall back to absolute addressing through R10/R11.

---

## 3. CPython knowledge inside the backend (leaks)
- **ipyencoding.h/.c [read]:**
  - `offsetof(PyObject, ob_refcnt)`, `offsetof(PyObject, ob_type)` and `offsetof(PyTypeObject, tp_dealloc)` (INC_OB_REFCNT_internal, decref_dealloc_calling).
  - `Py_TRACE_REFS` → `_Py_Dealloc`. `_Py_RefTotal` is ignored, and Py_REF_DEBUG is not handled.
  - `DEC_OB_REFCNT_T` assumes `tp_dealloc` is fixed per type.
  - The frame-local exception model (`FINALIZE_FRAME_LOCALS` → `cimpl_finalize_frame_locals`, the Py2 `reset_exc_info`/`sys.exc_type` semantics in pycompiler.c).
  - `LOC_CONTINUATION->array->items[0..2]` as f_exc_type/value/traceback.
- **iencoding.c [read]:**
  - `compile_time_check` and `run_time_result_check` hard-code CPython's error-return conventions: NULL, non-NULL, negative, −1, `PyErr_Occurred`, `Py_NotImplemented` (with DECREF), and tp_iternext NULL-without-error → StopIteration.
  - They also raise `PycException_Raise(ERtPython)` and `PycException_SetVInfo(PyExc_StopIteration, psyco_vi_None())`, create `SkFlagPyObj` constants, and call `psyco_PyErr_Occurred`, which itself inlines `PyThreadState->curexc_type` through `v_mem_mov`.
- **Suggested split [inferred]:**
  - Keep the generic emitters in the backend: "add imm to [reg+off]", "dec and test, call fn(reg) if zero", "sized load/store".
  - Move the CPython-specific pieces (offsets, dealloc policy, error-check policy table) into a `py*`-layer file that a meta-compiler could generate from object.h and the `Cf*` flags.
  - `run_time_result_check` could become core code if the backend exposed "test RAX against an immediate or zero → cc".

---

## Per-file sections

### ni/x64/iencoding.h
| group | class | note |
|---|---|---|
| reg_t, CC enum, PROCESSOR_* fields, RTVINFO_*, DUPLICATE_PROCESSOR | backend (interface) | see §1 |
| WRITE_*/UPDATABLE_WRITE_*, REX/MODRM/SIB/OFFSET/INDIRECT/DIRECT/ADDRESS_ENCODING | backend | generic encoders; the `mod` argument doubles as the /digit |
| OP_REX_64_* family, MODRM_REG_*, REX_64_REG_X_B | obsolete | unused, superseded by the *_ENCODING family |
| instruction macros (MOV/CMP/ADD/PUSH/…) | backend | several unused: MOV_R_X8/X32, CMP_R_A/O8/UO32 (benchmarks only), TEST_R_I, LEA_R_O8, POP_O, ADD_R_I32, PUSH_FROM*, SAVE_REG_TO_RT, UNARY_INSTR_FROM_RT, SHIFT_*_CL, DELAY_USE_OF_2, DONT_OVERWRITE_SOURCE, RTVINFO_IN_BYTE_REG |
| COMMON_INSTR_* | backend | ALU group encoding (ADD=0 … CMP=7) |
| BEGIN_CALL / CALL_SET_ARG_* / END_CALL_* | backend | SysV + 16-byte alignment; BEGIN_CALL opens a `do{` that END_CALL closes |
| NEED_* / spill / reload | backend | allocator |
| jumps, short jumps, alignment | backend | see §2 patching |
| bininstr*/bint_* prototypes, BINARY_INSTR_* | backend (interface) | used by codegen.c |

### ni/x64/iencoding.c
| group | class | note |
|---|---|---|
| RegistersLoop | backend | allocation order |
| psyco_call_code_builder | backend | proxy emitter |
| compile_time_check, var_call, compile_time_call | glue (+ CPython policy) | libffi at compile time |
| run_time_result_check, run_time_result, run_time_call | backend + CPython policy | C call emission |
| psyco_call_psyco | backend | Psyco calling convention |
| psyco_compute_cc | backend | cc → register |
| bininstr*, bint_*, unaryinstr*, bininstrcond, make_runtime_copy | backend | integer ALU |
| `#if 0` integer_and_i, integer_seqindex | obsolete | dead i386 code |

### ni/x64/ipyencoding.h / .c
| group | class | note |
|---|---|---|
| INC_/DEC_*OB_REFCNT*, decref_dealloc_calling | backend + CPython layout | Py_INCREF/Py_DECREF mirrors |
| psyco_incref_*/decref_* inline functions | glue | vinfo → register → emitter |
| decref_create_new_ref/lastref | glue | refcount for stored fields (`eat_reference`) |
| INITIALIZE_/FINALIZE_FRAME_LOCALS | backend + Py2 exc model | prologue/epilogue |
| WRITE_FRAME_EPILOGUE, FUNCTION_RET | obsolete | unused macro copies of function_return |
| function_return | backend | epilogue + RET n |

### ni/x64/idispatcher.h / .c
| group | class | note |
|---|---|---|
| fz_find_runtimes proto, ipromotion_s, UPDATABLE_JUMP_IF_EQUALS, fix_fast_common_case, ipromotion_finish | backend | promotion inline cache |
| conditional_jump_to, change_cond_jump_target, resume_after_cond_jump | backend | jump patching |
| setup_conditional_code_bounds, make_code_conditional | backend | short conditional blocks |
| dmove_s, data_original_table, data_update_stack, data_free_unused, data_new_buffer, psyco_unify | core + backend | unification; the ref-transfer logic is core, the moves are backend |

### ni/x64/iprocessor.c, processor.h, iinitialize.h
| group | class | note |
|---|---|---|
| write_glue_run_code_fn, psyco_processor_run | backend | C ABI → Psyco convention |
| write_psyco_int_mul_ovf | backend | generated `imul; seto` helper for compile-time overflow checks |
| psyco_next_stack_frame | backend | frame walking |
| psyco_processor_init | glue | emits the glue into a code buffer |

### ni/x64/v_mem_mov.h
| group | class | note |
|---|---|---|
| v_mem_mov | backend | v==NULL → load (returns new RT vinfo, nonneg = !signed_); else store. Folds a compile-time base or index into the displacement |

### ni/platform.c / .h
| group | class | note |
|---|---|---|
| psyco_allocate_executable_buffer | glue | Unix: mmap RWX, 32× basicsize. Win32: VirtualAlloc RWX |

---

## Patterns observed
The seed patterns (translations of CPython code) mostly do not apply. Backend-specific:
- `refcount` (emitter side): ipyencoding.h `INC_OB_REFCNT`/`DEC_OB_REFCNT` mirror `Py_INCREF`/`Py_DECREF`; `decref_dealloc_calling` mirrors `if (--op->ob_refcnt == 0) _Py_Dealloc(op)`. *Mechanical* (generable from object.h). The choice of `_NZ` variants (assume the count stays non-zero) is *judgement*.
- `ct-fold`: iencoding.c `compile_time_call` (libffi) for `CfPure`. `psyco_int_mul_ovf` serves codegen.c's constant folding. *Mechanical*.
- `exception-set`: iencoding.c `run_time_result_check` maps `CfPyErr*` → Jcc + `PycException_Raise(ERtPython)`. The policy table is *mechanical* if the Cf flags are known; the flags themselves are *judgement*.
- **NEW: `inline-cache-patch`**: idispatcher.h `ipromotion_finish` / `fix_fast_common_case`. Self-modifying compare+JE for the last promoted value in front of the C dispatcher. *Judgement*.
- **NEW: `lazy-branch-proxy`**: `psyco_call_code_builder` + `conditional_jump_to` + `change_cond_jump_target`. A branch to code not yet compiled goes through a callback stub that compiles and then repoints the jump. Core/backend *judgement*.
- **NEW: `cc-virtual`**: the processor flags are modelled as VirtualTime vinfos in `po->ccregs` and only materialised (`psyco_compute_cc`) when something would clobber them.

---

## Leftovers, bugs and 32-bit assumptions

### 32-bit assumptions and leftovers
- **[read]** `CMP_I8_O` and `CMP_I8_A` encode with `rex_w=false`, so they compare **dword**, not qword.
  - `CHECK_NONZERO_FROM_RT` uses `CMP_I8_O` for stack-resident values (codegen.c `integer_non_null`). A 64-bit value whose low 32 bits are zero (e.g. `1<<32`) would be judged zero [inferred effect].
  - `FINALIZE_FRAME_LOCALS` uses `CMP_I8_A` on f_exc_type (a pointer).
  - tools/asm_test_cases.txt encodes this same 32-bit behaviour as expected output (`cmp $0,0x20(%rax)`).
- **[read]** `ADD_O8_I8` passes `(updatable=true, rex_w=false)`: a 32-bit add. It is only used when `ob_refcnt` is not at offset 0 (Py_TRACE_REFS builds).
- **[read]** `POP_O` passes mod `0x40`, which OFFSET_ENCODING ORs with 0x80 for a 32-bit offset to give mod=11 (register-direct). It is wrong but unused (only `POP_EBP_BASE`, also unused).
- **[read]** vcompiler.h `is_runtime_with_reg` tests bit 31, which is set for R8–R15 (`reg<<28`), so it returns false for those registers. It is an i386 leftover, used only by the unused `DONT_OVERWRITE_SOURCE`.
- **[read]** Naming and comments: `*_EBP_BASE` macros are RSP-relative, comments say EAX/EBX, `PENTIUM_INSNS` mentions "386 and 486", `COMPACT_ENCODING` is unused, and "Set to 0 to limit registers to RAX>RDI" is orphaned. `HAVE_FP_FN_CALLS 1` sits under the comment "XXX hack remove float support".
- **[read]** `long` is used throughout as the machine word and pointer (`long args[]`, `long initial_stack[]`, `sizeof(long)` stack slots), together with the SysV argument registers. Win64 (LLP64, RCX/RDX/R8/R9 plus shadow space) cannot work, so the `_WIN32` branch of platform.c is dead in practice.
- **[read]** rel32 jumps carry the ±2 GB assumption (§2).

### Calls and the stack
- **[read]** `run_time_result_check`, `test_condition_and_pyerr_occured`: `PUSH_R(RAX)`/`POP_R(RAX)` without `psyco_inc/dec_stackdepth`. That misaligns the calls emitted inside `psyco_PyErr_Occurred` and shifts every RSP-relative offset by 8. Any spill PUSHed in between would also be what `POP RAX` pops [inferred]. Compare `psyco_call_code_builder`, which does count its PUSH RAX.
- **[inferred]** `run_time_call` passes argument index `i` (which counts doubles too) to `CALL_SET_ARG_*` as the integer-register index. With a `d`/`D` before an integer argument, the integer lands in the wrong register. `BEGIN_CALL(arg_count - double_arg_count)` is consistent with this only when doubles come last.
- **[read]** `_CHECK_IS_NEXT_ARG` asserts the indices go 0,1,2…, but every caller passes them in reverse. The asserts in `CALL_SET_ARG_FROM_RT`/`_DOUBLE`/`_STACK_REF` refer to an undeclared `vi`. Both only build because of `-DNDEBUG`.
- **[read]** `STACK_DEPTH_SINCE_CALL` returns −1 when inlining with no `LOC_CONTINUATION`, which would make the alignment calculation wrong.
- **[read]** `psyco_call_psyco`: the line `po->stack_depth += stack_correction;` after the argument pushes has no effect, because stack_depth is reset right after the call.
- **[read]** psyco_generic_call (codegen.c) `'r'` with an undefined source pushes the arbitrary contents of R10 (comment says this is intentional).

### Emitter bugs
- **[read]** `bint_add_i`: the path for immediates over 32 bits uses `ADD` (which clobbers flags) without `NEED_CC()`. The LEA path is safe.
- **[inferred]** `v_mem_mov`:
  - (a) Byte store from RSI, RDI or RBP without a REX prefix encodes DH/BH/CH. RBP is allocatable.
  - (b) For word stores, `0x66` is written *after* the REX byte via `WRITE_OPCODES`, so REX is ignored for R8–R15, and R13–R15 are the main allocatable registers.
  - (c) The RIP-relative displacement in `ADDRESS_ENCODING` ignores the trailing immediate of an immediate store to an absolute address (a compile-time base).
  - (d) The immediate-size test for `signed_` sends every negative value through R10 (inefficient, not wrong).
- **[inferred]** `psyco_unify` register phase (1): if the same vinfo is needed in two registers (graph-quotient sharing) and the second target is occupied, `XCHG` moves the first copy away.
- **[read]** `decref_dealloc_calling` updates register tracking (`CALL_SET_ARG_FROM_REG`) inside code that only runs if the count hits zero. This is safe only because the value is required to have a stack copy (not checked under NDEBUG).

### Misc and dead code
- **[read]** Macro hygiene: `MOV_R_I`, `TEST_R_I`, `CMP_I8_*`, `ADD_A_I8`, `SUB_*_I8/I32` etc. are two statements with no `do{}while(0)` wrapper. `PUSH_R`, `POP_R`, `ADD_R_R` and `XOR_R_R` end in `;`. `BEGIN_SHORT_JUMP` declares variables, so it cannot be nested in the same scope with the same id.
- **[read]** idispatcher.h redefines `ONLY_UPDATING` with `#undef`/`#define` inside inline functions. This works only because macros expand at the point of use.
- **[read]** Dead code:
  - `#if 0` blocks in iencoding.c (integer_and_i, integer_seqindex)
  - `bfunction_result` prototype (never defined)
  - `#undef FORGET_REF` (never defined)
  - WRITE_FRAME_EPILOGUE and FUNCTION_RET
  - PROMOTION_FAST_COMMON_CASE=0 branch (untested)
- **[read]** platform.c maps memory RWX (no W^X), which hardened kernels or SELinux may refuse.

---

## Glossary terms
- `code_t`: byte of machine code (psyco.h). `BEGIN_CODE`/`END_CODE` (psyco.h) bind a local `code` to `po->code` and check `codelimit` → `PsycoObject_EmergencyCodeRoom`.
- `ONLY_UPDATING` (iencoding.h): when 1, `WRITE_*` only advance `code`, and only `UPDATABLE_WRITE_*` write. Used to patch instructions in place.
- `REG_TRANSIENT_1/2` (iencoding.h): R10 and R11, scratch registers owned by the encoders and never tracked.
- `RegistersLoop` (iencoding.c): the allocation order.
- `REG_NUMBER(po, rg)`: the vinfo currently held in register rg.
- `ccregs`, `HAS_CCREG`, `INDEX_CC`, `NEED_CC*`, `psyco_compute_cc`: see §2.
- `stack_depth`, `STACK_POS_OFFSET`, `INITIAL_STACK_DEPTH`, `STACK_DEPTH_SINCE_CALL`, `RUN_ARGC`: see §2.
- finfo slot: the machine-stack word at position 8 of each Psyco frame. It holds a `stack_frame_info_t*` or −1. `ABOUT_TO_CALL_SUBFUNCTION` / `RETURNED_FROM_SUBFUNCTION` (iencoding.h) and frames.h `link_stack_depth`.
- proxy: code from `psyco_call_code_builder` (iencoding.c), used by coding pause, respawn and promotion.
- tag: the address just after a rel32 jump, returned by `conditional_jump_to` and consumed by `change_cond_jump_target` (idispatcher.h).
- `ipromotion_s` (idispatcher.h): the head of dispatcher.c `rt_promotion_t`. `jump_if_equal_code` is the start of the patchable compare+JE.
- argtags `l v r a A d D`: iencoding.c `run_time_call` / codegen.c `psyco_generic_call`.

---

## tools/
- **tools/asm**: shell one-liner. Wraps its arguments in `asm("…")`, compiles with gcc and `objdump -d`s the result to show the bytes gas produces. Writes /tmp/adsfdasf.
- **tools/test_asm** (Python, uses distutils): for each `MACRO; gas-syntax` line of asm_test_cases.txt, compares the bytes from `./asm` with those printed by test_mcg_template.c compiled with `-DREPLACE_ME=MACRO`. Encoder regression test (5 cases).
- **tools/test_c_encoding** (shell + **Python 2**: `print` statement, `.decode("hex")`, `/usr/include/python2.7`): expands one macro and disassembles its bytes with distorm3.
- **tools/test_mcg_template.c**: the C template both of the above use. **Stale include path `../c/x64/iencoding.h`** (the tree is now `ni/`), so the tools do not build as they stand [read].
- **tools/asm_microbenchmarks/**: `benchmark.h` mmaps an RWX page, emits `BEGIN_LOOP … END_LOOP` (RSI counter vs RDI = 0x1FFFFFFF) with encoder macros, then times the call. The bench_* files time an empty loop, `MOV r, imm64`, `CMP r, r` and `CMP r, [rsp]`. Same stale `../../c/x64` path.
- **tools/test_harness** (Python): uses `pointbreak` (debugger library) to count breakpoints on `NiEval_EvalFrame` and `NiCode_Run` while running a command, i.e. how often frames are evaluated vs run as compiled code. Imports `nianalyser`.
- **tools/test_runner** (Python): `unittest` main, which first does `import ni` if `NI_ENABLED` is set. Runs the test suite with or without the JIT.
- (Not assigned: `gen_opcodes_h.py` writes the opcode-name table to the stale path `../c/opcodes.h`; `nianalyser/` is the trace analyser fed by the `ni_trace_*` hooks that the jump/call/return macros in iencoding.h emit.)

---

## Questions
- R12 is callee-saved but left out of `RegistersLoop`. Was that deliberate (it needs SIB like RSP), or an oversight?
- Is `psyco_call_code_builder`'s `restore` argument meant to control the PUSH/POP of RAX (i386 had a variant)? x64 always saves RAX.
- `HAVE_FP_FN_CALLS 1` versus the comment "remove float support": is float support meant to be on?
- Is the ±2 GB proximity of code buffers guaranteed anywhere (e.g. by allocating one large region in codemanager)? I found nothing.
- Does anything ever call `run_time_call` with a double argument before an integer one? (pfloatobject.c and pmath.c would show it.)
