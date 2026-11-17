---
written: 2026-11-17
---
# Calls to closures (KNOWN_BUGS F8)

Compiled code calling a closure (a function with free variables) made the generic call, `function_call` through the eval hook, unless the function was still the virtual one `MAKE_FUNCTION` had made in the caller (comprehensions, local lambdas). This page measures how often that happens and what it cost, and records what was done.

## How often [run]

`tools/closurecalls.py` runs each pyperformance benchmark under plain CPython with `sys.setprofile` and counts, per warm iteration, the calls a `CALL_*` makes straight to a Python function: no C function between (not `map`, `sorted(key=)`), not an operator slot, not `__init__`/`__new__` under a class call (`type_call` makes those). At `176e9ad`:

| benchmark | direct calls | to closures | of which not comprehensions | to code with cells of its own |
|---|---|---|---|---|
| richards_super | 547,077 | 65,790 | 65,790: four `fn` methods using `super()` (free `__class__`) | 0 |
| mdp | 1,425,174 | 116,178 | 108,860: namedtuple's `_make`, `_replace` | 7,319 |
| deltablue | 50,293 | 201 | 201: methods using `super()` | 0 |
| chaos | 165,003 | 10,002 | 2 | 10,001 |
| raytrace | 2,017,298 | 15,265 | 0 | 15,333 |
| nqueens | 82,755 | 82,753 | 0 (genexprs) | 1 |
| others | | comprehensions only, or none | | under 400 |

So the real cases are **methods that call zero-argument `super()`**, which the compiler makes closures over `__class__`, and closures made by factories (namedtuple). Comprehensions and generator expressions were already direct (virtual functions). mdp's other closure calls (`Fraction.__new__`, 397k; `forward` from `_operator_fallbacks`, 360k) come from `type_call` and operator slots, which are C, so a compiled `CALL_*` never makes them.

Calls to code with cells of its own (a function containing a closure over its locals) are refused too (`psyfunc.c:fncall_init`): chaos 10k (6% of its direct calls), raytrace 15k (0.8%). Not done here; see BACKLOG.

## What a call cost [run]

`tools/closure_micro.py CASE N`, release build at `176e9ad`, instructions per loop iteration (one call and an add) from `perf stat -e instructions:u` at N = 100,000 and 1,100,000:

| case | CPython 3.7 | Ni before | Ni after |
|---|---|---|---|
| `plain`: a module function | 1,500 | 429 | 429 |
| `const`: a closure in a global | 1,619 | 2,356 | 432 |
| `arg`: a closure passed in, a new one per run | 1,513 | 2,346 | 483 (436 while it was inlined, before C49) |
| `nested`, `nonlocal`: a helper made in the caller (virtual) | 1,514 / 1,286 | 446 / 437 | 446 / 437 |
| `deco_pos`: `def wrapper(*args): return fn(*args)` | 2,279 | 4,054 | 469 |
| `deco`: the same with `**kwargs` | 2,516 | 4,299 | 4,302 |

The generic call cost about 1,900 instructions more than a direct one, and made a closure call slower under Ni than in CPython. A `**kwargs` wrapper is still the generic call: `fncall_init` refuses `CO_VARKEYWORDS` (BACKLOG).

## What was done

