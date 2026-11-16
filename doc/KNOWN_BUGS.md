---
written: 2026-11-16
head_ref: e1fd808
verified: false   # every entry below comes from reading code; nothing has been built or run
---
# Known and suspected bugs

One list, gathered from the 2026-11-16 reviews (`doc/reviews/2026-11-16/`), the history passes (`doc/history/`) and a direct read of the memory-access code.

- **Live** means a path the current Python 3 code can reach. **Dormant** means the code exists but no current caller triggers it; it will bite when the relevant feature comes back.
- Evidence: **[read]** means seen in code; **[inferred]** means reasoned, for example from the ISA rules; **[run]** means observed in a test or debugger.
- When one is confirmed or fixed, record it here with the commit.

## Memory access: `ni/x64/v_mem_mov.h` and the field functions in `ni/vcompiler.c`

M1–M5 and M9 were confirmed by executing the emitted code (`tests/encoding/run_v_mem_mov.sh`, 90k cases) and fixed in one commit (see git log for `v_mem_mov`).

`v_mem_mov` emits every load and store behind `psyco_get_field`/`psyco_put_field` on run-time objects. It was written for Ni in `5e0f435` (2018-06-07) to replace i386's `psyco_memory_read/write` (`git show 9bd5834:c/i386/iencoding.c`). The only fields live Python 3 code touches are 8 bytes wide, which is why most of these bugs are dormant.

| # | Bug | State | Evidence |
|---|---|---|---|
| M1 | **Fixed.** **Signed byte/word loads don't sign-extend to 64 bits.** It emits `MOVSX r32, r/m8|16` (0F BE / 0F BF) with `rex_w = false`. The 32-bit destination write then zero-extends, so a stored -1 reads back as 0x00000000FFFFFFFF. Needs REX.W. | **Live** via `pstructmember.c` `T_BYTE`/`T_SHORT` | [read] encoding; [inferred] effect (x86-64 rule: 32-bit writes zero the upper half) |
| M2 | **Fixed.** **Word stores put 0x66 after REX.** `b1 = 0x66` is emitted by `WRITE_OPCODES` *after* `REX_ENCODING`. A REX prefix that doesn't immediately precede the opcode is ignored, so with R8–R15 as value or base (R13–R15 are the main allocatable registers) the store uses RAX–RDI instead. | Dormant (word stores only in Py2 `pstringobject.c`) | [read] |
| M3 | **Fixed** (value copied to R10). **Byte stores from RBP/RSI/RDI emit no REX**, so the register encodes CH/DH/BH, not BPL/SIL/DIL. RBP is allocatable (`RBP_IS_RESERVED` 0). | Dormant | [read]; [inferred] effect |
| M4 | **Fixed** (`ADDRESS_ENCODING` takes `imm_len`). **RIP-relative immediate stores miss the immediate's length.** For an immediate store to a compile-time address, `ADDRESS_ENCODING` computes `rip_offset = address - (code + 5 + op_len + rex)`, leaving out the trailing 1/2/4-byte immediate, so the store lands that many bytes past the target. Only small constants qualify, because large ones go via R10. | Dormant (needs a store into a compile-time object) | [read] |
| M5 | **Fixed.** **Unsigned loads are flagged `RunTime_NonNeg`** (`RunTime_New(v_reg, false, !signed_)`), including 64-bit unsigned values with the top bit set. That's wrong for later signed comparisons. | Dormant (no unsigned 64-bit field descriptors today) | [read] |
| M6 | **Fixed** (4-byte case added, by reading; no test). **Compile-time field reads treat every 4+-byte field as `long`.** `psyco_internal_getfld` `switch (FIELD_SIZE2(df))` handles only sizes 0 and 1, and `default:` does `*(long *)ptr`. That was right on i386, but on x86-64 it reads 8 bytes from 4-byte fields (wrong value, possible over-read). Unsigned 4-byte fields also aren't zero-extended. Inherited from upstream. | Dormant (no immutable 4-byte descriptors on live paths) | [read] |
| M7 | Moot now: stores no longer look at the signed flag. `psyco_put_field_array` passes `(df & FIELD_UNSIGNED)` as `signed_`; `psyco_internal_putfld` passes `== 0`. The sense is inverted between the two. Stores only use it for the immediate-range test, so the effect is mostly inefficiency. | Live, minor | [read] |
| M8 | **Guarded** (`f53ef68`). A virtual `base`/`index` would be treated as compile-time and dereferenced (`V_CT_GET_VALUE`). Callers compute them first; `v_mem_mov` now `extra_assert`s it. | Guarded | [read] |
| M9 | **Fixed:** only qword stores need the value to fit an imm32. Inefficiency: every negative signed constant goes through `MOV R10, imm64` (the range test is `value < 0 \|\| value >= 2^31`). | Live, perf | [read] |
| M10 | `pstructmember.c` `T_INT`/`T_UINT` use `DEF_ARRAY(long, 0)`, an 8-byte read of a 4-byte field ("XXX assumes sizeof(int) == sizeof(long)"). | **Live** | [read] |
| M11 | `pdictobject.c` `DICT_ma_used` is declared `int`, but it's `Py_ssize_t` in Python 3. A 4-byte signed read is fine while counts stay below 2^31. | Live, benign | [read] |

