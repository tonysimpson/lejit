---
written: 2026-11-17
status: measurements; rerun with tools/compilebench.py
---
# Compile cost

D29 says backend changes are judged on compile time as well as run time. This note records the baseline (QUESTIONS 35).

## Method

- **Counters** (`f598d22`, `ni._stats()`): time and count for each way into the compiler from run time, plus bytes of code emitted (proxies included). Only the outermost entry counts, so nested compiles aren't counted twice. [read]
  - `analyse`: building a code object's merge points and liveness (`mergepoints.c:_build_merge_points`);
  - `call`: first compile of a function from the eval hook;
  - `pause`: resuming a coding pause (`vcompiler.c:do_resume_coding`);
  - `respawn`: compiling a rare branch by replay (`dispatcher.c:do_respawn`);
  - `promotion`: compiling for a newly seen value (`do_promotion_internal`, `detected_megamorphic_pyobj_site`). The cache lookups before them aren't counted.
- **Cost of the counters:** two `clock_gettime` calls per entry, about 50 ns. That's 0.15 ms over go's 4,400 entries.
- **`tools/compilebench.py`:** each pyperformance benchmark in a fresh process, CPython then Ni.
  - **first:** one iteration, which includes Ni's compiling;
  - **warm:** the best of five calibrated runs, per iteration;
  - **compile:** over the whole run;
  - **even:** iterations until Ni is ahead overall, which is compile time / (CPython warm − Ni warm).
- **Cross-check:** richards' first iteration is 1.4 ms slower than its second, and the counters account for 0.85 ms of that. [run]

## Baseline, `baseline-3.7` at `f598d22`, release build [run]

| benchmark | CPython first | CPython warm | Ni first | Ni warm | compile ms | code KB | compile share of Ni first | even |
|---|---|---|---|---|---|---|---|---|
| chaos | 93.5 | 73.1 | 39.1 | 38.2 | 0.41 | 83 | 1% | 1 |
| comprehensions | 0.03 | 0.02 | 0.19 | 0.01 | 0.29 | 63 | 155% | 35 |
| coroutines | 24.4 | 24.5 | 23.0 | 23.4 | 0.03 | 5 | 0% | 1 |
| deepcopy | 0.36 | 0.34 | 0.52 | 0.17 | 0.42 | 70 | 81% | 3 |
| deepcopy_memo | 0.04 | 0.04 | 0.05 | 0.01 | 0.06 | 7 | 111% | 3 |
| deepcopy_reduce | 0.01 | 0.00 | 0.08 | 0.00 | 0.11 | 21 | 140% | 116 |
| deltablue | 4.55 | 4.41 | 4.60 | 2.83 | 1.51 | 355 | 33% | 1 |
| fannkuch | 276 | 273 | 151 | 148 | 0.35 | 83 | 0% | 1 |
| float | 68.1 | 65.8 | 46.2 | 42.9 | 0.13 | 20 | 0% | 1 |
| create_gc_cycles | 0.57 | 0.49 | 0.62 | 0.52 | 0.14 | 24 | 23% | – |
| gc_traversal | 2.07 | 1.80 | 1.93 | 1.84 | 0.11 | 12 | 6% | – |
| generators | 27.9 | 28.3 | 30.8 | 29.5 | 0.09 | 15 | 0% | – |
| go | 155 | 154 | 120 | 103 | 13.95 | 1597 | 12% | 1 |
| hexiom | 6.61 | 6.51 | 10.79 | 3.30 | 7.02 | 984 | 65% | 3 |
| mdp | 2136 | 2259 | 1753 | 1736 | 1.36 | 265 | 0% | 1 |
| meteor_contest | 68.2 | 67.7 | 49.9 | 48.5 | 0.69 | 101 | 1% | 1 |
| nbody | 67.4 | 64.8 | 19.9 | 19.6 | 0.31 | 55 | 2% | 1 |
| nqueens | 69.1 | 68.7 | 70.7 | 70.1 | 0.02 | 4 | 0% | – |
| pyflate | 398 | 402 | 217 | 210 | 3.03 | 532 | 1% | 1 |
| raytrace | 310 | 310 | 266 | 265 | 1.82 | 361 | 1% | 1 |
| richards | 44.2 | 42.4 | 27.9 | 25.4 | 0.62 | 153 | 2% | 1 |
| richards_super | 56.2 | 55.7 | 41.0 | 39.6 | 0.63 | 151 | 2% | 1 |
| scimark_fft | 177 | 182 | 75.5 | 75.0 | 0.99 | 219 | 1% | 1 |
| scimark_lu | 107 | 107 | 101 | 101 | 0.36 | 65 | 0% | 1 |
| scimark_monte_carlo | 69.2 | 68.1 | 29.7 | 29.3 | 0.09 | 16 | 0% | 1 |
| scimark_sor | 120 | 117 | 99.8 | 99.1 | 0.15 | 33 | 0% | 1 |
| scimark_sparse_mat_mult | 2.80 | 2.83 | 1.50 | 1.44 | 0.18 | 33 | 12% | 1 |
| spectral_norm | 77.7 | 77.0 | 11.3 | 11.1 | 0.15 | 32 | 1% | 1 |
| unpack_sequence | 0.01 | 0.01 | 0.17 | 0.01 | 0.29 | 185 | 170% | 83 |

