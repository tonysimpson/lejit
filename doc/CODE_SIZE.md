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
- **Promotion sites** (`tools/codesize.py -p`, `ni._promostats()`): see "Promotions" below.
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

The virtual ints also differ in whether their type field is fixed (promoted) or not. `psyco_compatible` turns differing plain constants into run-time values (a partial match), but a constant object against a virtual int is a structural difference: no match, so a new block. Nothing capped the blocks per merge point until D40. [read] (`dispatcher.c:psyco_compatible`, `vcompiler.c:psyco_compile`)

## Options

1. **Small respawn stubs.** Done (`2f8d00f`, D32), results below. Each stub was its own code buffer: a call into `do_respawn` with its arguments, then a `respawn_t` (about 30 bytes of code and 32 of data, aligned). One `call` to a shared trampoline that finds its `respawn_t` by return address in a table would be 5 bytes of code. It changes nothing about what is compiled, so it is the safest large win: up to about 45% of all code, and less compile time spent creating buffers. [inferred]
2. **One form for ints at merge points.** Done (`1c61916`, `5e37663`, D33), results below. Before `psyco_compatible`, write a constant `int` object as a virtual int with a constant value, and treat a virtual int's type field the same whether it is fixed or not (it is always `int`). Then go's counters differ only in constants, which the existing partial match already generalises. Narrow and safe; the same applies to floats and bools. [inferred]
3. **A cap per merge point.** Done for ints (`932f416`, D40), results below. After N blocks at one merge point, generalise the new state before matching: plain and fixed constants become run-time, and virtual values are computed. Forgetting facts is always safe; code after it may promote again. This is the general backstop for explosions the first two don't foresee. N is a policy choice, measured under D29. [inferred]
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

## A cap per merge point (`932f416`, D40) [run]

### How many blocks each merge point gets

`tools/codesize.py -s` on all 23 pyperformance benchmarks, `NI_CODESTATS=1` build at `f27cfcb` with the cap off. From this commit `vcompiler.c:mp_new_block` also logs a block that compilation flows into from a merge point (`psyco_compile` with `continue_compilation`), which `cs_state` used to miss, so the counts above this section are low: `Board.useful` @310 had 16 blocks after D33, not 13.

| blocks at the merge point | 1 | 2 | 3 | 4–7 | 8–15 | 16+ |
|---|---|---|---|---|---|---|
| merge points | 520 (50%) | 265 (26%) | 77 | 117 | 43 | 11 |

1,033 merge points started 2,543 blocks. Most of the 2s are loop heads: the first iteration with the counters still constant, then a general one. The largest: meteor_contest `solve` @66 (34) and @36 (30), hexiom `constraint_pass` @104 (32), pyflate (24), go `Board.useful` and deltablue `add_propagate` (20). Twelve benchmarks have no merge point with more than 3.

What differs between the blocks of one merge point, over 13 benchmarks (`forms` of each local that varies, weighted by the extra blocks; a tally of what co-varies, not of causes):

| forms the local takes | extra blocks it appears in |
|---|---|
| unbound (fixed 0) / run-time | 1,498 |
| virtual int with a known value / with a run-time value | 1,371 |
| run-time / run-time with a promoted type | 1,223 |
| run-time with a promoted type / another virtual (float, empty list) | 224 |
| run-time with a promoted type / virtual int with a run-time value | 147 |

Only the second is a fact the cap may forget. Unbound locals and promoted types are fixed (`SkFlagFixed`): code relies on them, and forgetting a promoted value at the merge point where it was promoted makes the promoted state match the one it came from, a loop. The others are two forms of one value (QUESTIONS 38).

### What the cap forgets

Past `MP_BLOCK_CAP` blocks at a merge point, a state that matches no block exactly has the known value of each virtual int or bool in its locals made run-time (`dispatcher.c:psyco_forget_int_values`), and is compared again. Three versions were measured (instructions compiled, `codesize.py`):

- **Every non-fixed constant in the locals, N = 4–16.** hexiom, pyflate and scimark compile 9–17% less, but meteor_contest compiles 5% more at N = 6 and 16% more at N = 16: a constant function in a local (`bisect`) becomes run-time, the next call promotes it again, and that is one more form at the merge point. At N = 4, go +7% and deltablue +5%: deltablue's extra blocks are copies of `remove_propagate_from` rejected only by the alignment test (19 → 43 such blocks), because constants made run-time are passed in registers or on the stack and move its callees' return-address slot.
- **Only the values of virtual ints.** No losses, but the case in `tests/cases/mp_cap.py` still started 41 blocks at its loop head with a cap of 6, all with every counter run-time. `a = b = c = d = 0` leaves four locals sharing one virtual int, and a run-time value shared in a snapshot never matches separate ones (`compatible_vinfo`), so each pattern of sharing was a block. A value made run-time also kept its sign (`RunTime_NonNeg`), which a later possibly-negative value can't match.
- **As the second, with each shared small int given its own virtual int and the sign forgotten** (the version kept). That test case: 447 code buffers → 82, 64 blocks at the loop head → 8. Unsharing is only done for small ints and bools, whose computed objects are the same either way, so `a is b` doesn't change.

Virtual objects are never computed: computing a virtual int calls `PyLong_FromLong` with an error check, a respawn point inside the generalisation, which a replay starting from the next block's snapshot would never reach.

### Choosing N

Instructions compiled (KB of code / instructions / most blocks at one merge point), the kept version; benchmarks not listed are the same for every N:

| benchmark | no cap | N = 4 | N = 6 | N = 8 | N = 12 |
|---|---|---|---|---|---|
| hexiom | 411 / 7,442 / 32 | 340 / 5,925 / 12 | 345 / 6,120 / 17 | 364 / 6,503 / 22 | 384 / 6,863 / 22 |
| pyflate | 225 / 5,168 / 24 | 195 / 3,995 / 8 | 204 / 4,268 / 9 | 203 / 4,427 / 10 | 216 / 4,883 / 16 |
| scimark | 196 / 5,262 / 12 | 174 / 4,351 / 7 | 184 / 4,789 / 10 | 196 / 5,262 / 12 | 196 / 5,262 / 12 |
| go | 441 / 7,183 / 20 | 456 / 7,314 / 13 | 419 / 6,819 / 13 | 414 / 6,790 / 13 | 434 / 7,118 / 18 |
| fannkuch | 33 / 983 / 9 | 36 / 1,052 / 12 | 32 / 932 / 9 | 37 / 983 / 9 | 33 / 983 / 9 |
| all 23 | 2,901 / 60,800 | 2,785 / 57,416 | 2,763 / 57,690 | 2,805 / 58,727 | 2,846 / 59,871 |

N = 6: the least code, and no benchmark compiles more than with no cap. N = 4 compiles slightly fewer instructions in total but more in go (+2%) and fannkuch (+7%): forgetting after four blocks loses values that the fifth and sixth would have used. With N = 6 the merge points with 8 or more blocks go from 54 to 43, and with 16 or more from 11 to 5; what remains differs in fixed values and in forms of a value.

Release build, `tools/compilebench.py`, cap off (`-DMP_BLOCK_CAP=0`) against N = 6, two runs each, same session (timings indicative: other agents benchmark on the machine):