## Front end (translated CPython)

| # | Bug | State | Evidence |
|---|---|---|---|
| F1 | **Fixed** (`4cf49f8`). Mixed-type arithmetic raised TypeError: `pabstract.c:binary_op1` was Python 2's, trying number slots only for types with `Py_TPFLAGS_CHECKTYPES`, which doesn't exist in Py3. Now 3.7's. Tests: `tests/cases/mixed.py`. | Fixed | [run] |
| F2 | **Fixed** (`5976d48`). No builtin metas were registered: `pbltinmodule.c` looked up Python 2's `"__builtin__"`. Now `builtins`; `len`, `abs`, `id` register, `divmod` (METH_FASTCALL) is skipped by the flags check (F13). | Fixed | [run] |
| F3 | **Fixed.** Python 2 opcode meanings: `RAISE_VARARGS` (`ebc5cfc`), `BUILD_MAP` and new `BUILD_CONST_KEY_MAP` (`295961f`), `CALL_FUNCTION` (masked to 8 bits; 300 arguments raised "'int' is not callable") and `CALL_FUNCTION_KW` (`ec836d0`). `END_FINALLY` keeps Ni's own finally scheme, consistent with itself (F18). | Fixed | [run] |
| F4 | **Fixed** (`ebc5cfc`). Catching any exception segfaulted: handled-exception state was mapped onto `curexc_*` (Py2's model), and `cimpl_set_exc_info` increfed `curexc_type` just after `PyErr_Fetch` cleared it. Catching now follows 3.7 (EXCEPT_HANDLER block, six stack items, POP_EXCEPT). Tests: `tests/cases/exceptions.py`. What still differs is F18. | Fixed | [run] |
| F5 | **Fixed.** Float division by zero (`9ed7e6c`, `cimpl_fp_div` raises ZeroDivisionError); `pmath.c` now has 3.7's `math_1` errors and int-valued `floor`/`ceil` (`f217df0`). | Fixed | [run] |
| F6 | `pfloatobject.c:direct_compute_float` converts the stored 64-bit word numerically instead of bit-casting it. | Live when frames are rebuilt | [read] |
| F7 | **Fixed** (`06e7922`). `compute_function` stored closure/annotations/kwdefaults/defaults with raw puts into fields not declared mutable or reference-holding, under an `\|\|` meant as `&&`; it aborted once importlib functions compiled. Now `PyFunction_Set*`. | Fixed | [run] |
| F8 | Calling a virtual function ignores closures (code with free or cell variables is now rejected outright, `eec8990`); keyword-only defaults go through `tp_call` (F17). | Mitigated | [inferred] |
| F9 | `pstructmember.c` `T_STRING_INPLACE` builds bytes where CPython 3 builds str. | Live | [read] |
| F10 | `pobject.c:PsycoObject_RichCompare` can run a user `__eq__` twice: a residual slot returns NotImplemented, then there's a residual `PyObject_RichCompare`. | Possible | [inferred] |
| F11 | `pabstract.c:PsycoEval_CallObjectWithKeywords` passes a C-NULL `kw` as `'v'` on the `use_proxy` path, and `psyco_generic_call` dereferences it. | Possible | [inferred] |
| F12 | `PsycoObject_IsTrue` doesn't normalise lengths to 0/1, and `PsycoObject_RichCompareBool` returns the raw value. | Live, latent | [read] |
| F13 | `METH_FASTCALL` builtins are never specialised (`pmethodobject.c`), and `pmath` metas are silently not registered when `ml_flags` differ. | Live (perf) | [read] |
| F14 | `int` is passed to `"l"` (`va_arg(long)`) in `compute_tuple` and `pfloat_richcompare`. This is undefined behaviour, benign in practice. | Live | [inferred] |
| F15 | `plongobject.c:PsycoLong_FROM_LONG` leaks on error. | Live | [read] |
| F16 | **Fixed.** `pfloatobject.c:pfloat_richcompare` dropped the int comparison result without a decref, which left a register owned by an unreachable vinfo. Every float comparison aborted in `psyco_assert_coherent`. | Fixed (see git log) | [run] |
| F17 | **Fixed** (`0d3e280`). A compiled call to a function with keyword-only arguments failed: the fallback `cimpl_call_pyfunc` rebuilds the function from code, globals and defaults, losing `__kwdefaults__`. Such calls now use the function's own `tp_call`. Exposed by importlib's `_verbose_message` once F4 was fixed. | Fixed | [run] |
| F18 | Exception details that still differ from 3.7 after F4: (1) a bare `raise` records one extra traceback entry (ceval's reraise skips it); (2) inside a `finally` block during an exception, `sys.exc_info()` isn't set, because Ni pushes a single 3-tuple there, not ceval's EXCEPT_HANDLER; (3) the Py2 per-frame saved exception (`FINALIZE_FRAME_LOCALS`, `cimpl_finalize_frame_locals`, a prologue slot) is now never set, so it is dead code still writing `curexc_*` if reached. | Live (minor) | [read] |
| F19 | **Fixed** (`ec836d0`). Code with keyword-argument or star calls was never compiled: `CALL_FUNCTION_KW`/`CALL_FUNCTION_EX` weren't whitelisted, and the shared helper used Python 2's encoding. Still interpreted: `f(x, *a)` and `f(**a, k=1)`, which need `BUILD_TUPLE_UNPACK_WITH_CALL`/`BUILD_MAP_UNPACK_WITH_CALL`. Tests: `tests/cases/kwcalls.py`. | Fixed | [run] |

## Backend (`ni/x64`)

| # | Bug | State | Evidence |
|---|---|---|---|
| B1 | **Fixed** (`d56791d`, found by `tests/encoding/check.py`). **`CMP_I8_O`/`CMP_I8_A` compared 32 bits** (no REX.W). `CHECK_NONZERO_FROM_RT` uses them for stack values, so a 64-bit value with a zero low half (e.g. `1 << 32`) tests as zero. `tools/asm_test_cases.txt` encodes this same behaviour as the expected output. | **Live** | [read] |
| B2 | **Fixed** (`36c7fdc`, by reading). `run_time_result_check` / `test_condition_and_pyerr_occured` do `PUSH RAX` without updating `stack_depth`, so calls made inside are misaligned and RSP-relative offsets are off by 8. This may be linked to the alignment kludge in `dispatcher.c:psyco_compatible`. | **Live** (inline `PyErr_Occurred`) | [read]; link [inferred] |
| B3 | **Fixed** (`e86d7ef`, by reading; latent, no mixed call sites). `run_time_call` uses the overall argument index for the integer registers, so an integer after a double goes in the wrong register. | Depends on callers | [inferred] |
| B4 | **Fixed** (`8efaf24`). `bint_add_i` with an immediate over 32 bits used `ADD` (clobbers flags) without `NEED_CC()`. | Fixed | [read] |
| B5 | **Fixed** (`1e65d32`). rel32 jumps assumed every code buffer is within ±2 GB of every other, but `platform.c` mmapped without a hint. Regions are now requested contiguously and one out of reach is refused loudly; the jump emitters `extra_assert` the range. | Fixed | [run] |
| B6 | `STACK_DEPTH_SINCE_CALL` returns -1 when inlining with no `LOC_CONTINUATION`, which breaks the alignment arithmetic. | Possible | [read] |
| B7 | **Fixed** (`666c209`). `psyco_unify` register phase: a value wanted in two registers was moved to the first and then away to the second (MOV, or XCHG which would leave the wrong value). Now copied last. Reached by `tests/cases/loops.py` `alternating_share` (whether `shared_after_loop` reaches it depends on allocation history). With `ALL_CHECKS`, unify now verifies the target registers in the generated code (UD2 on mismatch); breaking the copy step makes `alternating_share` fail. No case found where the old XCHG fired. | Fixed | [run] |
| B8 | `psyco_call_code_builder` ignores its `restore` argument and saves only RAX. Not live [read]: only callee-saved registers are allocated, promotion does `NEED_CC` first, respawn recompiles, and `psyco_coding_pause` now asserts no condition code survives (`extra_assert`, `4554896`). | Guarded | [read] |
| B9 | **Fixed** (`2286ac5`). Backend `assert()`s were compiled out by `-DNDEBUG`, and some were wrong (an undeclared `vi`; `_CHECK_IS_NEXT_ARG` required ascending order while callers go last first). Now correct `extra_assert`s, plus a check that every call argument is set. | Fixed | [run] |
| B10 | Executable memory is mapped RWX (no W^X). Hardened kernels may refuse it. | Environment | [read] |
| B11 | **Fixed** (`da6eb6d`). `ADD_O8_I8`, the `Py_INCREF` emitter, had its `updatable`/`rex_w` arguments swapped, so it was a 32-bit add on `ob_refcnt`. | Was live, benign in practice | [run] encoder test |
| B12 | **Fixed** (`90b5cce`). `POP_O` passed 0x40 as the ModRM reg bits, so with a 32-bit displacement it encoded `pop rax` plus garbage. | Was latent | [run] encoder test |
| B13 | **Fixed** (`ea790ae`). Memory operands based on R12 encoded as `[r12+r12*1+d]`, because `OFFSET_ENCODING` passed the base as the SIB index. | Was latent (R12 is not allocated) | [run] encoder test |
| B14 | **Fixed** (`98f71df`). The PyErr_Occurred check after a -1 result pushed RAX, then let the register allocator run; a spill PUSH made the POP restore the wrong value and left the spilled vinfo above the top of stack. Seen as stale list lengths in loops indexing `c[-1]`, wrong results and code bytes decremented through a code address. Now emitted without allocation. Tests: `tests/cases/loops.py`, `builtin_metas.py`. | Fixed | [run] |
| B15 | **Fixed** (`5408840`). `psyco_unify` wrote a jump to its new buffer at `po->code` even for `psyco_unify_code`'s throwaway stack buffer; B5's range assert fired (importing `enum` once merge points followed control flow). Sibling of B5's final-jump fix. Guard: `tests/smoke.py`. | Fixed | [run] |

## Core

| # | Issue | Evidence |
|---|---|---|
| C1 | Not a bug: a needed guard, badly named. Compiled code derives call alignment from the return-address slot's position relative to RSP, and unification can't move that slot, so states may only share code if it has the same alignment. Rename and document when touching `dispatcher.c`. Original note: `psyco_compatible` rejects matches whose return-address slot differs mod 16 (`compare_return_address_in_a_stupid_way`). This is a kludge that causes extra code duplication. | [read] |
| C2 | Restoring dead-local deletion (history A3, DECISIONS D15) also needs the `CodeBuffer_Check` guard in `psyco_compatible` that `e1fd808` removed. | [read] |
| C3 | Compiled code and many code-buffer references are never freed. | [read] |
| C4 | **Fixed** (`601aa8f`). `mergepoints.c:psyco_next_merge_point` walked past the -1 sentinel into unrelated memory. Hit on the first run (importlib). | [run] |
| C6 | **Fixed** (see git log, `frames.c`). Every compiled call leaked its frame: `frame_runtime_capsule_new` increfs the frame, code and globals, but the capsule destructor only freed the struct. Found by the leak check in `tests/run.py`. | [run] |
| C5 | **Fixed** (`ad3cf79`). The headers defined globals without `extern` (`EXTERNVAR` was empty), so gcc 10+ (`-fno-common`) failed to link. `EXTERNVAR` is now `extern` and `-fcommon` is gone. | [run] |
| C7 | **Fixed** (`1f46b4b`). `psyco_assert_coherent` didn't count `po->pr.tb` as a live reference, so it aborted when a finally block re-raised with a run-time traceback in a register. Debug builds only. | [run] |
| C8 | **Fixed** (`09c55a5`). `mergepoints.c` placed the merge point of an EXTENDED_ARG-prefixed instruction after the prefix, where jumps never land and the compiler never stops; the merge-point assertion failed in long functions (`sre_parse`). | [run] |
| C9 | **Reference counts differ from CPython's.** Compiled code doesn't own a reference for each value-stack slot, so an object can have refcount 1 where ceval's stack would make it 2. C code that tests `Py_REFCNT(x) == 1` to reuse or keep an object then behaves differently. Fixed where seen: `**kwargs` in `CALL_FUNCTION_EX` (functools.partial kept the caller's dict). Other such callees may exist (e.g. in-place string concatenation, tuple reuse). | Live (class) | [run] |
| C10 | **Fixed.** At shutdown the garbage collector cleared the `ni` module, freeing `co_to_mp`, which the eval hook still used when later code ran (weakref callbacks): a segfault at exit after CPython's `test_fractions`. The dicts now hold their own reference (`ni.c`). Guard: `tests/cpython_tests.py` compares exit status. | Fixed | [run] |
| C11 | **Compiled frames are only partly visible** (history B2: upstream's frame emulation was removed in the port). Fixed: `NiCode_Run` makes its frame current, and `sys._getframe` is replaced by `ni_getframe`, which emulates compiled frames (`namedtuple` got `__module__` `'__main__'`, `doctest` failed). Still missing: the hook behind `PyEval_GetFrame` (`globals()`, `vars()`, tracebacks), C code that walks `tstate->frame` itself (`warnings` stacklevel), locals and line numbers in emulated frames, and a real frame's `f_back` skips compiled callers. Tests: `tests/cases/frames.py`. | Partly fixed | [run] |
| C12 | **Fixed** (`c4c4ee3`). Compiled code didn't count recursion depth (ceval does on every frame entry), so endless recursion segfaulted instead of raising RecursionError. Now counted in `NiCode_Run` and around direct compiled calls (two C calls per call, about 30% on pure call overhead; could be inlined). Test: `calls.endless_recursion`. | Fixed | [run] |
