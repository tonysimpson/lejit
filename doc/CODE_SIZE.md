---
written: 2026-11-17
status: measurements; rerun with tools/codesize.py
---
# Where the emitted code goes

Why Ni emits so much code, and how much of it is the same code emitted more than once. Release build with `NI_CODESTATS=1` at `67a9c29`, each benchmark run until it compiles nothing new. [run]

## Method

- **`NI_CODESTATS=1`** (setup.py) builds in counters; `ni._codestats()` returns and resets them. Off by default, so normal builds don't pay for them.
- **By buffer kind** (`codemanager.h:psyco_codestats_buffer`): bytes of each code buffer when it is closed, by what it holds:
  - `block`: a block compiled from a merge point or a function entry;
  - `promotion`, `megamorphic`: code compiled for a newly seen value (`dispatcher.c`);
  - `respawn`: the stub behind every run-time condition, which compiles the rare branch if it is ever taken;
  - `respawned`: code compiled when a stub fires;
  - `stubs`: 4 KB pool chunks holding the 15-byte respawn and pause stubs (D32), counted whole when taken;
  - `coding_pause` (until `d51860c`), `unify`, `glue`: pause stubs, state-matching code at merge points, start-up code.
- **By instruction** (`pycompiler.c:codestats_begin`): the bytes from the start of one bytecode instruction to the start of the next, per copy, and the copy itself. An instruction's error paths and the end of a block count with it; stubs and replays don't.
- **Identical copies** (`tools/codesize.py -d`, needs capstone): copies of the same instruction that disassemble the same once branch and call targets are replaced; a second level also ignores 64-bit constants (per-site guard structs, shared keys, type pointers).

## Results

| | richards | deltablue | go |
|---|---|---|---|
| code emitted | 197 KB | 694 KB | 1636 KB |
| respawn stubs | 52% (1,495 × 71 B; 15 fired) | 49% (5,035 × 69 B; 207 fired) | 46% (11,256 × 68 B; 505 fired) |
| promotion code | 31% | 31% | 36% |
| times each instruction is compiled, on average | 2.3 | 4.3 | 13.5 |
| identical copies, jump targets aside | 14% of instruction code | 16% | 42% |
| identical copies, 64-bit constants also aside | 29% | 22% | 43% |
| identical copies made by a promotion | 91% | 85% | 81% |

### Respawn stubs are half the code

Every run-time condition (`runtime_condition_t/f`, `dispatcher.c:psyco_prepare_respawn_ex`) gets its own stub buffer straight away: a call into `do_respawn` plus a `respawn_t`. Only 1–4% of them ever fire. [run]

### The same code, compiled again

- **go: `Board.useful` is 57% of go's instruction code**, with up to 298 copies of one instruction. `ZobristHash.dupe`, inlined at its end, has 138 copies of its first instruction but only 5 distinct ones. `useful` counts with five locals that start as the constant 0 (`empties = opps = weak_opps = neighs = weak_neighs = 0`). [run]
  - My guess is that each combination of "still the constant 0" or "run-time" makes a different state at the loop head, so the loop and everything after it are compiled again for each one. The combination only changes the final `return` expression. [inferred]
- **richards: methods of the base class `Task` are compiled once per subclass** (`runTask`: 7 copies, 2 distinct apart from constants). The code is the same except for the type and shared-keys pointers in its guards. [run]

### What differs between the copies (go, `tools/codesize.py go -s`)

`vcompiler.c:cs_state` logs the state each new block at a merge point starts from. In go, one merge point in `Board.useful` (@310) started 65 blocks. The slots that differ are the five counters and `strong_neighs`/`strong_opps`, each in one of three forms: [run]

- a constant `int` object (`0`);
- a virtual int whose value is a known constant (`1`, `2`);
- a virtual int whose value is run-time.

The virtual ints also differ in whether their type field is fixed (promoted) or not. `psyco_compatible` turns differing plain constants into run-time values (a partial match), but a constant object against a virtual int is a structural difference: no match, so a new block. Nothing caps the blocks per merge point. [read] (`dispatcher.c:psyco_compatible`, `vcompiler.c:psyco_compile`)

## Options

