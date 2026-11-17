# Status

## 2026-11-16: 3.7 baseline (branch `baseline-3.7`, D18)

Ni builds and runs on CPython 3.7.17 (pyenv, gcc 14). Test suites, all passing (`tools/check.sh` runs them all):
- `tests/run.py`: 228 behaviour cases, each run with and without Ni, required to compile, and leak-checked. No expected failures. Two cases correctly fall back to the interpreter because of unsupported opcodes.
- `tests/encoding/check.py`: 56 `@asm`-annotated emitters, about 192k operand combinations, compared with GNU `as`.
- `tests/encoding/run_v_mem_mov.sh`: about 90k executed loads and stores through `v_mem_mov`.
- `tests/compile_bounds.py` (how much warm code compiles) and `tests/cpython_tests.py` (50 of CPython's own test modules, with and without Ni).

Fixed (one commit each; details in KNOWN_BUGS):
- core: C4 (merge-point lookup ran past its sentinel), C6 (every compiled call leaked its frame), M6 (4-byte compile-time field reads);
- backend: B1, B11–B13 (emitter encodings), M1–M5 and M9 (`v_mem_mov`), B2 (uncounted push), B3 (double/int argument registers);
- translated, under the D18 exception: F16 (a leak in float comparison that crashed every float compare);
- translated, at the owner's request: F4 (exceptions now caught the 3.7 way, with `RAISE_VARARGS` from F3), which exposed F17 (keyword-only defaults dropped) and C7 (a debug check); F1 (mixed-type arithmetic) and F5 (float division by zero).

Inspected the generated code for `while_sum` with nianalyser (`tools/nianalyser/dump_function.py`): 80 chunks, all making sense. There are type promotions with inline caches, then residual calls to `long_add`/`long_richcompare` (integers aren't virtual yet, D13). A quick benchmark (prime counting, sieve, Collatz, Mandelbrot) ran 2× faster than CPython 3.7 for integer code (every operation is still a residual call) and 5× for float code (floats are virtual). Cheap inefficiencies seen: constant arguments always use a 10-byte `movabs`, even for 0, and stack corrections are emitted as `sub rsp, -8`.

Left alone per D18 (translated layer, to be regenerated): the rest of F2–F18, notably the opcodes that keep Python 2 meanings. Then fixed B4, B5, B9, C5, and guarded M8. Quick wins: LOAD_METHOD/CALL_METHOD, DUP_TOP_TWO, BUILD_MAP/BUILD_CONST_KEY_MAP, builtin metas (F2). Compiling more real code exposed and fixed F7, C8 (EXTENDED_ARG merge points), B14 (stack corruption in the -1 error check), and zero-argument `super()` (closures now rejected). A smoke test of about 30 stdlib modules (json, re, textwrap, argparse, enum, ast, decimal, csv...) gives the same output under Ni as under CPython. Open backend and core items: B6–B8, B10, C2, C3.

Later the same day: A19 (compact immediates), B7 (a value wanted in two registers at unify), B8 (guarded), F19/F3 (keyword and star calls, 300-argument calls), and **D10: merge points from control flow** (`27f066d`). D10 needed B15 and a fix for popping jumps back to a loop head, which otherwise compiled each back-edge afresh. Same speed on hot loops; compile-heavy code (the smoke test) about 2× faster with less memory. Guardrails (D21): `tools/check.sh` runs everything, including `tests/compile_bounds.py`, which bounds how much code is compiled (`ni._stats()`). `tools/demo.py` runs about 2× faster under Ni than plain.

Then CPython's own tests (`tests/cpython_tests.py`). All 14 modules now give the same results under Ni as plain. Fixed on the way: F5 (math module metas re-ported from 3.7), C9 (`**kwargs` reference across a call), C10 (crash at exit), C11 (`sys._getframe` now sees compiled frames, partly), C12 (recursion depth counted; endless recursion segfaulted), C13 (per-code data keyed by value: `test_json` aborted). C15 (functions calling `locals()`/`eval()` and similar are now interpreted). Then 36 more modules (`test_list` to `test_userstring`): fixed C16 (generator names), F20 (exception handled inside `finally`), C17 (tuple identity across calls), and traceback frames (C11); `test_tuple` followed from C17. Found and left open: C14 (compiled code is never freed). Now 49 of 50 modules match; `test_re` differs (C11: `warnings` `stacklevel` skips compiled callees).

Then the rest of the baseline: D15 dead-local deletion (`458ef80`), inlining again with B6 fixed (`085392a`), C14 partly (`32a249e`, `32f4c3a`: compiled code objects can die; globals borrowed, D23), F21.

**Virtual ints** (`fdbaa88`, QUESTIONS 26 step 1): + - * unary - bool and comparisons follow 3.7's one-digit fast paths on a machine word. Then **virtual bools** (`3702e3f`): a comparison result stays a condition until used, which needed B8 and B16 fixed. Release build, against CPython 3.7:

| | CPython | Ni before | virtual ints | + virtual bools |
|---|---|---|---|---|
| count_primes | 0.148s | 0.070s (2.1×) | 0.045s (3.3×) | 0.042s (3.5×) |
| sieve | 0.137s | 0.071s (1.9×) | 0.030s (4.6×) | 0.026s (5.3×) |
| collatz_longest | 0.991s | 0.496s (2.0×) | 0.383s (2.6×) | 0.324s (3.1×) |
| mandel_art | 0.200s | 0.045s (4.4×) | 0.023s (8.7×) | 0.020s (10×) |
| fib(27) | 0.025s | 0.012s (2.1×) | 0.010s (2.5×) | 0.009s (2.8×) |

**First generated code** (`64f88e7`, QUESTIONS 26 steps 2–3): `tools/metac/gen_long.py` reads `Objects/longobject.c`. It generates + - * unary - and ~ (5 of 22 int slots), with the same calls as the hand-written versions it replaces. It logs why it skips the rest (`METACOMPILER.md`). Then the floats (`749985f`): `tools/metac/gen_float.py` generates + - * / % unary - abs and bool (8 of 14 slots) by copying the double-only C into helpers. `%` and `bool` are new.

**Builtin metas** (`0feefaf`, after the NeedForSpeed branch's, translated from 3.7's `bltinmodule.c`): `hash`, `min`/`max` (two or more arguments) and `pow`. They needed METH_FASTCALL dispatch (F23: in 3.7 `divmod`, `pow`, `round`, `sum`, `isinstance` and `next` are FASTCALL, so no meta for them could run) and F24. Then `isinstance` (`3494aba`: folded to a constant for static types and tuples of them; heap types, `__instancecheck__` and run-time tuples ask CPython), `round(x)` and the type calls `int(x)`, `float(x)`, `type(x)` (`cea78eb`, a meta for `type_call`), and `sum` over a tuple of known length (`d487bbb`). Both `isinstance` and `type_call` only make constants of static types (`96aaec2`, F25: a promoted heap class stayed alive). [run] Release build: an `isinstance` dispatch loop 0.045s → 0.030s (CPython 0.033s); an `int()`/`round()`/`float()` loop 0.120s → 0.062s (CPython 0.121s); a tuple `sum` loop 0.042s → 0.015s (CPython 0.042s).

**Benchmarks** (`tools/bench.py`, release build, best of 3, fresh process per mode, results checked equal) [run], 2026-11-16 at `0fc9420`: count_primes 3.4×, sieve 3.5×, collatz 3.2×, digit_sums (divmod) 2.0×, mandel 9.7×, quantise (int/float/round) 2.8×, stats (min/max/sum/round/abs) 2.7×, shapes (isinstance dispatch) 1.0×, the same with builtins in locals 4.6×, hashing 1.6×, nqueens 2.5×, nbody 2.4×, word_counts 1.2×; total 2.6×. The gap between shapes and shapes_locals is global lookups: every LOAD_GLOBAL is a run-time dict lookup (`cimpl_load_global`, D23), and in an `isinstance(x, int)` loop they cost about 20 times the work they guard. [run]

**Cached globals** (`7a3a109`, D25): LOAD_GLOBAL is a constant behind a version-tag guard. [run] Release build, `tools/bench.py`: total 2.6× → 3.5×; shapes 1.0× → 5.9× (it now beats shapes_locals, 5.0×), quantise 2.8× → 7.0×, stats 2.7× → 6.8×, digit_sums 2.0× → 3.7×, hashing 1.6× → 2.2×, nbody 2.4× → 2.8×. word_counts stays 1.2× (str methods and dicts are residual calls).

**Bytecode slice** (`69900ac`, D26): 39 of `pycompiler.c`'s opcode cases (stack shuffles, `LOAD_CONST`/`LOAD_FAST`/`STORE_FAST`, unary, binary and in-place arithmetic, `STORE_SUBSCR`, `GET_ITER`) are now generated by `tools/metac/gen_ceval.py`. The input is `tools/metac/bytecodes37.c`, which describes 3.7's opcodes in 3.12's instruction DSL, and CPython's own parser reads it. The descriptions are checked against 3.7, and the same generator translates 13 of 3.12's 186 instructions unchanged (`METACOMPILER.md`). [run] The new test found F28 (Python 2 error messages).

**Virtual range** (`da1539f`, A8 restored): `for i in range(...)` with one-digit int bounds is a register loop (`prangeobject.c`). [run] Release build, `tools/bench.py` (with new target benchmarks, `b1a3c7d`): total 3.0× → 3.4×; sieve 3.6× → 5.8×, nqueens 2.9× → 7.7×, nbody 2.7× → 4.1×. range_loops stays 2.1× because `^ & | << >>` on ints are still residual. Not yet sped up: generators (1.1×), instances (`points`, 1.0×), str work (2.0–2.8×).

## 2026-11-16: first indexing pass (at `e1fd808`)

Done:
- Every `.c`/`.h` in `ni/` and `tools/` has been read in full. The 29 complex files (core, virtual objects, backend, buggy translations) got a comment written for that file at the top, and two single-site bugs got comments at the site. Simple files are untouched (D9). Comment-only change: verified by comparing each file with comments stripped against HEAD (all 31 identical).
- `doc/MAP.md`: coverage ledger, one hand-maintained row per file.
- `doc/KNOWN_BUGS.md`: every suspected bug in one place, including a direct review of the memory-access code (`ni/x64/v_mem_mov.h`, `vcompiler.c` field functions).
- `doc/GOAL.md`, `GLOSSARY.md`, `ARCHITECTURE.md` (lifecycle, mixed-file breakdowns, backend boundary), `PATTERNS.md` (P01–P26 + generator rules), `REFERENCES.md`, `BUILD.md`, `DECISIONS.md`, `QUESTIONS.md`.
- `doc/history/removed-files.md` and `doc/history/removed-mechanisms.md`: what was taken out of Psyco during the port, with commits and reasons.
- `CLAUDE.md` at the repo root as the entry point; a status note added to `README.md`.

How it was done: the core (`vcompiler`, `dispatcher`, `codegen`, `psyfunc`, `mergepoints`, `eval_hook`, `Python/pycompiler`, `frames`) was reviewed directly. `Objects/` (in two groups), `x64/` and the two history passes were done by parallel sub-reviews, then merged and spot-checked. At the time, bugs came from reading only; the baseline section above records what has since been confirmed by running.

Not done / next candidates:
- The sub-review reports (per-function tables, virtual-object layouts, bug lists with evidence) are kept as point-in-time records in `doc/reviews/2026-11-16/` (including the brief the reviewers worked from). They aren't maintained; re-check against the code before relying on them.
- Diff each translated file against the CPython 3.7 source it mirrors, to measure drift precisely.
- Owner answered Q1–6 (DECISIONS D10–D15).
- Start the meta-compiler design: the per-slot signature table (Q7) and the `Cf*` derivation (Q8).

**Int bit operations and division** (A1-adjacent): `& | ^ << >> // % divmod` on one-digit ints are word operations (`plongobject.c:plong_bitop`, `plong_divmod_op`; a constant power-of-two divisor is a shift or mask, other divisors a pure C helper; `divmod` gives a virtual tuple). [run] Release build: total 3.4× → 3.9×; digit_sums 4.1× → 20.6×, collatz 3.1× → 5.7×, hashing 2.7× → 3.9×, quantise 7.5× → 11.3×.

**pyperformance** (`tools/pyperf37.py`): 29 benchmarks from pyperformance 1.11.0's pure-Python set, run directly on 3.7. [run] Release build, after `b782b2b`, two full runs: geometric mean **1.35×** (both runs) (CPython 3.15's JIT: about 1.07× on x86-64, `doc/CPYTHON_JIT.md`). Best: spectral_norm 6.9×, nbody 3.5×, scimark_fft 2.5×, scimark_monte_carlo 2.4×, pyflate 2.0×, sparse_mat_mult 1.9×, fannkuch 1.8×, richards 1.7×. Slower than CPython (0.83–0.96×): comprehensions, deepcopy (3), generators, richards_super, nqueens, mdp, hexiom, coroutines. Single runs vary by up to about 0.4× on the shortest benchmarks (the first table, at 1.26×, had nbody at 0.92×; not reproduced in 60 later runs).

**Closures** (B7, partly): code with cell or free variables compiles when entered from its own frame (LOAD_CLOSURE, LOAD_DEREF, STORE_DEREF; cell arguments). Calls to closure code from compiled code stay real calls, and a call site on run-time functions promotes the code object before the function, so new lambdas don't each make a specialisation. `__class__` (zero-argument `super()`) stays interpreted. [run] pyperformance 1.35× → 1.40× (chaos 1.05× → 1.81×, hexiom 0.91× → 1.52×, comprehensions 0.88× → 1.08×); `tools/bench.py` 4.0×. Visible differences accepted: C18, C19.

**`*` unpacking** (`f(a, *b)`, `(*a, *b)`, `[*a, *b]`): BUILD_TUPLE_UNPACK_WITH_CALL, BUILD_TUPLE_UNPACK and BUILD_LIST_UNPACK compile (a virtual tuple when every part is a known tuple). It was the only opcode keeping `copy.deepcopy` interpreted. [run] pyperformance deepcopy 0.84× → 1.94×, deepcopy_memo 0.84× → 2.41×, deepcopy_reduce 0.86× → 1.52×.

**Zero-argument `super()`** compiles: code reading `__class__` only runs compiled from its own frame, which holds the first argument and the cell, unless the code rebinds its first argument (then interpreted). [run] richards_super 0.90× → 1.37×; CPython's `test_super`, `test_class`, `test_scope` pass with Ni.

**Keyword-only arguments** compile at the frame entry (CPython has filled them in); calls to such code from compiled code stay real calls. [run] CPython's test_keywordonlyarg, test_dataclasses, test_functools, test_enum, test_json unchanged with Ni.

**F31 and generators** (`e657847`, `21ae5a3`): a respawn replay could make a different LOAD_GLOBAL decision from the original compile (`changing_globals` wasn't in snapshots), which crashed test_base64; found with `tools/nianalyser/crash_report.py` (D28). The hook now leaves generator frames at once: pyperformance generators 0.88× → 0.99×, coroutines 0.96× → 1.01× [run]. test_coroutines and test_asyncgen added to `tests/cpython_tests.py`. Options for compiling generators: `doc/GENERATORS.md`.

**Comprehension calls and generator expressions** (`3ccd317` and the next commit): calls to comprehensions and local lambdas with free variables are compiled calls with the cells passed in; generator expressions consumed by sum/any/all/list/tuple/set run fused, with no generator (GENERATORS.md option B). [run] pyperformance comprehensions 1.09× → 1.81×, hexiom 1.59× → 1.98×, mdp 1.06× → 1.20×.

**f-strings, sets, F22; benchmarks at `235e085`** [run], release build: f-strings (FORMAT_VALUE, BUILD_STRING; namedtuple's `_make`/`_replace` hold them), set displays and comprehensions, and genexprs fused with `min`/`max` now compile. `tools/interpreted.py` lists what still runs interpreted per benchmark: only generator functions are left in the hot paths (nqueens, scimark_lu, generators, coroutines; GENERATORS.md option A, QUESTIONS 33). pyperformance geometric mean **1.64×** (29 benchmarks; 1.56× before today's comprehension and genexpr work), bench.py total 4.2×. Run-to-run noise on this machine is about ±7% per benchmark (richards 1.63–1.81× across six runs of two builds).

**Attribute lookup (A9): merged** (`d70e7f7`, D30) [run]. Attribute lookup is specialised on the type behind a `tp_version_tag` guard, and `obj.m()` is a direct compiled call. pyperformance geometric mean 1.64× → 1.76× (richards 2.09×, deltablue 1.88×, go 1.88×, float 1.88×, chaos 2.15×); compile time and code size don't grow (COMPILE_COST.md). The crash it had before merging was found by the replay check (`796222d`): `PsycoObject_GetAttr` tested `tp_getattro` before the guard, so a replay after a class gained `__getattr__` took another path and its respawn points shifted by one. The "lambda leak" was the harness, whose leak check started after one warm-up call (`be2ad76`). F33 and F35 were found on the way.