- **A constant function** (a method found on its class, a decorated global): `pfunction_simple_call` no longer refuses closure code. `psyco_call_pyfunc` reads `func_closure` and its items at compile time and passes the cells as constants into the callee's free-variable slots, where `LOAD_DEREF` reads them as it reads a frame's (contents at run time, `FMUT`). The cells are borrowed: compiled code uses only a cell's address. `func_closure` can't be set from Python and its length is the code's number of free variables (`funcobject.c:func_set_code`). The F52 defaults check is unchanged.
- **A function known only at run time** (`pfunction_call`, after its code is promoted): `psyfunc.c:psyco_call_closure`. The function itself is not promoted, which would specialise per object and keep each alive. Its globals must be the caller's (one compare; otherwise the real call), so globals and builtins stay unheld constants. Its cells are loaded from `func_closure` and passed as run-time arguments. A call that would need defaults is the real call, since defaults are per object.
- **Zero-argument `super()`** reads the `__class__` cell and the first argument from `tstate->frame`, which a directly called method doesn't have. `pbltinmodule.c:psuper_noargs` is `super_init`'s frame lookup done on the compiled frame: `ptype_call` sends `super()` there (also when the callee isn't known at compile time and the code has a `__class__` cell, behind a run-time test), and `cimpl_super_noargs` makes the same checks and messages. The liveness pass keeps the first argument of such code alive (`mergepoints.c:_find_dead_locals`), or the dead local was dropped and `super()` raised `arg[0] deleted`. Code that rebinds its first argument is still interpreted.
- Tracing (D46), the recursion count (C12) and frames/tracebacks are those of every direct call: `psyco_call_pyfunc` emits them for closures as for anything else. Small closures with constant or virtual cells can be inlined (`call_with_inline_frame` already stores the free cells); one whose cells were loaded at run time is not, since inlining it crashed (C49, found by CPython's `test_decorators`).
- Found on the way, **F92**: a compiled call with the wrong number of arguments raised a Python 2 message (`f() takes exactly 1 argument (2 given)`, also for no arguments). It now makes the real call, which raises CPython's.

Tests: `tests/cases/closure_calls.py` (constant and run-time closures, a decorated function with and without `**kwargs`, nonlocal writes, loop variables, a cell changed between calls, recursion and RecursionError through a closure, tracebacks, tracing, defaults and `__defaults__` changes, other globals, a swapped `__code__`, `super()` in methods and in a lambda, argument counts, an inlinable run-time closure as a method call's argument, which segfaults without the C49 guard). Without `psuper_noargs`, `super_methods` and `closures.super_call` fail (`__class__ cell not found`); without the liveness change those and `containers.super_getitem` fail (`arg[0] deleted`). [run]

## Results [run]

Release builds, baseline `176e9ad` against `650372a`, PYTHONHASHSEED=0. Instructions per warm iteration: `perf stat -e instructions:u` over `tools/profile_bench.py` 1 s and 4 s runs, the difference divided by the difference in iterations.

| benchmark | before (M) | after (M) | |
|---|---|---|---|
| richards_super | 353.94 | 263.18 | −25.6% |
| deltablue | 27.51 | 26.21 | −4.7% |
| mdp | 30,721 | 30,679 | −0.1% (1 and 3 iterations: rough; measured before the C49 guard) |
| richards | 162.67 | 162.64 | |
| raytrace | 3,073.95 | 3,073.59 | |
| go | 1,056.47 | 1,056.95 | |
| hexiom | 40.58 | 40.70 | +0.3% |
| chaos | 460.19 | 459.52 | −0.1% |
| pyflate | 2,275.32 | 2,306.49 | +1.4%, see below |

`tools/compilebench.py`, 29 benchmarks: code 2,283 → 2,300 KB (+0.7%; richards_super 123.4 → 134.5, chaos 50.7 → 55.4, deepcopy_reduce 28.3 → 24.3), compile 19.7 → 19.8 ms. richards_super warm 21.05 → 13.96 ms, 2.6× → 3.9× CPython's (timings on a shared machine).

**pyflate's +1.4% is not a cost of this change.** pyflate calls no closure from compiled code. The difference comes from two closures in `os.py` (`_Environ`'s `encode` and `decode`) called once at start-up, now compiled as callees: with only those two refused (an environment switch in a test build) pyflate is back to 2,275.8 M. Compiling a few unrelated small functions before the benchmark moves the baseline build alone between 2,265 and 2,312 M (0, 1, 2, 3, 5, 8 functions: 2,273, 2,273, 2,270, 2,265, 2,307, 2,312), while padding the Python heap or the GC count doesn't move it. So pyflate's warm code depends on the code-buffer state left by earlier compiles, by about ±1% [run]; coding-pause placement is the likely mechanism [inferred]. Compile counts and every function's compile status are the same in both builds.

Decision: D58.