1. **Small respawn stubs.** Done (`2f8d00f`, D32), results below. Each stub was its own code buffer: a call into `do_respawn` with its arguments, then a `respawn_t` (about 30 bytes of code and 32 of data, aligned). One `call` to a shared trampoline that finds its `respawn_t` by return address in a table would be 5 bytes of code. It changes nothing about what is compiled, so it is the safest large win: up to about 45% of all code, and less compile time spent creating buffers. [inferred]
2. **One form for ints at merge points.** Done (`1c61916`, `5e37663`, D33), results below. Before `psyco_compatible`, write a constant `int` object as a virtual int with a constant value, and treat a virtual int's type field the same whether it is fixed or not (it is always `int`). Then go's counters differ only in constants, which the existing partial match already generalises. Narrow and safe; the same applies to floats and bools. [inferred]
3. **A cap per merge point.** After N blocks at one merge point, generalise the new state before matching: plain and fixed constants become run-time, and virtual values are computed. Forgetting facts is always safe; code after it may promote again. This is the general backstop for explosions the first two don't foresee. N is a policy choice, measured under D29. [inferred]
4. **Copies per type with identical code** (richards' `Task` methods). These are real specialisation: the guards hold a different type and different shared keys. Sharing them needs a guard that accepts a set of types. That is research, and it is worth doing only after 1–3. [inferred]

The general principle behind 2 and 3: states should be compared by what code after them can exploit, not by how a value happens to be represented. Psyco's "fixed" flag (a promoted value something already depends on) is a step towards that; 2 removes differences of representation, and 3 bounds the rest.

## Respawn stubs made small (`2f8d00f`, D32) [run]

A stub is now `mov r11, record; jmp psyco_stub_trampoline` (15 bytes) in a 4 KB pool chunk (`dispatcher.c:psyco_new_stub`). The record is on the heap and is freed once the branch is compiled. One shared trampoline (`x64/iprocessor.c:write_stub_trampoline`) aligns the stack and calls `psyco_stub_enter`. Same session, release build, before (`2c5514e`) and after:

| benchmark | code KB | compile ms | Ni warm (ms) |
|---|---|---|---|
| richards | 200 → 116 | 0.62 → 0.57 | 12.1 → 12.2 |
| deltablue | 700 → 430 | 2.48 → 2.37 | 1.65 → 1.66 |
| raytrace | 438 → 267 | 1.76 → 1.70 | 192 → 189 |
| go | 1650 → 1057 | 13.2 → 13.1 | 75.0 → 74.5 |
| hexiom | 1007 → 631 | 6.36 → 6.18 | 2.23 → 2.23 |
| pyflate | 516 → 296 | 2.72 → 2.69 | 144 → 143 |
| chaos | 98 → 65 | 0.49 → 0.47 | 27.5 → 27.8 |

pyperformance geometric mean: 1.93×.

A first version entered the trampoline with `call` and popped the return address instead of returning. go was 8% slower (75 → 81 ms): branch mispredictions rose 19% per iteration, because the unmatched `call` leaves the return-address predictor one entry off, and `ret`s mispredict after it. In go, a stub fires on every `Board.useful` call (the guard on the `TIMESTAMP` global, which `do_changed_global` resumes). Entering by `jmp` with the record in R11 fixed it. [run] (`perf stat`)

## Coding pauses on the same stubs (`d51860c`, D32) [run]

A coding pause (a branch not compiled yet, a full buffer, too many locked buffers) had its own code buffer with an inline call into `do_resume_coding`: 67–68 bytes each, and go has 942 of them. It now takes a stub from `dispatcher.c:psyco_new_stub` like respawn, and `vcompiler.c:do_resume_coding_body` frees the record once the continuation is compiled (the only way in is the one jump it then repoints). Pauses never pass a run-time argument (`extraarg` is `SOURCE_DUMMY`), so the trampoline needs nothing new. Same session, release build, before (`9771e71`) and after; timings indicative only, other benchmarks ran on the machine at the same time:

| benchmark | code KB | compile ms | Ni warm (ms) |
|---|---|---|---|
| richards | 115.9 → 111.4 | 0.63–0.71 → 0.58–0.61 | 12.3 → 12.3 |
| deltablue | 429.5 → 415.7 | 2.39 → 2.44–2.45 | 1.64–1.69 → 1.65–1.66 |
| go | 1056.8 → 1006.3 | 13.4–13.5 → 12.9–13.8 | 75.0–77.2 → 74.5–75.6 |
| hexiom | 630.9 → 599.4 | 6.0–6.2 → 6.1–6.2 | 2.24–2.26 → 2.25–2.28 |
| pyflate | 295.8 → 277.0 | 2.7–2.9 → 2.7–2.9 | 145–148 → 149–154 (noise: see below) |

In the `NI_CODESTATS` build, go's `coding_pause` row (942 buffers, 62 KB) goes and the `stubs` row grows by four chunks (164 → 180 KB); emitted code 1048 → 1002 KB, compiles per instruction unchanged (19,534). `perf stat -e instructions,branch-misses tools/profile_bench.py go 4`, two runs each: branch misses 47.78M, 47.81M before and 47.85M, 47.79M after; instructions 79.4G, 78.9G before and 77.4G, 78.4G after (noisy). pyflate, same way: branch misses 69.7M, 66.7M before and 68.4M, 65.3M after; elapsed 4.51, 4.56 s before and 4.60, 4.52 s after. A few instructions' byte counts move by about 0.1 KB: spans that cross a buffer switch are counted when the next buffer happens to lie above the last, and buffer placement changed (`pycompiler.c:codestats_flush`).

## One form for ints at merge points (`1c61916`, `5e37663`, D33) [run]

Two changes, both in `dispatcher.c`:

- `canonical_int` (`1c61916`): before `psyco_compatible` compares, and so before the snapshot is frozen, a local holding a constant small int (-5 to 256) or bool, not fixed, becomes a virtual int with that constant value. Computing it again gives the same object (`PyLong_FromLong`'s small ints, `PyBool_FromLong`), so identity is kept. Only locals: LOAD_CONST's constants carry no `SkFlagPyObj`, and the stack may hold other things.
- `compatible_vinfo` (`5e37663`): in the value word of a virtual int or bool, a constant against a run-time value in the snapshot is a partial match (the constant is un-promoted), as two different constants already were. The type field needed nothing: equal constants match whether fixed or not.

The first alone was not enough: the forms became "virtual int, value known" and "virtual int, value run-time", and a known value never matched a run-time one, so each combination of "still 0" and "run-time" across go's seven counters was still its own block. The type field's fixed flag turned out not to matter. [run]

`Board.useful` @310: 65 blocks → 52 (first change) → 13 (both). `NI_CODESTATS` build (`tools/codesize.py`):

| benchmark | code KB | instructions compiled | blocks at merge points |
|---|---|---|---|
| go | 1048 → 718 → 422 | 19,534 → 12,754 → 7,183 | 119 → 107 → 67 |
| richards | 114 → 114 → 114 | 2,006 → 2,005 → 2,005 | 57 → 57 → 57 |
| deltablue | 426 → 433 → 433 | 6,142 → 6,224 → 6,224 | 106 → 107 → 107 |

Release build, `tools/compilebench.py`, same session (timings indicative: other benchmarks ran on the machine):

| benchmark | code KB | compile ms | Ni warm (ms) |
|---|---|---|---|
| go | 1057 → 426 | 13.1 → 3.0 | 75.2 → 74.7 |
| hexiom | 631 → 423 | 5.98 → 3.45 | 2.31 → 2.19 |
| pyflate | 296 → 232 | 2.65 → 1.77 | 145 → 145 |
| deltablue | 430 → 433 | 2.42 → 2.46 | 1.66 → 1.63 |
| richards | 116 → 116 | 0.59 → 0.60 | 12.1 → 12.2 |
| raytrace | 267 → 267 | 1.65 → 1.60 | 186 → 190 |
| chaos | 65 → 65 | 0.45 → 0.44 | 27.1 → 27.3 |

`perf stat -e instructions`, whole process: go's first iteration 1.91 → 1.75 G (−8%); steady state richards −3%, go, deltablue, raytrace, chaos and hexiom within ±0.7%, pyflate +1.2% (5 iterations, repeated). A counter generalised to run-time loses facts the specialised copy folded, so warm code can be slightly slower; we judged smaller code and faster compiles worth it (D29).

deltablue compiles 1.3% more (6,142 → 6,224 instructions) after the first change; not investigated (QUESTIONS).