| benchmark | code KB | compile ms | Ni first (ms) | Ni warm (ms) |
|---|---|---|---|---|
| hexiom | 416 → 349 | 3.47, 3.48 → 2.72, 2.71 | 5.80, 5.83 → 5.04, 5.04 | 2.09, 2.10 → 2.13, 2.10 |
| go | 441 → 423 | 3.14, 3.82 → 2.83, 2.84 | 74.5, 74.9 → 71.6, 71.9 | 71.1, 68.9 → 66.4, 67.4 |
| pyflate | 227 → 206 | 1.90, 1.99 → 1.50, 1.58 | 150, 156 → 148, 151 | 147, 148 → 146, 148 |
| scimark_fft | 112 → 100 | 0.91, 0.97 → 0.81, 0.81 | 77.4, 78.1 → 77.0, 79.3 | 75.9, 74.1 → 75.5, 79.9 |
| fannkuch | 38 → 33 | 0.48, 0.34 → 0.28, 0.33 | 146, 166 → 145, 144 | 142, 151 → 143, 143 |
| all 29 | 2,960 → 2,814 | 21.6, 22.1 → 19.4, 20.0 | | |

Others are within a stub chunk (4 KB) of the same code; meteor_contest 61 → 65 KB is one more chunk with the same instructions compiled.

`perf stat -e instructions:u` per steady-state iteration ((12 iterations − 2) / 10, after 3 warm-up ones), cap off → N = 6: hexiom 40.6, 40.9 → 41.0, 41.0 M (+0.2 to +1.1%), go 1,231, 1,197 → 1,220, 1,237 M, pyflate 2,655 → 2,657 M, scimark 1,873.5 → 1,873.6 M, fannkuch 3,453 → 3,454 M, chaos 504.5 → 505.3 M, meteor_contest 647.9 → 646.8 M; deltablue (33.2 → 33.4 M) and raytrace (3,404 → 3,366 M), which compile the same code either way, show the noise: about ±1%. hexiom's first five iterations, compiling included: 424.9 → 415.8 M (−2.1%). A counter made run-time loses the folds the specialised copy had, as with D33; here it costs nothing measurable.

### The 16-byte alignment test (KNOWN_BUGS C1)

`codesize.py -s` also counts the blocks started although a snapshot matched (exactly or partially) apart from `compare_return_address_in_a_stupid_way`. With the cap off: 45 of 2,543 blocks (1.8%): deltablue 19 of 284 (6.7%), meteor_contest 8 of 116, go 6 of 255, deepcopy 4, hexiom 4, pyflate 2, raytrace 1, richards 1; none in the other 15. Unchanged with N = 6. Each is an upper bound on a block the test caused: without it the state would have joined existing code, though a partial match might still have compiled again. In deltablue 10 of the 19 are at one loop head (`add_constraints_consuming_to` @18, which has 10 blocks in all), states that differ from an existing block only in the return-address slot's alignment, presumably because the function is entered from callers at different stack depths [inferred]. Generalising more (the first version above) made it worse, 19 → 43 in deltablue, with whole copies of `remove_propagate_from`. Not changed, as asked.


## Promotions (`914d08e`, `8aadadf`) [run]

What promotions cost after D32 and D33, which values they promote, and what would shrink them. `NI_CODESTATS=1` build on `3232c0c`, `tools/codesize.py <bench> -p -d`; compile-time shares from a release build. Each benchmark runs until it compiles nothing new.

### Method

- `ni._promostats()` (`pycompiler.c`, `NI_CODESTATS` only) records each site when `dispatcher.c:psyco_finish_promotion` emits it: code object, instruction, opcode, flavour (`pflags`), stub bytes, the site whose value was being compiled at the time (its parent), and a backtrace taken in `PycException_Promote`. `tools/promostats.py` resolves the backtrace with `addr2line` into "who asked" (e.g. `Psyco_NeedType < try_rich_compare`). For each value it records what the value is, the bytes closed and the time spent compiling after it (`do_promotion_internal`, `detected_megamorphic_pyobj_site`), and how often its block ran. [read]
- Run counts come from a 13-byte `mov r11, &counter; inc qword [r11]` at the start of each promoted block, in this build only. The flags are dead there, because `ipromotion_finish` spills them before its compare. The byte counts below leave the counter out, but the buffer-kind table in `codesize.py` includes it (about 1–2%). A miss is an entry into the C dispatcher (`do_promotion_*`): the inline cache held another value.
- "Code after a value" is everything compiled from a promotion entry, including the stubs of later sites. Code reached later through a coding pause counts as a pause.
- "Why an instruction has extra sites" walks up each site's parents to where two sites at one instruction meet (`promostats.py:forks`).

### How much [run]

| benchmark | code KB | promotion stubs | code after a value | promotion share of compile time (release) | sites | never entered | values | sites with one value |
|---|---|---|---|---|---|---|---|---|
| richards | 117 | 12.3 KB (10%) | 82.5 KB (70%) | 71% (0.50 of 0.71 ms) | 147 | 5 | 148 | 95% |
| deltablue | 440 | 66.3 KB (15%) | 312 KB (71%) | 69% | 790 | 138 | 688 | 79% |
| raytrace | 273 | 45.7 KB (17%) | 229 KB (84%) | 77% | 551 | 50 | 524 | 85% |
| go | 444 | 73.7 KB (17%) | 329 KB (74%) | 64% (2.12 of 3.31 ms) | 880 | 83 | 805 | 89% |
| hexiom | 417 | 76.0 KB (18%) | 302 KB (72%) | 64% | 896 | 104 | 792 | 88% |
| chaos | 66 | 14.8 KB (22%) | 61.9 KB (94%) | 81% | 268 | 6 | 171 | 64% |
| pyflate | 218 | 25.0 KB (11%) | 133 KB (61%) | 57% | 296 | 33 | 264 | 89% |
| scimark | 200 | 36.0 KB (18%) | 157 KB (79%) | 66% | 429 | 34 | 399 | 91% |
| mdp | 179 | 42.0 KB (24%) | 143 KB (80%) | 72% | 497 | 48 | 452 | 89% |
| deepcopy | 70 | 15.3 KB (22%) | 60.0 KB (85%) | 76% | 183 | 17 | 237 | 86% |
| nbody | 40 | 12.6 KB (31%) | 38.7 KB (96%) | 88% | 150 | 0 | 150 | 100% |

- **Most code is compiled after a promotion, but most of that isn't waste.** A promotion ends the block, so the rest of the function is compiled from it. With one value per site (79–100% of sites), that code would exist anyway. The waste is the stubs, the extra values at a site, and the extra sites at one instruction (below).
- **A promotion stub costs 84–86 bytes per site (measured), 10–31% of all code.** 2–15% of sites never run (deltablue: 138 stubs, 11.7 KB). [run]
  - From the code, about 19 bytes are the inline cache (`mov r10, imm64; cmp reg, r10; je`), 32–40 are the `rt_promotion_t` written into the code buffer, and about 30 are the call into the code builder (`x64/iencoding.c:psyco_call_code_builder`). [inferred]
- **Compile time:** promotion entries are still 57–88% of compile time (release, `ni._stats()`), about 2.6 µs each. COMPILE_COST.md's 70% still holds.

### What is promoted, and who asks [run]

All 17 benchmarks together (stub plus code after, 2.5 MB):

| kind of value | share | identical copies (constants aside) |
|---|---|---|
| a type (`Psyco_NeedType`, `need_type_mega`) | 93.5% | 186 KB |
| a function (calls) | 1.9% | 3 KB |
| megamorphic versions | 1.8% | 0 |
| a machine word (`psyco_atcompiletime`) | 1.4% | 1 KB |
| a code object | 0.7% | 0 |
| another object (e.g. a class with a metaclass) | 0.7% | 0 |

- **Who asks**, by share of promotion code:
  - attribute lookup (`need_type_mega < PsycoObject_GetAttr`, LOAD_ATTR/LOAD_METHOD): 22%;
  - `binary_op1`: 16%;
  - `try_rich_compare`: 14%;
  - `PsycoObject_IsTrue`: 7%;
  - UNPACK_SEQUENCE: 7%;
  - `binary_iop1`: 6%;
  - `PsycoObject_GetItem`: 5%;
  - then a long tail.
