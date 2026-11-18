# Backlog

What is still open, one line each, with where the detail is. Done items leave this list (git keeps them).

## Generation (this phase)
- Steps 5 and 8 of GENERATION.md: what is left is reasoned in `tools/metac/hand_table.py` and the METACOMPILER.md failures log; QUESTIONS 50 asks whether policy-only reasons count.
- 3.8: build it, which needs QUESTIONS 48 (block stack) and benefits most from QUESTIONS 49 (`_PyArg_CheckPositional`); F114 (PORT38.md).
- Generate the eval-breaker handler from `ceval.c` instead of copying it (D77 (6)).
- Mechanism snippets for 3.11+ (D75): the stubs in `ni/mechanisms/*` that `#error` "not written yet".

## Open bugs
In KNOWN_BUGS.md. Worth doing first among them: C29 (no events for a frame already running compiled), then C11's `PyEval_GetFrame` hook and `f_lineno`; F97, F100, F103 are small translated-layer fixes. On the 3.12 backlog by D64: C14, C15, C18, F27.

## Speed
- Attribute stores: checks shared between consecutive stores to one object; present-name loads on the shared store cache (FINDINGS.md "Attribute stores"); GC tracking inline; one dealloc glue for every decref (D47).
- Load guards: a class attribute bumped as a counter fails every guard on its class (`tools/load_micro.py bump`); `store_guard` could share `load_guard`'s C table (D60).
- Type hints (D63): a wrong hint at a polymorphic call site pays a respawned block; the guard could be skipped where the promotion after it has seen several types, if a replay decides the same.
- Fewer `eval_breaker` tests: `type_call` for a class with Python `__new__`/`__init__`, behind a guard (D53).
- Calls to closures (D58, FINDINGS.md "Calls to closures"): callees with cells of their own; `**kwargs` callees; run-time closures needing defaults or other globals.
- Unshared-dict loads: an inline probe of a combined dict for a constant key, if go still shows `lookdict`.
- Strings, only with a string-heavy benchmark: tuple slices and steps, run-time `in`, iteration, virtual characters (QUESTIONS 40).
- Opcode coverage: generators other than genexprs (FINDINGS.md "Generators" option A, QUESTIONS 33), class bodies, module code, C15's functions.
- Register calling convention between compiled functions (QUESTIONS 35 first).

## Code size and compile time
- Shared code after polymorphic promotion sites, option D: 3.12 (D64, FINDINGS.md "Option D", QUESTIONS 43–46).
- Forgotten values (D43): forget a forgotten value's type too (needs a generic path for `Psyco_NeedType`); mark values that partial matches un-promote before the cap (SPECIALISATION.md).
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
