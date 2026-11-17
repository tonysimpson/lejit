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

### What differs between the copies (go, `tools/codesize.py go -s`)

`vcompiler.c:cs_state` logs the state each new block at a merge point starts from. In go, one merge point in `Board.useful` (@310) started 65 blocks. The slots that differ are the five counters and `strong_neighs`/`strong_opps`, each in one of three forms: [run]

- a constant `int` object (`0`);
- a virtual int whose value is a known constant (`1`, `2`);
- a virtual int whose value is run-time.

The virtual ints also differ in whether their type field is fixed (promoted) or not. `psyco_compatible` turns differing plain constants into run-time values (a partial match), but a constant object against a virtual int is a structural difference: no match, so a new block. Nothing caps the blocks per merge point. [read] (`dispatcher.c:psyco_compatible`, `vcompiler.c:psyco_compile`)

## Options

1. **Small respawn stubs.** Each stub is its own code buffer: a call into `do_respawn` with its arguments, then a `respawn_t` (about 30 bytes of code and 32 of data, aligned). One `call` to a shared trampoline that finds its `respawn_t` by return address in a table would be 5 bytes of code. It changes nothing about what is compiled, so it is the safest large win: up to about 45% of all code, and less compile time spent creating buffers. [inferred]
2. **One form for ints at merge points.** Before `psyco_compatible`, write a constant `int` object as a virtual int with a constant value, and treat a virtual int's type field the same whether it is fixed or not (it is always `int`). Then go's counters differ only in constants, which the existing partial match already generalises. Narrow and safe; the same applies to floats and bools. [inferred]
3. **A cap per merge point.** After N blocks at one merge point, generalise the new state before matching: plain and fixed constants become run-time, and virtual values are computed. Forgetting facts is always safe; code after it may promote again. This is the general backstop for explosions the first two don't foresee. N is a policy choice, measured under D29. [inferred]
4. **Copies per type with identical code** (richards' `Task` methods). These are real specialisation: the guards hold a different type and different shared keys. Sharing them needs a guard that accepts a set of types. That is research, and it is worth doing only after 1–3. [inferred]

The general principle behind 2 and 3: states should be compared by what code after them can exploit, not by how a value happens to be represented. Psyco's "fixed" flag (a promoted value something already depends on) is a step towards that; 2 removes differences of representation, and 3 bounds the rest.