- The types promoted are mostly `int`, `float`, `bool`, `list` and user classes. A value loaded from an attribute, a list item or a call result has an unknown type, and each operation on it promotes the type. Promotion already takes the type, not the value, wherever only the type is used. [run], [read] (`Psyco_NeedType`)
- **The cut-off.** `MEGAMORPHIC_MAX` is 5 (`codegen.h`). It applies only to `need_type_mega` (attribute lookup) and `psyco_static_type_promotion` (calls to a type, `isinstance`); every other `Psyco_NeedType` site has none. [read]
  - In these benchmarks only 9 sites went megamorphic (scimark 1, mdp 3, deepcopy 5).
  - The import of `pkg_resources`/`pyparsing` (when capstone loads) has 36 sites with 5 or more values, and one `STORE_ATTR` with 23 types. [run]
- **How often values run.** The code for a value usually runs hundreds to millions of times per site. deltablue and go each have about 22 KB compiled for values whose block ran at most once in the measured run.
- **Misses are rare except at polymorphic attribute and arithmetic sites.** Each miss calls the C dispatcher, walks the list, and rewrites the inline cache's immediate in the code.
  - richards: one warm iteration enters promoted blocks 1.05 M times and misses 77,676 times (7.4%). 74k of the misses are at one `LOAD_METHOD` in `schedule`, which sees four `Task` subclasses in turn.
  - raytrace misses 3.9% (`Vector.__sub__` with `int` and `float`); the other benchmarks 0.1–0.3%.
  - `do_promotion_pyobj_mega` alone is 2.5% of richards' profile (`perf record`, release). The profile can't show the cost of rewriting code that is about to run. [run]

### Where the extra copies come from [run]

| benchmark | identical copies from promotions (jump targets aside / constants too) | ... same site, another value | code for a site's extra values | extra sites at an instruction that already has one |
|---|---|---|---|---|
| richards | 11.2 / 19.9 KB | 2.7 KB | 3.8 KB | 81 sites, 45 KB |
| deltablue | 44.2 / 56.8 KB | 2.8 KB | 11.5 KB | 586, 223 KB |
| raytrace | 12.9 / 14.4 KB | 1.7 KB | 8.4 KB | 391, 161 KB |
| go | 40.4 / 42.0 KB | 0.1 KB | 1.1 KB | 679, 252 KB |
| hexiom | 20.9 / 21.0 KB | 0 | 0 | 691, 231 KB |
| pyflate | 5.8 / 5.9 KB | 0 | 0.1 KB | 189, 83 KB |
| deepcopy | 8.6 / 11.7 KB | 6.4 KB | 11.8 KB | 121, 22 KB |

- **Sites that see several values make few copies.** The code compiled for a site's second and later values is 0–12 KB per benchmark. Identical copies from the same site are 0–3 KB (deepcopy 6 KB).
- **The copies come from one instruction having many sites.** go's `Board.useful` @126 `COMPARE_OP` has 32 sites, each promoting the type `int`; deltablue's `add_constraints_consuming_to` @28 has 63, each promoting `bool`. Each copy of the code before the instruction emits its own site, and each site compiles the rest of the function again.
- **Why the code before was copied.** Over 9 benchmarks, where two sites at one instruction meet:

| where the two sites meet | KB compiled after them |
|---|---|
| never: separate compiles entered by coding pauses or respawns | 825 |
| never: one compile entered by a call, the other by a pause or respawn | 128 |
| within one value's compile (a loop that reaches a merge point in a new state) | 202 |
| at an earlier site, with different values | 23 |

  So the duplication is a merge-point matter. Code entered from a pause or a respawn reaches a merge point in a state that doesn't match an existing block. It compiles a new block and promotes the same types again.
- **Why states with the same types don't match.** [read]
  - `dispatcher.c:compatible_array` rejects a match when the current state knows a fixed (promoted) value that the snapshot doesn't (`CHECK_FOR_NULL`). It has two reasons: "might just have been promoted", and, for `recdepth <= 2`, "we might as well try compiling with this extra information". Upstream's comment on the second: "XXX This assumption should be tested".
  - The other way round, a fixed value in the snapshot must be known in the current state.
  - So two paths that promoted a type in a different order or place never share code.
- **Experiment:** in a scratch build, I dropped the `recdepth <= 2` clause and kept the promotion-loop guard. All ten benchmarks hung, with no output after four minutes. Matching on less knowledge needs more care than deleting the test. [run]

### Options, ranked

Gains are against the code and compile time above. "Code" means all emitted code.