All times are in ms. In total, compiling took 35 ms for 5.4 MB of code, about **157 KB per ms**. A "–" in the even column means Ni isn't faster when warm.

## What it shows [run], [inferred]

- **Compiling is cheap.** Ni pays for itself within 1–3 iterations on almost everything, including the first iteration of most benchmarks. That supports D29. The exceptions are iterations well under a millisecond: comprehensions, deepcopy_reduce, unpack_sequence. Even there, Ni pays back in under 0.3 ms.
- **Promotion dominates.** It accounts for about 70% of compile time and most entries (go: 3,277 promotions, 625 pauses, 500 respawns).
- **Code size is the cost to watch, more than time.** go emitted 1.6 MB and hexiom 1 MB (since D32 and D33: 0.43 and 0.42 MB, CODE_SIZE.md). hexiom's first iteration is 1.6× slower than CPython's: 7 ms of compiling, then 2× faster warm. Memory and the instruction cache are the likely limits, not compile speed. The counters can't see the instruction cache; `perf stat` with icache events could.
- **Not measured:** time spent in merge-point lookup (`psyco_compatible`) while running and on the promotion cache-hit path. Both are run-time dispatch, not compiling.

## A9 (`a9-getattr` `aaea91d`, rebased on `b020746`) [run]

A9 specialises attribute lookup on the type. Overall it doesn't make compiling dearer: 32.9 ms against 35.3 ms in total, and 5.3 MB of code against 5.4 MB, at 162 KB per ms. Rows that changed by more than noise:

| benchmark | Ni warm, base → A9 (ms) | compile ms, base → A9 | code KB, base → A9 |
|---|---|---|---|
| deltablue | 2.83 → 2.26 | 1.51 → 2.01 | 355 → 507 |
| deepcopy_reduce | 0.00 → 0.00 | 0.11 → 0.18 | 21 → 36 |
| go | 103 → 82 | 13.95 → 12.92 | 1597 → 1448 |
| hexiom | 3.30 → 2.92 | 7.02 → 5.98 | 984 → 859 |
| pyflate | 210 → 180 | 3.03 → 2.57 | 532 → 463 |
| raytrace | 265 → 234 | 1.82 → 1.47 | 361 → 348 |
| richards | 25.4 → 20.8 | 0.62 → 0.52 | 153 → 144 |

Where a method call becomes a direct compiled call, less generic code is emitted. deltablue grows instead: more of it is now specialised per type.
