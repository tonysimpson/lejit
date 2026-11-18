# Backlog

What is still open, one line each, with where the detail is. Done items leave this list (git keeps them).

## Generation (this phase)
- Steps 5 and 8 of GENERATION.md: what is left is reasoned in `tools/metac/hand_table.py` and the METACOMPILER.md failures log; D99 says which reasons are final.
- 3.8: build it, which needs D100 (block stack) and benefits most from D100 (`_PyArg_CheckPositional`) (PORT38.md).
- Generate the eval-breaker handler from `ceval.c` instead of copying it (D77 (6)).
- Mechanism snippets for 3.11+ (D75): the stubs in `ni/mechanisms/*` that `#error` "not written yet".

## Open bugs
In KNOWN_BUGS.md. Worth doing first among them: C11's `PyEval_GetFrame` hook and emulated frames that keep their identity (also C29's rest). C15's and C18's rests are to be done on 3.7 (D64).
- From other projects' suites (FINDINGS.md, `tools/thirdparty`): C61, memory per code buffer (1.9 kB of RSS each, FINDINGS.md "Memory per code buffer").

## Speed
- From the adversarial suite (FINDINGS.md): compile speed itself (`import_heavy` 9.9× and `many_functions` 9.1× on the short first call: D12 keeps compiling everything, so compiling must get cheaper); fresh or huge generated code compiled and run a few times (`generated_lexer` 2.2×, `template_engine` 2.0×, `exec_templates` 1.6×, `reload_modules`); in-place `s += piece` when the local holds the only reference, as ceval does (`report_concat` 2.0×, cause [inferred]); globals and builtins rebound under compiled code (`plugin_system` 1.3×); Python dunder methods (`operator_overloading` 1.3×); `lambdas_in_loops` 1.2×; a guard broken by a class attribute bumped every call (`class_counter` 1.1×).
- Attribute stores: checks shared between consecutive stores to one object; present-name loads on the shared store cache (FINDINGS.md "Attribute stores"); GC tracking inline; one dealloc glue for every decref (D47).
- Load guards: a class attribute bumped as a counter fails every guard on its class (`tools/load_micro.py bump`); `store_guard` could share `load_guard`'s C table (D60).
- Type hints (D63): a wrong hint at a polymorphic call site pays a respawned block; the guard could be skipped where the promotion after it has seen several types, if a replay decides the same.
- Fewer `eval_breaker` tests: `type_call` for a class with Python `__new__`/`__init__`, behind a guard (D53).
- Calls to closures (D58, FINDINGS.md "Calls to closures"): callees with cells of their own; `**kwargs` callees; run-time closures needing defaults or other globals.
- Unshared-dict loads: an inline probe of a combined dict for a constant key, if go still shows `lookdict`.
- Strings, only with a string-heavy benchmark: tuple slices and steps, run-time `in`, iteration, virtual characters (QUESTIONS 40).
- Opcode coverage: generators other than genexprs (FINDINGS.md "Generators" option A, QUESTIONS 33), class bodies, module code, C15's functions.
- Register calling convention between compiled functions (QUESTIONS 35 first).

- Real-world suite: applications chosen (REALWORLD.md); commit the workloads and harness blind, then run once Ni works on 3.10 or later.

## Code size and compile time
- Shared code after polymorphic promotion sites, option D: on 3.7 (D64, FINDINGS.md "Option D", QUESTIONS 43–46).
- Forgotten values (D43): forget a forgotten value's type too (`Psyco_NeedType`'s cut-off goes to ceval, D140; a generic path in its callers would keep it compiled); mark values that partial matches un-promote before the cap (SPECIALISATION.md).
- Unused facts (D50, D57): promotions at a merge point, whose own compare must keep the fact.
- Rejoining (D44): respawn points that are not split sites (error paths, overflow); a small case for C27.
- Promotion code (FINDINGS.md "Code size"): matching across promoted knowledge; why chain placement moves richards' cycles; a cut-off for sites without one.
- Other two-form values at merge points (QUESTIONS 38): ints outside -5..256, floats, stack slots.
- Type-set guards: one copy of a base-class method for several subclasses (richards' `Task`).
- The alignment test at merge points (C1): align at calls instead.
- Merge-point lookup is a linear scan, and meta lookup allocates a `PyLong` key per lookup (ARCHITECTURE.md).
- libffi compile-time calls call `ffi_prep_cif` every time (QUESTIONS 36); instruction-cache effects (QUESTIONS 35).

## Tooling
- Encoder from `@asm` annotations (QUESTIONS 37); `v_mem_mov.h` annotations and a lint for unannotated emission (BUILD.md).
- Stale tools (QUESTIONS 19) and `MANIFEST` (QUESTIONS 20).

## Research
- Decoupling the core (D16, QUESTIONS 21–23, 25).
- 3.12 port (PORT312.md, QUESTIONS 29–33).
- **A fast mode** (Tony, D127): the default always follows CPython; a fast mode may differ where Python promises nothing, as PyPy does (identity of floats and ints by value, finaliser timing; C17, D130). Needs the list of such places and their measured cost first.
