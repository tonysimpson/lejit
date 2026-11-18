# One policy for specialisation

Ni specialises code in two places, promotion sites and merge points (GLOSSARY, "Shared plain words"). The mechanisms stay separate; the policy (how many copies to keep, which facts are worth keeping, when to stop specialising) is one.

| | promotion site | merge point |
|---|---|---|
| keys on | one value: a type, a function, an int | the whole state |
| chooses | at run time: a compare chain on the actual value (D42) | at compile time: `psyco_compatible` picks a block |
| copies | one block per value seen | one block per state seen |
| limit | megamorphic, `MEGAMORPHIC_MAX` (5) values, not counting the megamorphic version (D66); every type promotion has one (D135) | the cap, 6 blocks (D40) |
| generic copy | the megamorphic path: the value stays run-time | a block compiled from a generalised state |
| code | `dispatcher.c:psyco_finish_promotion`, `do_promotion_*`, `x64/idispatcher.h` | `dispatcher.c:psyco_compatible`, `vcompiler.c:psyco_compile` |

Why one policy: a fact gained at one point decides how many copies are made at another. A promotion adds a fact that splits the next merge point; a value the cap generalises is specialised again by the next promotion; a polymorphic site copies everything after it; a respawned branch used to run on alone to the next merge point.

## The rules

**Specialisation point**: a promotion site or a merge point. Each keeps copies of the code after it, keyed on what it knows, up to a limit, then uses a generic copy.

1. **Every fact has a reason to be kept.** A fact is kept past the next specialisation point only if code after it relies on it. Done for merge points (D50, D57): a run-time value's known field that nothing since the last merge point relied on is dropped when no block matches exactly. "Used" is `source_known_t.used`, the serial of the last block that relied on the fact, compared with `PsycoObject.mp_serial`; not `SkFlagFixed`, which a promotion sets at birth. A promotion's own compare at its instruction never drops, or it would loop.
2. **Each point has a limit, and the generic copy is final.** Past the limit, what was generalised is marked **forgotten** (`RunTime_Megamorphic`), and every promotion with a generic path takes it for a marked value (D43). Without the mark, generalising only moves the copying downstream.
3. **The mark travels with the value.** It is kept in snapshots and compared at merge points, so a replay sees it (D21). Values derived from a forgotten one should be forgotten too; a forgotten value's type is still promoted, but at a `Psyco_NeedType` site that counts it: its callers need a type, so past the limit the site's version has no generic code and the frame goes on in ceval from that instruction (D135); not in a generator or coroutine, which promotes without limit.
4. **At a polymorphic site, share the code after it** (option D): once a site has seen K values, compile its continuation once with the value forgotten, the operation depending on it through a cache. Measured, small on pyperformance; not on 3.7 (FINDINGS.md, QUESTIONS 43–46). K stays `MEGAMORPHIC_MAX`.
5. **A branch compiled late rejoins.** Done at split sites (D44, D49): a respawned branch joins the block the site already starts. Branches from coding pauses already meet merge points; respawn points that aren't split sites are left.
6. **One budget, measured per function.** Limits judged against code size and compile time per function, not only per point (D29); a function that has compiled many copies should generalise sooner. Not yet measured.

## What stays as it is

- **Fixed facts are never dropped.** Something emitted relies on them, and un-promoting one that a promotion compiled from would loop (D40).
- **Identity is kept.** Generalising may make a value run-time, never recompute an object that `is` could see (D33, D41).
- **A hint is not a fact.** A value can carry a hint, such as the type it had before D59 forgot it (`vinfo_t.type_hint`, D63): it decides what code is emitted (a guard instead of a promotion), so it is kept in snapshots for replays, but states are compared without it.
- **Replays are deterministic.** A decision may depend on the state, the snapshots and one-way state read through `psyco_block_serial` (D21, D31), never on counts a replay would see differently. Merge-point decisions are safe because a replay never passes a merge point (D40).
- **Joining can merge promotion sites.** Two blocks' sites become one that sees both sets of values; a site's room is fixed at the site, not by keeping copies (D66).

## Open

- A shared megamorphic table keyed on (type, operation), if megamorphic sites show in profiles.
- Whether two states that differ only in the forgotten mark should match: safe if the forgotten one is the more general, never the reverse.
- Rule 6, once the rest shows how much is left.
- 3.12's specialising interpreter has its own per-instruction caches; whether Ni reads them as hints is QUESTIONS 30.
