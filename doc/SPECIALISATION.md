---
written: 2026-11-17
head_ref: 0dca1a1
status: design note; guides the queued specialisation work (BACKLOG)
---
# One policy for specialisation

Ni specialises code in two places: promotion sites and merge points (GLOSSARY, "Shared plain words"). Each grew its own limits and rules. This note says what the two have in common, where their separate decisions work against each other, and one policy for both. The mechanisms stay separate.

## The two places

| | promotion site | merge point |
|---|---|---|
| what it keys on | one value: a type, a function, an int | the whole state |
| when it chooses | at run time: a compare chain on the actual value | at compile time: `psyco_compatible` picks a block, and the code jumps straight there |
| copies | one block per value seen | one block per state seen |
| limit | megamorphic, 5 values (`MEGAMORPHIC_MAX`); only some sites have it (CODE_SIZE "Promotions") | the cap, 6 blocks (D40) |
| generic copy | the megamorphic path: the value stays run-time | the block compiled from a generalised state |
| sharing copies | none: each value's block runs to the next merge point | partial matches generalise differing constants (D33, D41) |
| code | `dispatcher.c:psyco_finish_promotion`, `do_promotion`, `x64/idispatcher.h` | `dispatcher.c:psyco_compatible`, `vcompiler.c:psyco_compile` |

[read]

The difference in **when** is fundamental. A merge point knows the incoming state while compiling, so it needs no run-time test. A promotion exists because the value is only known at run time. So the mechanisms stay apart. The **policy**, meaning how many copies to keep, which facts are worth keeping and when to stop specialising, is the same question in both, and today it is answered twice, independently.

## Where the two work against each other

1. **A promotion adds a fact that splits the next merge point.** Code after a promotion knows "this is a `Task`". At the next merge point it can't match a block compiled without that fact, nor the reverse (`compatible_array`), so it compiles a new block. Code entered from coding pauses and respawns re-promotes the same types this way: 953 KB across 9 benchmarks, against 23 KB from a site seeing several values (CODE_SIZE "Promotions"). [run] deltablue's `change` keeps a fourth block for the same reason (D41). [run]
2. **The cap generalises a value, and the next promotion specialises it again.** The cap made a constant function run-time; the next call promoted it, one block per function: meteor compiled 5% more (D40). That is why the cap generalises only ints today. [run]
3. **A polymorphic site splits everything after it.** Each value gets its own copy of the rest of the block, so a site with 3 types costs 3 copies as well as the misses. JavaScript inline caches cache how to do the operation and share the code after it. [read] (V8, SpiderMonkey CacheIR, JavaScriptCore), [inferred] for Ni
4. **Nothing joins a respawned branch back.** Each respawned branch runs to the next merge point, which is a specialisation decision by default (QUESTIONS 42; richards' `Task.__init__` 10 → 54 KB with inline stores). [run]

All four are one problem: a fact gained in one place decides how many copies are made somewhere else, and neither place knows about the other.

## The policy

**Specialisation point**: a promotion site or a merge point. Each one keeps copies of the code after it, keyed on what it knows, up to a limit, then uses a generic copy. The rules below apply to both.

1. **Every fact has a reason to be kept.** A fact is kept past the next specialisation point only if code after it relies on it. Psyco already marks this for constants (`SkFlagFixed`: something emitted depends on the value). The policy extends it: a promoted type that nothing after the merge point has used yet is not a reason to start a new block. [inferred]
2. **Each point has a limit, and the generic copy is final.** Past the limit, the point compiles one generic copy, and what it generalised is marked **forgotten**: no later point may specialise on it again. Without the mark, generalising only moves the copying downstream (2 above). [inferred]
3. **The mark travels with the value.** A forgotten value is part of the state, kept in snapshots and compared at merge points, so a respawn replay sees it (D21). Values derived from it, such as its type, are forgotten too, or promotion of the type undoes it. [inferred]
4. **At a polymorphic site, share the code after it.** Once a promotion site has seen K values, its continuation is compiled once with the value forgotten, and only the operation itself depends on the value, through a cache keyed on the value (option D). [inferred]
5. **A branch compiled late rejoins.** A respawned branch, or one entered from a coding pause, compares its state with existing blocks at the first point where they can match, instead of running to the next merge point on its own. [inferred]
6. **One budget, measured per function.** Limits are measured against code size and compile time per function, not only per point (D29). A function that has already compiled many copies should generalise sooner. A policy choice, not yet measured. [inferred]

### What stays as it is

- **Fixed facts are never dropped.** Something emitted relies on them, and un-promoting one that a promotion compiled from would loop (D40). [read]
- **Identity is kept.** Generalising may make a value run-time, never recompute an object that `is` could see (D33, D41). [read]
- **Replays are deterministic.** A decision may depend on the state, the snapshots and one-way state read through `psyco_block_serial` (D21, D31), never on counts that a replay would see differently. Merge-point decisions are safe because a replay never passes a merge point (D40). [read]

## Steps

In order, each measured on code size, compile time and speed (D29):

1. **Forgotten values** (done, D43): the mark (`RunTime_Megamorphic`), honoured by every promotion entry that has a generic path, set by the cap. Lets the cap generalise more than ints. Rules 2 and 3, except that a forgotten value's type is still promoted (`Psyco_NeedType` has no generic path).
2. **Respawned branches rejoin** (in flight, QUESTIONS 42): rule 5. Then inline stores (D38) merge on top.
3. **Merge points match across an unused fact**: rule 1. A state that knows a promoted type nothing has used yet matches a block without it. A naive version that dropped the check made every benchmark hang (CODE_SIZE "Promotions"), so it needs the "used" information and a guard against promoting and generalising in a loop.
4. **Shared code after polymorphic sites** (option D, BACKLOG): rule 4, built on the mark from step 1. Design first.
5. **A shared megamorphic table**, keyed on (type, operation), if megamorphic sites still show in profiles.
6. **One budget per function**: rule 6, once 1–4 show how much is left.

The out-of-line chain of cases at promotion sites (in flight, option B) is a mechanism under this policy: it makes the run-time choice cheaper, and changes nothing about which copies exist.

## Open questions

- **Where a fact's "used" bit lives.** `SkFlagFixed` exists for constants only. A promoted type is a constant once promoted, so the bit may already be there; whether it is set early enough to be useful at the next merge point is unmeasured. [inferred]
- **Whether two states that differ only in the forgotten mark should match.** Matching is safe if the forgotten one is the more general; the reverse would let specialisation back in.
- **K for rule 4**, and whether it should be the megamorphic limit.
- **How this maps onto 3.12** (PORT312.md): 3.12's specialising interpreter has its own per-instruction caches; whether Ni reads them as hints is QUESTIONS 30.