1. **Promotion stubs on D32-style stubs.** Done (`85aabc2`, D32); results in "Promotion stubs made small" below. Keep the 19-byte inline cache. On a miss, run `mov rsi, reg; mov r11, record; jmp psyco_stub_trampoline` (18 bytes), with the `rt_promotion_t` in the heap record.
   - **How the stub carries the value:** the run-time value is already in a register at the site (`ipromotion_finish` calls `RTVINFO_IN_REG(fix)`), so the stub copies it into RSI, the second SysV argument. RSI is never allocated, and the trampoline (`x64/iprocessor.c:write_stub_trampoline`) touches only RDI, RAX (saved), RSP and R11. `psyco_stub_enter(record, value)` passes it on as `fn(closure, value)`; the respawn and pause functions ignore it.
   - Promotion records are never freed (the site stays live), so the stub can sit inline at the site rather than in the pool.
   - **Code:** about 85 → 37 bytes per site, −48 B. The records (about 40 bytes each) move to the heap. [inferred from the measured 84–86 B]

     | benchmark | code saved |
     |---|---|
     | richards | −7 KB (−6%) |
     | deltablue | −37 KB (−8%) |
     | raytrace | −26 KB (−9%) |
     | go | −41 KB (−9%) |
     | hexiom | −42 KB (−10%) |
     | chaos | −13 KB (−19%) |
     | nbody | −7 KB (−18%) |

   - **Compile time:** slightly less, since less code is written; too small to measure.
   - **Run-time risk:** none expected. The miss path is the same C call, entered by `jmp` rather than an unmatched `call` (D32's rule).
   - **D21:** no concern; nothing compiled changes.
   - Small to medium.
2. **Matching across promoted knowledge at merge points.** This is the largest pool. Identical copies made by promotions are 5–10% of code (deltablue 44 KB, go 40 KB, richards 11 KB). Extra sites at an instruction that already has one hold 30–75% of the code after values, and most of that is the same types promoted again on a path entered from a pause or a respawn. Two forms:
   - Let a state that knows more (a fixed type the snapshot lacks) match, dropping the fact, as upstream's XXX suggests. The snapshot's code already has its own promotion site for that type. The experiment above shows the loop guard must be designed first: a state just after a promotion must not match the snapshot just before it.
   - When the snapshot knows a fixed type the state lacks, emit a type guard (the promotion's compare) and jump into the existing block, with a stub for when the guard fails.
   - **Code:** up to 5–10% from identical copies, and more if near-identical copies also merge.
   - **Compile time:** falls with the code, since promotions are most of it.
   - **Run-time risk:** the first form loses a known type on some paths (an extra inline-cache compare); the second adds a guard.
   - **D21:** safe if the decision depends only on the state and the snapshots, as D33's does: a respawn replay starts from a snapshot and never crosses a merge point.
   - Overlaps with the cap on blocks per merge point (option 3, in flight) and with type-set guards (option 4). Research; measure it under the cap first.
3. **A polymorphic inline cache, or one that stops rewriting.** Done (`ff1bce8`, D42); results in "Polymorphic inline cache" below. Before, each miss rewrote the cached value in the code. A short chain of compares (one per value, up to the cut-off), or keeping the first value, would turn richards' 74k misses per iteration into compares.
   - **Code:** about +19 B per extra value, at the 1–20% of sites with more than one.
   - **Compile time:** no change.
   - **Run time:** a gain, not a risk: at least the 2.5% of richards spent in `do_promotion_pyobj_mega`, plus whatever rewriting code costs (raytrace misses 3.9%). Measure with `perf`.
   - **D21:** no concern; it is run-time dispatch only.
   - Medium.
4. **A cut-off for every `Psyco_NeedType` site.** Today only attribute lookup and type calls have one.
   - **Code and compile time:** no change on these benchmarks, where no such site sees more than 4 types. It is a backstop for programs like `pyparsing`, with 23 types at one `STORE_ATTR`.
   - **Run-time risk:** the megamorphic version is generic, so slower where it applies.
   - **D21:** no concern. The count lives in the site at run time, and the result is a `RunTime_Megamorphic` flag in the state, which a replay reads from the snapshot, as with the existing cut-off.
   - Small.
5. **A lower cut-off, or sharing a site's copies.** The most it could save is the code for a site's extra values: 0–12 KB (0–3%; deepcopy 17%). Each value lost is a type no longer known downstream. Not recommended.
6. **Promoting fewer kinds of value.** Types are 93.5% of promotion code, and they are the specialisation Ni exists for. Functions, code objects, words and other objects together are about 5%. Not recommended.

## Two forms of one value (`e823ee8`, D41, QUESTIONS 38) [run]

The four pairs QUESTIONS 38 listed, measured with a tally of `codesize.py -s`'s states over all 23 pyperformance benchmarks (`NI_CODESTATS=1` release build at `ce81596`, cap on). Each local's state is classed as a constant (`C:int-large`: an int object outside -5..256), a virtual int with a known or a run-time value (`Vlong:known`, `Vlong:R`), a run-time object with a promoted type (`R:int`, `R:list`), a virtual float or empty list. "Only" counts the blocks whose state differs from an earlier block at the same merge point in that one local and nothing else: the blocks a single form would save directly. In all: 2,848 KB, 57,313 instructions compiled, 2,367 blocks.

| pair | extra blocks it co-varies with | blocks differing only in it | where |
|---|---|---|---|
| large int constant / virtual int | 87 | 14 | deltablue `change`, go `randrange`, `_randbelow` |
| large int constant / run-time int | 9 | 2 | go `_randbelow` |
| virtual int / run-time int object | about 250 | 46 | hexiom, chaos, comprehensions, go `score` |
| virtual float / run-time float | 33 | 6 | go `score` (an int or a float: a real difference) |
| virtual empty list / run-time list | 169 | 30 | hexiom `unknown`, pyflate, gc_traversal |

For comparison, a virtual int with a known value against one with a run-time value (a first iteration, then the general one) differs alone in 105 blocks, and an unbound local against a bound one in 95.

### Kept: a large int constant becomes a run-time object from the second block

`dispatcher.c:psyco_unpromote_large_ints`, called from `vcompiler.c:psyco_compile` and `psyco_compile_code` when a state matches no block at a merge point that already has one: each non-fixed constant int object in a local (small ones are virtual ints already, D33) is made run-time, and the state is compared again. It is the same object, so `x is CONST` still holds; writing it as a virtual int would lose that, since computing one makes a new object. The first block keeps the constant, so a function always called with the same constant is still specialised on it. Only loads of constants are emitted, so it is replay-safe for the cap's reasons (D40).

| benchmark | instructions compiled | blocks |
|---|---|---|
| pyflate | 4,183 → 4,092 (−2.2%) | 218 → 210 |
| deltablue | 6,224 → 6,213 | 284 → 283 |
| all 23 | 57,313 → 57,211 (−0.2%) | 2,367 → 2,358 |

The others are unchanged. deltablue's `change` @0 still has 4 blocks: with 1050 and 2000 now run-time, the extra block differs in `v` (a run-time object with and without a promoted type), a fixed fact. The pair's remaining co-variation (2 blocks) is first blocks that keep their constant.

Release build, `tools/compilebench.py`, two runs each, before (`ce81596`) and after, same session (timings indicative: other agents benchmark on the machine): all 29 benchmarks' code 2,887 → 2,884 KB, compile 20.7, 21.4 → 19.9, 21.5 ms; pyflate 201.4 → 199.8 KB, compile 1.81, 1.60 → 1.55, 1.61 ms, warm 141, 135 → 135, 138 ms; deltablue 449.9 → 449.3 KB; others the same code. `perf stat -e instructions:u`, ((3 + 12) − (3 + 2) iterations) / 10, two runs each: pyflate 2,435.5 → 2,428.0 M (−0.3%), deltablue 31.65 → 31.43 M, richards 194.1 → 194.8 M, go 1,143 → 1,169 M (go compiles the same code either way; its 12-iteration runs alone spread by 220 M, so this is noise).

### Not done: the pairs that need a virtual computed

Each of the other pairs has one direction that loses identity and one that needs a call:

- **Writing the run-time object as a virtual** (read its value, drop the object) makes a new object when it is next needed: `x is lst[0]` changes for an int outside -5..256, a float or a list. It also needs a run-time check that an int fits a word. Unsound.
- **Computing the virtual** (`PyLong_FromLong`, `PyFloat_FromDouble`, `PyList_New`) keeps identity, since nothing has seen a virtual yet, but the call's error check is a respawn point. After `psyco_compatible`'s verdict, which depends on the blocks compiled so far, a replay from the block's snapshot restarts the bytecode compiler at the merge point and never reaches it (D40). Before the snapshot, in the block that flows into the merge point, a replay does pass that point (`psyco_limit_nested_weight` already computes there, against `pycompiler.c`'s `extra_assert(!is_respawning(po))` just above it), but then the decision may only depend on the state, not on the blocks: computing every virtual int or float at every merge point would box loop counters and accumulators on each iteration. `psyco_compile_cond` also compares a copy of the state where no code can be emitted.

To see whether a replay-safe way (a snapshot that records "compute these locals first", replayed from the main loop's entry like `NEXT_INSTR_POP`) would be worth building, an experiment computed the virtual when a snapshot held a run-time object there (value already run-time; release build only, replays not handled; not committed): all 23 benchmarks 57,211 → 57,156 instructions compiled (−0.1%), 2,358 → 2,350 blocks: hexiom −75 instructions, raytrace −34, comprehensions −12, go +66. The list pair alone dropped (16 → 3 blocks differing only in it); the int pair barely moved (24 → 21), because the virtual usually comes first and the run-time object second, the direction that would need virtualising. Not worth a change to replay, and computing at merge points boxes values that stayed unboxed before. [run]

## Promotion stubs made small (`85aabc2`, D32) [run]

A promotion site is now the 19-byte inline cache, then `mov rsi, reg; mov r11, record; jmp psyco_stub_trampoline`: 37 bytes in all, written in place at the site (`x64/idispatcher.h:ipromotion_finish`, `dispatcher.c:psyco_new_stub_at`). Before, the miss path jumped over an inline `rt_promotion_t` and called `do_promotion_*` through `psyco_call_code_builder` (85–87 bytes). [read]

- **The value.** `ipromotion_finish` already put it in a register (`RTVINFO_IN_REG`, which loads it if it was on the stack), and the inline compare uses that register. RSI is never allocated, and the trampoline touches only RAX (saved), RDI, RSP and R11 before its call, so RSI arrives as `psyco_stub_enter(record, value)`'s second argument and goes on as `fn(closure, value)`. Respawn and pause functions take only the closure and ignore it. [read]
- **The cache rewrite** (`fix_fast_common_case`) patches the `mov r10, imm64` and the `je` target at `jump_if_equal_code`, as before; the stub after them is never patched. [read]
- **Lifetime.** A promotion record is entered again for every value the cache misses, so it is never freed, as the inline `rt_promotion_t` never was. Records are carved from 64 KB chunks of the C heap. A first version took them from `PyMem_Malloc`: scattered through pymalloc's pools, they changed hexiom's allocation pattern enough to cost 0.5% more instructions per iteration (40.70 → 40.87–40.95 M; 40.58 M from the C heap). [run]
- **Replays** (D21). A promotion ends the block, so a respawn replay never reaches one; `psyco_finish_promotion` asserts it under `ALL_CHECKS`, since a record made in a replay would leak. [read]
- **D32's rule** holds: the stub enters the trampoline with `jmp`.

`NI_CODESTATS` build, `tools/codesize.py <bench> -p`, before (`59b334b`) and after. Instructions compiled, sites and values are identical in every benchmark, so nothing compiled changed:

| benchmark | code emitted (KB) | promotion stubs (KB) | bytes per site |
|---|---|---|---|
| richards | 131 → 123 (−6%) | 12.3 → 5.3 | 86 → 37 |
| deltablue | 457 → 413 (−10%) | 66.2 → 28.5 | 86 → 37 |
| raytrace | 285 → 255 (−11%) | 45.6 → 19.6 | 85 → 36 |
| go | 459 → 413 (−10%) | 70.5 → 30.3 | 86 → 37 |
| hexiom | 359 → 317 (−12%) | 62.9 → 26.8 | 87 → 37 |
| chaos | 68 → 58 (−15%) | 14.7 → 6.4 | 56 → 24 (codesize's average) |
| nbody | 42 → 33 (−21%) | 12.6 → 5.4 | 86 → 37 |
| pyflate | 205 → 189 (−8%) | 23.5 → 10.0 | 87 → 37 |

Release build, `tools/compilebench.py`, all 29 benchmarks: code 2,887 → 2,615 KB (−9.4%); total compile 19.9–20.4 ms (three runs) → 19.6–20.3 ms (two runs). Eight alternating runs of the first four iterations: hexiom 2.78 → 2.80 ms, deltablue 2.61 → 2.59 ms (medians). Other agents shared the machine (load 5–7).

Instructions and branch misses per warm iteration (`perf stat -e instructions:u,branch-misses:u`, `PYTHONHASHSEED=0`, (12 − 2 iterations) / 10, two runs each):

| benchmark | instructions (M) | branch misses (M) |
|---|---|---|
| richards | 199.96, 199.96 → 200.81, 200.81 (+0.4%) | 0.092, 0.099 → 0.097, 0.090 |
| raytrace | 3,252, 3,253 → 3,283, 3,270 | 1.63, 1.64 → 1.65, 1.69 |
| go | 1,141.0, 1,141.1 → 1,141.1, 1,141.1 | 0.77, 0.80 → 0.80, 0.74 |
| deltablue | 32.06, 32.07 → 32.08, 32.06 | 0.003, 0.006 → 0.004, 0.002 |
| hexiom | 40.70, 40.70 → 40.58, 40.58 | 0.030, 0.032 → 0.031, 0.031 |

- **A miss costs about 11 more instructions.** richards misses 77.7k times per iteration and gains 0.85 M instructions. The trampoline aligns the stack at run time and goes through `psyco_stub_enter`, where the old call knew the alignment at compile time. Cycles per richards iteration didn't move (52.7–53.4 M before, 52.0–52.5 M after).
- **raytrace** misses more often (1.2 M misses over the codesize run). Its instruction count varies by ±20 M between runs of one build; five more runs gave medians of 3,252 before and 3,270 M after (+0.5%), and cycles were within noise.
- Branch misses are within run-to-run noise everywhere: the trampoline's one `jmp r11` now serves every promotion miss, where each site had its own `jmp r10`, and richards' misses didn't rise.
- A polymorphic inline cache (option 3) would remove most misses, and this cost with them. Done: see the next section.

## Polymorphic inline cache (`ff1bce8`, D42) [run]

A promotion site's inline compare now keeps the first value it saw. The next four values go in a chain of the same `mov r10, imm64; cmp reg, r10; je` entries (19 bytes each), out of line in the D32 stub pool. Hits on any of them never enter the trampoline or C. [read] (`x64/idispatcher.h`, `dispatcher.c:promotion_cache_add`)

- **Entering the chain.** On the second value, `jmp chain` is written over the start of the site's own miss path (`mov rsi, reg; ...`). So the chain ends with its own copy of the miss stub (`mov rsi, reg; mov r11, record; jmp psyco_stub_trampoline`, 18 bytes).
- **Size.** A chain starts with room for one entry (37 bytes). Most chains hold one. When it needs more, it is copied to a chain with room for four (94 bytes), and the old one is left unused. Entries are read back for the copy (`ipromotion_read_entry`), and under `ALL_CHECKS` the copy is checked against the original.
- **The cut-off.** Once a site can compile no more values, the chain ends in `jmp` to the megamorphic version instead of the miss stub. That is `do_promotion_pyobj_mega` after `MEGAMORPHIC_MAX` values, or `do_promotion_static_type` once its count reaches it. Before, every new value at a megamorphic site entered C.
- **Sites with no cut-off** (`Psyco_NeedType`, words). When the chain is full, a new value replaces the last entry. The move-to-front list in C and its 1-in-23 slow path are unchanged.
- **Patching safety.** Code is only patched from the C dispatcher. The trampoline reached it from this site's miss path, or from its chain's. It returns through `jmp rax` to a target, never back into the bytes it changed. Ni is single-threaded under the GIL, and x86 keeps the instruction stream coherent with stores made by the same thread, so no instruction is ever seen half-written. [read], [inferred] for the coherence.
- **D32's rule holds:** the chain uses only `je` and `jmp`.
- **D21: nothing compiled changes.** Promotions end the block, and `psyco_finish_promotion` asserts that no replay reaches one. The cache is written only by `do_promotion_*` at run time, after the value's code is compiled, and no compile decision reads it. Instructions compiled are identical in every benchmark below.
- **Why the first value stays inline**, not the most frequent:
  - Counting frequencies would need a counter in emitted code.
  - At the polymorphic sites the values are close in frequency. richards `schedule` @34: first value `DeviceTask` 104k runs, the most frequent `HandlerTask` 141k. raytrace `__sub__`: `int` 357k, `float` 477k.
  - A value further down the chain costs one more compare and a not-taken `je` (about 3 instructions).
  - Rewriting the inline value was what made misses expensive. Each rewrite patched code that was about to run, and went through an indirect `jmp`.

Misses (entries into C), `NI_CODESTATS` build, `tools/codesize.py <bench> -p`, over the whole run, before (`b60e8bb`) and after:

| benchmark | promoted blocks run | misses before | misses after | chains (bytes, from counts) |
|---|---|---|---|---|
| richards | 3.33 M | 233,056 (7.0%) | 158 | 36 (about 1.5 KB) |
| raytrace | 30.6 M | 1,206,108 (3.9%) | 524 | 69 (about 2.6 KB) |
| deltablue | 0.50 M | 1,857 (0.37%) | 688 | 60 (about 3.0 KB) |
| go | 11.7 M | 22,567 (0.19%) | 765 | 44 (about 2.0 KB) |
| hexiom | 0.55 M | 654 | 654 | 38 (about 1.5 KB) |
| chaos | 4.67 M | 171 | 171 | 42 (about 1.9 KB) |
| pyflate | 26.0 M | 251 | 249 | — |

- **Misses now equal the values compiled.** Each value enters C once, to compile its code. Every miss in these benchmarks was at a site with at most four values, so nothing was left for the cut-off or a full chain to catch.
- **Chains.** "Chains" counts those made over the whole run, including imports and setup, so it is more than the sites with two or more values in `-p`'s measured window. Bytes are 37 per chain plus 94 per grown chain [inferred from counts in a scratch build].
- **Code emitted** (`codesize.py`) moved by at most one 4 KB pool chunk: richards 117.2 → 117.2 KB, raytrace 244.9 → 244.9, deltablue 404.1 → 408.1, go 407.2 → 407.2, hexiom 309.5 → 309.5, chaos 56.6 → 52.6, pyflate 184.6 → 184.6.
- **A first version** gave every chain room for four from the start: 94 bytes for a chain that usually holds one, or 3.4–6.5 KB per benchmark.

Release build, per warm iteration, `perf stat -e instructions:u,branch-misses:u,cycles:u`, `PYTHONHASHSEED=0`, (12 − 2 iterations) / 10. Four runs of each, alternating before and after; load average 4–5 from other agents:

| benchmark | instructions (M) | branch misses (M) | cycles (M) |
|---|---|---|---|
| richards | 235.80 → 232.43 (−1.4%) | 0.093–0.096 → 0.061–0.070 (−33%) | 60.3–62.3 → 59.1–60.3 (−3%) |
| raytrace | 3,242–3,273 → 3,229–3,250 (−0.6%) | 1.51–1.70 → 1.21–1.25 (−25%) | 1,083–1,123 → 933–949 (−14%) |
| deltablue | 32.05–32.07 → 32.05–32.06 | 0.004–0.007, same | 8.0–8.5 → 7.9–8.4 |
| go | 1,139.0 → 1,138.7 | 0.76–0.81 → 0.76–0.79 | 337–352 → 336–343 |
| hexiom | 40.58 → 40.60–40.62 (+0.07%) | 0.029–0.033 → 0.027–0.038 | 11.7–15.2 → 11.1–11.6 |
| chaos | 486.85 → 486.82–487.06 | 0.130–0.147 → 0.136–0.156 | 145–152 → 135–139 |

- **richards and raytrace gain mostly in cycles and branch misses, not instructions.** The miss path was short (about 40 instructions). What it cost was rewriting code just before running it, and the shared trampoline's indirect jumps.
- An earlier build (fixed four-entry chains, otherwise the same) gave richards 55.5–62.0 M cycles and 0.033–0.048 M branch misses. The chain's placement in the pool seems to matter at richards' four-way `schedule` site. Unexplained.
- **hexiom** has no polymorphic misses in the warm loop. Its +0.07% instructions is the extra compare at sites whose second value is now always in the chain.
- **chaos's** fewer cycles (also seen in `compilebench` below) are not explained by misses (there are none). Probably code placement, since the stub pool now takes chunks differently.

`tools/compilebench.py -k <bench>`, release build, two runs each:

| benchmark | Ni warm (ms) | compile (ms) | code (KB) |
|---|---|---|---|
| richards | 11.58, 11.70 → 10.88, 10.66 | 0.69, 0.69 → 0.67, 0.67 | 118.0 → 118.0 |
| raytrace | 201.8, 201.6 → 182.9, 181.5 | 1.78, 1.64 → 1.71, 1.65 | 250.3 → 246.3 |
| deltablue | 1.52, 1.52 → 1.47, 1.48 | 2.53, 2.56 → 2.68, 2.50 | 406.4 → 406.4 |
| go | 64.3, 66.1 → 63.0, 64.0 | 3.16, 3.05 → 3.16, 3.12 | 405.6 → 409.6 |
| hexiom | 2.24, 2.27 → 2.12, 2.08 | 2.84, 2.66 → 2.81, 2.81 | 311.9 → 311.9 |
| chaos | 29.35, 28.99 → 26.79, 26.64 | 0.45, 0.48 → 0.48, 0.47 | 57.3 → 57.3 |

Compile time is unchanged.

**A latent bug found on the way** (KNOWN_BUGS C26). The stub pool took 4 KB from `psyco_new_code_buffer`, but a reused code buffer may have only about 1 KB left (`BUFFER_MARGIN`). `psyco_assert(pool_end <= limit)` fired in CPython's `test_descr` once chains used the pool more. The pool now takes what the buffer has.


## Forgotten values (`61db667`, D43, SPECIALISATION.md step 1) [run]

Past the cap (D40), `dispatcher.c:psyco_forget_values` now also un-promotes each non-fixed constant local that some block at the merge point doesn't hold (`varies_at` reads the snapshots' top-level sources), and **forgets** it: its run-time source gets `RunTime_Megamorphic`, the flag a megamorphic promotion site already puts on its value. Then:

- **Promotions honour it.** `psyco_pyobj_atcompiletime_mega` (attribute lookup's type, calls to a function) and `psyco_atcompiletime_static_type` (`type_call`, `isinstance`) already returned "megamorphic" for a flagged value. `pfunction_call` and `PsycoCFunction_Call` now test the callable itself, before promoting a value read from it (`func_code`, `m_ml`), and take the generic call. `Psyco_NeedType` and `psyco_atcompiletime` have no generic path and still promote; the type of a forgotten value is not forgotten (below). [read]
- **Merge points compare it.** It is in the source word, so snapshots keep it and a replay starts with it (D21). A forgotten value never matches an unmarked run-time one in a snapshot (that code may promote it again); an unmarked one may match a forgotten one; and, new, a non-fixed constant partially matches a forgotten one (or a megamorphic one), so it is un-promoted and joins it (`compatible_vinfo`). [read]

### Method

`NI_CODESTATS=1` release builds of `b60e8bb` (before) and the change, `tools/codesize.py <bench> -s` on all 23 pyperformance benchmarks, `PYTHONHASHSEED=0`; release builds for `tools/compilebench.py` (two runs each) and `perf stat -e instructions:u` (((3 + 12) − (3 + 2) iterations) / 10, two runs each). Variants were built with a temporary switch (not committed) and measured the same way. A synthetic case, `tests/cases/mp_forget.py`'s four functions through `orders` three times, counts code buffers and bytes from `ni._stats()`.

### What was tried

Instructions compiled (all 23 benchmarks; meteor_contest), and the synthetic case's code bytes (funcs / classes / default_const / mixed):

| version | all 23 | meteor | synthetic bytes |
|---|---|---|---|
| before (ints only) | 57,298 | 1,448 | 23,273 / 36,556 / 14,904 / 29,071 |
| every constant, no mark (D40's rejected version) | 57,363 | 1,524 (+5.2%) | 29,660 / 26,229 / 18,392 / 23,904 |
| every constant, marked, constants join | 57,378 | 1,523 | 21,834 / 30,681 / 24,509 / 28,478 (hash seed not fixed) |
| every constant, marked, type kept as a fixed fact | 57,383 | 1,522 | — |
| **only constants that differ, marked, constants join** (kept) | 57,301 | 1,448 | 21,834 / 30,681 / 15,024 / 24,382 |
| as kept, and a forgotten value may also enter an unmarked block | 57,285 | 1,448 | 21,834 / 30,681 / 12,328 / 24,382 |

- **The mark alone did not fix meteor_contest.** Its +5% was not re-promotion now: `bisect` (a default argument) is the same constant in all 34 blocks at `solve` @66, so forgetting it gains nothing there, and the forgotten form spreads to every merge point after it (@82 11 → 12 blocks, @174 7 → 9), whose existing blocks hold the constant. Forgetting only a constant that some block doesn't share brings meteor back to 1,448.
- **Keeping the type** (a fixed type in the forgotten value's array) changed nothing on pyperformance, and a forgotten value with a fixed type can't be joined by a run-time value without one [inferred]; dropped.
- **Letting a forgotten value enter an unmarked block** was best on the synthetic case (default_const 59 → 42 buffers), but it also lets a megamorphic site's value back into code that promotes it: the megamorphic version would re-enter the old site and call the C dispatcher on each pass (deepcopy, which has 5 megamorphic sites, compiled 16 instructions fewer, probably that [inferred]). Not kept; it is the open question in SPECIALISATION.md, and safe only one way.

### Results (the kept version)

`codesize.py -s`, before → after: every benchmark compiles the same instructions and blocks except deltablue, 6,213 → 6,216 instructions, 283 → 285 blocks (`add_propagate` @18 forgets a `Planner`, `chain_test` @96 a `None`). Code KB differ by a pool chunk at most (mdp 161 → 157, deepcopy 64 → 65); all 23: 2,632 → 2,629 KB, 2,364 → 2,366 blocks.

**The cap rarely meets a constant that varies.** Over all 23 runs (startup included) the cap forgot 2–20 constants per benchmark, nearly all in the stdlib at import (`sre_parse._parse`'s `_NamedIntConstant`s and `None`, `gettext._expand_lang`'s strings). In benchmark code: deltablue (above), richards and richards_super (`runTask` @56, a `None`), deepcopy (a `list` in `deepcopy` @12). What is left past the cap differs in fixed facts and forms of a value, as D40 found.

`perf stat -e instructions:u` per warm iteration (M), two runs each:

| benchmark | before | after |
|---|---|---|
| richards | 235.80, 235.80 | 235.34, 235.91 |
| richards_super | 437.25, 437.63 | 437.56, 437.65 |
| deltablue | 32.08, 32.08 | 32.10, 32.10 (+0.05%) |
| deepcopy | 2.158, 2.177 | 2.171, 2.177 |
| mdp | 30,578, 30,622 | 30,510, 30,498 (−0.3%) |
| meteor_contest | 646.76, 646.76 | 646.70, 646.70 |
| chaos | 487.11, 487.11 | 487.63, 487.01 |
| go | 1,139.1, 1,139.1 | 1,139.1, 1,139.2 |
| hexiom | 40.59, 40.59 | 40.59, 40.59 |
| raytrace | 3,255, 3,251 | 3,248, 3,241 (−0.3%) |
| pyflate | 2,438.7, 2,438.7 | 2,453.7, 2,453.7 (+0.6%) |

- **pyflate +0.6% is real and repeatable, but not pyflate's own code.** Nothing in pyflate is forgotten; with forgetting switched off at run time in the same build it is 2,438.7 M again. The forgotten constants are in `sre_parse` at import, so the likely cause is the code before pyflate's taking different room in the buffers, which moves where coding pauses split pyflate's blocks [inferred]. deltablue's +0.05% is its two forgotten locals or the same.
- No benchmark shows a loss from a generic call through a forgotten value: none is called in a warm loop.

`tools/compilebench.py`, release, two runs each: all 29 benchmarks' code 2,590 → 2,590 KB, compile 20.2, 21.4 → 20.3, 21.7 ms; deltablue 406.4 → 405.7 KB, mdp 156.9 → 152.9 KB, deepcopy 35.9 → 39.9 KB (one pool chunk, same instructions compiled); warm times within noise (the second "after" run shared the machine: scimark_sor 61 → 89 ms in one run only).

**Synthetic case** (`tests/cases/mp_forget.py`, code buffers / bytes, before → after): funcs (a local holding one of five functions and builtins, called in the loop) 77 / 23,273 → 74 / 21,834 (−6%); classes (one of four classes, instantiated) 81 / 36,556 → 70 / 30,681 (−16%); default_const 59 / 14,904 → 59 / 15,024 (+0.8%); mixed (a function, a class, a large int or `None`) 88 / 29,071 → 86 / 24,382 (−16%).

**Verdict:** kept as a backstop, like the cap: it costs nothing measurable on pyperformance and stops the copying in loops whose locals hold changing functions or classes. It is not a code-size win on these benchmarks.
## Respawned branches rejoin (`f229d14`, D44, QUESTIONS 42) [run]

Every run-time condition whose rare side isn't compiled is a respawn point. When the rare side is first taken, the branch is compiled (`dispatcher.c:do_respawn`) and runs on to the next merge point on its own: a copy of everything after the site. With many checks in a row (D31's loads, D39's type guards, D38's inline stores) the copies piled up: richards' `Task.__init__` went from 10 to 54 KB with inline stores. [run]

### The design

- **The main path already had a block there.** Every guarded site sets `split_after_global` (`load_global_cached`, `pobject.c:generic_getattr`, `heap_setattr`, the store checks), and the main loop ends the block after the instruction with a new block and its snapshot. That block had no list, so nothing could find it. [read] (`pycompiler.c`, the `split_after_global` branch)
- **Join points.** The blocks started after a split are now kept per instruction (`mergepoints.c:psyco_join_point`, a fatlist per code unit, made on first use, freed with the code object like the merge points'). `vcompiler.c:psyco_compile_join` compares the state with them. [read]
- **A respawned branch** (its block is a respawned one: `fz_respawned_from` set) is compared as at a merge point: it jumps into a compatible block (`psyco_unify`), with differing non-fixed constants made run-time. [read]
- **It may drop known fields of run-time values that the block doesn't have** (`dispatcher.c:psyco_compatible_join`). `compatible_array` refused a state with extra known items in the first two levels ("you get better results this way", Psyco's own guess) and dropped them below. A branch rather joins: most failures to join were the branch knowing *more*, such as the instance dict pointer the inline path's checks loaded on the way to the miss. Such a field is a cached load of an immutable field (`FMUT` fields are never cached), so the joined code reloads it. At first only fields that were not virtual and held no reference were dropped: unify decref'd what it drops after moving what it keeps, maybe over the dropped value's slot (KNOWN_BUGS C27, `test_enum` crashed); since D49 any field of a run-time value is. [read], [run]
- **Both paths rewrite small ints first** (`canonical_int`, D33) since D49. At first they didn't: at a split a local is often shared with a known field of a constant, which must stay constant, and `canonical_int` rewrote it in place (KNOWN_BUGS C28; `test_math`, `test_grammar`, `test_aifc` asserted; it was live at merge points too). [run]
- **The main path** (any other path) only jumps into an exact match, with no dropping and no un-promotion, so it keeps every fact it compiled with. **No block boundary is added**: the split was already there, so option (a)'s cost (a block boundary that loses facts) does not arise, and option (b) (recording the main path's state without a merge point) is what the split already did. [read]
- **In an inlined frame**: at first a branch there took exact matches only. The caller's code object and position were non-fixed constants in the state (`psyco_save_inline_po`), and making them run-time crashed the return (`rejoin.inlined_two_sites`). Since D49 they are fixed constants, which a match never un-promotes, and the branch matches as anywhere else. Merge points are never inside an inlined frame; join points can be. [run]
- **Replays (D21).** A replay never reaches the end of its block (the split is skipped while respawning, and the block ends at the split); a joined block's respawn points replay from that block's own snapshot; unification and un-promotion emit no respawn point. The checked build ran the suites and seven benchmarks without a replay divergence. [read], [run]
- **Counters:** `ni._stats()['joins']` = (branch exact, branch partial, branch none, main path exact). [read]

### Measurements

`NI_CODESTATS=1` builds, `tools/codesize.py <bench>`: code emitted (KB) and instructions compiled. "base" is `b60e8bb`, "store" is `b60e8bb` with the inline stores (`80a60ce`, cherry-picked), "+J" adds this change. [run]

| | base | base+J | store | store+J |
|---|---|---|---|---|
| richards | 119 / 2,012 | 117 / 1,977 | 222 / 2,714 | **161** / 1,977 |
| deltablue | 412 / 6,213 | 372 / 5,365 | 463 / 6,458 | 404 / 5,374 |
| raytrace | 251 / 3,954 | 191 / 2,956 | 284 / 4,260 | 205 / 2,962 |
| go | 416 / 6,822 | 276 / 4,634 | 466 / 7,246 | 302 / 4,634 |
| chaos | 58 / 995 | 56 / 960 | 60 / 995 | 58 / 960 |
| hexiom | 317 / 6,102 | 254 / 5,051 | 325 / 6,208 | 262 / 5,051 |
| pyflate | 187 / 4,092 | 168 / 3,735 | 210 / 4,166 | 183 / 3,735 |

- **Richards with inline stores: +87% code over base before, +35% now** (`Task.__init__` 51.5 → 10.8 KB, its copies 39 → 12, as before the stores). The owner's budget was +20–25%. What is left is the inline store's own size per compiled site (STORE_ATTR 108 → 208 bytes per compile), more stub chunks (6 → 9) and more promotion-compiled code (65 → 77 KB). Instructions compiled are back to base's. [run]
- **Without inline stores it helps more than expected**: go −34% code and −32% instructions compiled, raytrace −24%, hexiom −20%, deltablue −10%. The copies were D31's loads and D39's guards all along. [run]
- **How often a branch joins** (branch exact / partial / none, then main path exact): richards 6/0/3, 1 (store+J 81/0/3, 1); deltablue 93/1/0, 38; go 65/2/18, 57; raytrace 24/5/11, 31; hexiom 29/2/12, 40. The rest of the respawns end before a split (an error path, a merge point). A branch that doesn't join mostly has a run-time value where the block has a constant or a different virtual (a generic attribute lookup against a virtual bound method); that is right. Before dropping was allowed, store+J richards was 195 KB. [run]

`tools/compilebench.py`, release builds, all 29 benchmarks (one run of base and store, two of each +J: before and after the C27 and C28 restrictions, which changed nothing compiled in these benchmarks; machine shared with other agents): [run]

| | base | base+J | store | store+J |
|---|---|---|---|---|
| code, all 29 | 2,590 KB | 2,246 KB (−13%) | 3,021 KB | 2,454–2,458 KB (−19%) |
| compile, all 29 | 23.2 ms | 19.0–19.8 ms | 25.5 ms | 20.0–20.5 ms |
| richards code / compile | 118 KB / 0.69 ms | 116 / 0.73 | 221 / 1.03 | 160 / 0.95 |
| go code / compile | 406 / 3.12 | 271 / 2.47 | 463 / 4.68 | 298 / 2.60 |

Store+J against base: +9.5% code and −12 to −14% compile time over all 29, richards +36% code and +38% compile time.

Instructions per warm iteration, millions (`perf stat -e instructions:u`, (13 − 3 iterations) / 10; go and raytrace four to six runs, the others one or two): [run]

| | richards | deltablue | raytrace | go | chaos | hexiom | pyflate |
|---|---|---|---|---|---|---|---|
| base | 231 | 31 | 3,262–3,295 | 1,139–1,164 | 489 | 41 | 2,436 |
| base+J | 230–231 | 31 | 3,252–3,272 | 1,132–1,177 | 486–493 | 40 | 2,445–2,455 |
| store | 179 | 28 | 3,108–3,174 | 1,115–1,151 | 473 | 40 | 2,345 |
| store+J | 178–180 | 28 | 3,109–3,136 | 1,103–1,166 | 471–477 | 40 | 2,338–2,344 |

Speed is unchanged within noise: a branch that joins runs the main path's code, which is what it would have compiled anyway. go's ranges overlap (base 1,139–1,164, base+J 1,132–1,177); worth a quieter rerun. Inline stores keep their gain (richards −23% against base).

### Lifting the restrictions (D49) [run]

With C27 (`a4b946e`) and C28 (`261fd83`) fixed, the join points' own restrictions were lifted (`6f2c227`): any known field of a run-time value may be dropped, `canonical_int` runs at join points, and a branch in an inlined frame may un-promote (the caller's code object and position are now fixed constants). Each was also measured alone with a temporary switch; none changed anything.

"Before" is `a4b946e` (the fixes, restrictions in place), "after" is `6f2c227`'s code; same session, `PYTHONHASHSEED=0`, machine shared with other agents.

| | richards | go | raytrace | deltablue | hexiom | chaos | all 29 |
|---|---|---|---|---|---|---|---|
| code KB, `codesize.py -s` (NI_CODESTATS), before = after | 118 | 287 | 199 | 392 | 269 | 57 | |
| instructions compiled, before = after | 1,969 | 4,634 | 2,956 | 5,372 | 5,051 | 960 | |
| code KB, `compilebench.py` (release), before = after | 121.7 | 279.0 | 199.6 | 391.0 | 264.2 | 56.5 | 2,363 |
| compile ms, before (two runs) | 0.76, 0.72 | 2.46, 2.60 | 1.48, 2.04 | 2.79, 2.74 | 3.92, 2.57 | 0.50, 0.49 | 22.6, 21.3 |
| compile ms, after (two runs) | 0.81, 1.05 | 2.61, 2.43 | 1.49, 2.00 | 2.69, 2.65 | 2.60, 2.76 | 0.52, 0.48 | 21.4, 21.6 |
| instructions per warm iteration, M, before | 234.4 | 1,144.2 | 3,314.6 | 32.4 | 41.2 | 496.7 | |
| instructions per warm iteration, M, after | 234.4 | 1,144.2 | 3,286.1 | 32.4 | 41.2 | 492.5 | |

- **Nothing compiled changes.** `codesize.py -s` differs only in addresses and buffer-switch counts, the joins are the same, and over all 29 benchmarks only mdp's code moves (151.7 → 151.5 KB). The restrictions never met a case in pyperformance: no unify at a join point or merge point freed anything there (a temporary counter, checked build, the seven benchmarks), and `tests/cases`' rejoin, attr_stores, inlining and int_counters join the same number of times.
- **Instructions per warm iteration** are `perf stat -e instructions:u`, ((3 + 10) − (3 + 0) iterations) / 10, one run each; raytrace and chaos differ by about 1%, which is noise for identical code.
- **What it buys is simpler code** (`droppable()` and the `canonical_int` and inlined-frame exceptions are gone) and two live bugs found: C28 asserted at merge points too.

## Inline store sites shrunk (`4398496`, `035ef12`, `2606291`, D47; STORES.md "Shrinking the store sites") [run]

On top of store+J. Each failed check of an inline store used to be a respawn point: a stub per check and, when taken, a respawned buffer. Now every failed check calls one helper out of line, and the old value's dealloc goes through one shared glue. The type guard is a 47-byte emitter, used by loads too. Release builds, `tools/compilebench.py`, one session (base is `e4b3a5f`, base+J):

| | base | store+J | shrunk |
|---|---|---|---|
| code, all 29 | 2,363 KB | 2,548 KB | 2,344 KB (−0.8% against base) |
| compile, all 29 | 19.8 ms | 21.7 ms | 19.2 ms |
| richards code / compile | 121.7 KB / 0.74 ms | 162.0 / 0.99 | **127.6** / 0.72 |
| go code / compile | 279.0 / 2.55 | 305.6 / 2.85 | 270.0 / 2.32 |
| richards instructions per iteration | 227 M | 177 M | 161 M |

- **Richards is +10% over the baseline's 116 KB** (the budget was +20–25%). STORE_ATTR is 208 → 166 bytes per compiled site. Respawned buffers are 90 → 15 and stub chunks 10 → 6, the same as base (`tools/codesize.py richards -s`).
- **The removed respawns did more than the smaller sites.** Step 1 alone took richards from 162 to 136 KB; the sites themselves account for only 3 KB of that (172 compiles, 208 → 191 bytes each).
