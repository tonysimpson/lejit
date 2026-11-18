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
| B1 | Opcode coverage (LOAD_METHOD, POP_EXCEPT, CALL_FUNCTION_KW/EX, ...) | B | see the opcode table at `e404100` | a880444, 70882a6 | translated |
| B2 | Frame emulation (`_PyThreadState_GetFrame` hook, f_back chain) | B | hook commented out | 58aff23, a880444 | core |
| B3 | Compile a running frame (`PsycoCode_CompileFrame`) | B | gone | 58aff23 | core |
| B4 | `locals()/eval/vars/dir` safety (`NoControlFlowIfBuiltin`, ppsyco metas) | B | gone, with no replacement guard | 58aff23, a880444 | core |
| B5 | Proxy functions / `psyco.bind` | B | dead code | 58aff23, 7441415 | obsolete |
| B6 | Code-object filter hook, memory accounting, `_psyco` API | B | gone | 58aff23 | translated/core |
| B7 | Kw-only args, closures, module code, generators | B | rejected; **closures and keyword-only arguments compile at the frame entry** (`1780ba5`); **generator expressions compile fused into sum/any/all/list/tuple/set** (FINDINGS.md, Generators) | e1fd808, a880444 | translated |
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

## Since the port

The table is the state at the port (`e1fd808`). Since then, among others: LOAD_GLOBAL is cached behind the dicts' version tags (A1, D25); merge points go by control flow and small functions are inlined by upstream's rule (A2, A4, D10); dead locals are dropped at merge points (A3, D15); ints, bools and range are virtual again (A5, A6, A8; D33, PATTERNS P18, P23); attribute lookup is specialised on the type (A9, D30); builtin metas are registered or generated (A13, D86); `ALL_STATIC` builds (A21, BUILD.md); closures are called directly (B7, D58); METH_FASTCALL builtins are called directly (B8, D36). Current behaviour is the code's; the per-item detail and the opcode coverage table of the time are `git show e404100:doc/history/removed-mechanisms.md`.
