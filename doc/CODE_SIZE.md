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
  - `coding_pause`, `unify`, `glue`: pause stubs, state-matching code at merge points, start-up code.
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

## Next

1. **Confirm what differs between the identical copies:** compare the compiler states at the merge points that start them (for go, the loop head in `useful`). That shows whether constant counters cause it.
2. **Make respawn stubs smaller or lazy.** That could cut code by up to about 45% with no change in what gets compiled.
