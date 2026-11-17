---
written: 2026-11-17
head_ref: 6b9d7c2
status: design note and measurements; SPECIALISATION.md step 4 (rule 4). Nothing built.
---
# Option D: shared code after polymorphic promotion sites

Today each value a promotion site sees compiles its own copy of the code after the site. Option D: once a site has seen K values, compile that code once with the value forgotten, so that only the operation that needed the value depends on it, through a run-time cache (as JavaScript inline caches do). This note measures what that could save, then sets out how it would work in Ni, what it would cost in speed, and a staged plan.

**Verdict.** On pyperformance the most Option D could save is small. The upper bound over all 23 benchmarks (2,413 KB) is 18.1 KB (0.75%) at K = 2 and 5.6 KB (0.2%) at K = 3. Only richards (5.1 / 1.1 KB), deepcopy (4.7 / 3.1), deltablue (2.8 / 0.1) and scimark (0.3 / 0.2) gain at all. The copies it would remove run often, so a generic or cached operation would cost more time than the code is worth: richards about +5–8% time for −4.1% code at K = 2, and +2% for −0.9% at K = 3. K = 2 also slows every two-value site for nothing (raytrace: 2 M runs per iteration). Do not build the general mechanism on 3.7. If it is built later, the first step is the cached operation at the existing megamorphic cut-off, which changes no copy counts (below, "Plan"). [run] for the numbers, [inferred] for the cost.

## Why it saves little [run]

1. **Polymorphic sites are rare, and most see two values.** Over the 23 benchmarks, 90 sites compiled code for two or more values during the measured run: 70 saw two values, 10 saw three, 4 saw four and 6 saw five (the cut-off). 79–100% of sites see one value (CODE_SIZE.md "Promotions").
2. **Switching at the K-th value saves nothing until a site has more than K values.** The first K − 1 values keep their copies and the K-th gets the shared one, which is one copy too. At K = 2 a two-value site, the common case, gets exactly as many copies as today and only loses speed; only the 20 sites with three or more values gain.
3. **Each value's copy is short.** It runs until the next promotion (where the block ends) or the next merge point it matches. Typical copies are 100–2,000 bytes: richards' four-way `schedule` site is 594 bytes per value.
4. **Where a copy is large, the value is used downstream.** unpack_sequence's one polymorphic site (`UNPACK_SEQUENCE` on a `tuple` or a `list`) compiles 120 KB for `list` because every later unpack in the function relies on the type. That is specialisation doing its job; forgetting the type would make all 400 unpacks generic.

## Measurements [run]

### Method

`NI_CODESTATS=1` release build at `8081f96`, `PYTHONHASHSEED=0`, `tools/codesize.py <bench> -D`, each benchmark run until it compiles nothing new (as `-p`). Two instrumentation changes, both `NI_CODESTATS` only (the preprocessed release and checked `pycompiler.c` differ only in `__FILE__`/`__LINE__` strings): [read]

- Each per-instruction copy in `ni._codestats()` records the index of the promotion site's value it was compiled for (`pycompiler.c:codestats_flush`, `cs_promo_vidx`, `c3a0e22`).
- Each site in `ni._promostats()` records its parent's value index as well as its parent site, so a value's code can include the sites its code emitted and their values, down the tree (`8081f96`).

`promostats.py:shared_after` then reports, for each site that compiled code for two or more values (megamorphic versions left out):

- **Own block**: the bytes compiled for each value in the value's own compile.
- **Tree**: the own block plus the code of every site it emitted, and of theirs, summed over the outermost polymorphic sites only, so that nested sites are not counted twice.
- **Same / consts / differs**: each instruction compiled for every value is *same* if its copies disassemble identically, *consts* if they are identical once 64-bit constants (type pointers, guard and keys addresses) are aside, and *differs* otherwise. An instruction compiled for only some values counts as *differs*.
- **Upper bound saved at K**: the code of the K-th and later values less the largest of them. This assumes one shared copy is kept and is no larger than the largest specialised one. K = 1 is "share every value", which needs foreknowledge, because the first copy is compiled before anyone knows the site is polymorphic.
- **Runs**: how often the blocks of the K-th and later values ran, per iteration, averaged over the measured run (which starts with the iterations that compile). This is what would run the shared copy instead.

Not attributed to a value: code reached later through a coding pause or a respawn inside its continuation, which counts as a pause or a respawn (unpack_sequence's `tuple` copy shows 5 KB against `list`'s 120 KB, probably because of this [inferred]). Stub-pool chunks and promotion stubs written into other buffers are not counted either. So the trees are a lower bound on what each value cost, and the savings are an upper bound only given that.

### Per benchmark

The 13 benchmarks not listed compiled code for at most one value at every site in the measured run: chaos, comprehensions, coroutines, fannkuch, float, gc_collect, gc_traversal, generators, hexiom, meteor_contest, nbody, nqueens, pyflate. Code is all emitted code (KB); "own" is the values' own blocks; same / consts / differs split the own blocks.

| benchmark | code KB | sites (values: sites) | own KB | same / consts / differs KB | tree KB | saved K=1 (foreknowledge) | saved K=2 | saved K=3 | runs/iteration of values ≥ 2nd / ≥ 3rd |
|---|---|---|---|---|---|---|---|---|---|
| richards | 124.0 | 3 (2:1, 3:1, 4:1) | 6.2 | 0.5 / 5.7 / 0.0 | 16.0 | 10.4 (8.4%) | 5.1 (4.1%) | 1.1 (0.9%) | 74,116 / 24,658 |
| richards_super | 123.9 | 3 (same) | 6.2 | 0.5 / 5.7 / 0.0 | 16.3 | 10.7 (8.6%) | 5.2 (4.2%) | 1.1 (0.9%) | 74,116 / 24,658 |
| deepcopy | 64.5 | 7 (3:1, 4:1, 5:5) | 4.9 | 0.0 / 4.9 / 0.0 | 9.8 | 6.6 (10.3%) | 4.7 (7.3%) | 3.1 (4.9%) | 117 / 14 |
| deltablue | 405.1 | 26 (2:18, 3:7, 4:1) | 19.4 | 2.6 / 5.2 / 11.6 | 64.2 | 25.2 (6.2%) | 2.8 (0.7%) | 0.1 | 4,137 / 749 |
| raytrace | 205.1 | 34 (2:34) | 16.5 | 1.7 / 3.2 / 11.6 | 58.5 | 21.0 (10.2%) | 0 | 0 | 2,048,831 / 0 |
| unpack_sequence | 138.9 | 1 (2:1) | 122.9 | 0 / 0 / 122.9 | 122.9 | 5.0 (3.6%) | 0 | 0 | 1 / 0 |
| go | 289.4 | 6 (2:6) | 1.5 | 0.2 / 0.0 / 1.3 | 7.4 | 1.7 (0.6%) | 0 | 0 | 43,193 / 0 |
| mdp | 156.8 | 4 (2:4) | 1.9 | 0.2 / 0.2 / 1.5 | 6.5 | 2.1 (1.4%) | 0 | 0 | 362,271 / 0 |
| spectral_norm | 18.8 | 4 (2:4) | 0.6 | 0.0 / 0.1 / 0.5 | 5.1 | 1.7 (8.9%) | 0 | 0 | 323,700 / 0 |
| scimark | 168.7 | 2 (2:1, 5:1) | 0.3 | 0.0 / 0.2 / 0.1 | 1.4 | 0.7 (0.4%) | 0.3 (0.2%) | 0.2 (0.1%) | 35 / 1 |
| **all 23** | **2,413** | **90** | **180** | | **308** | **85.1 (3.5%)** | **18.1 (0.75%)** | **5.6 (0.23%)** | |

Savings use the tree, as a share of all code emitted. With own blocks only: K=1 34.5 KB, K=2 8.3 KB, K=3 3.3 KB over all 23.

**Who asks**, at the sites with the most code:

- **richards / richards_super.** `schedule` @34 `LOAD_METHOD` (`t.isTaskHoldingOrWaiting()`, four `Task` subclasses, 594 B each), `qpkt` @42 `LOAD_METHOD` (two), `Task.__init__` @6 `STORE_ATTR` (three). Every copy is the same code but for constants: the method is the base class's, so only the type pointer and shared-keys address in the guards differ. These are the copies a cache would remove cleanly. [run]
- **deepcopy.** Five classes, all named `C`, made afresh by the benchmark, at `_reconstruct` @94 `LOAD_METHOD`, `__init__`, `__setstate__` and `__reduce__`. Also `deepcopy` @72 `CALL_FUNCTION` on the copier function (four code objects: `_deepcopy_list`, `_atomic`, `_tuple`, `_dict`). Again identical but for constants. Their blocks ran 117 times per iteration in all. [run]
- **deltablue.** Constraint subclasses at `satisfy` @76, `incremental_add` @30, `execute` @14 and `make_plan` @40 (`LOAD_METHOD`), and `__init__` `STORE_ATTR`s. Mostly *differs*: the subclasses' methods differ, so the code after the call does too. [run]
- **raytrace.** Every polymorphic site has two values: `int` and `float` at `BINARY_MULTIPLY`/`BINARY_SUBTRACT` in `Vector.__sub__`, `dot` and `cross`, and `Sphere`/`Halfspace` and the two surfaces at `LOAD_METHOD`. Two values means nothing to save at K ≥ 2, and their blocks run 2 M times per iteration. [run]
- **go, mdp, spectral_norm.** Two values at `PsycoObject_IsTrue` (`NoneType` or `UCTNode`), `GET_ITER` (`list` or `tuple`) and the like. Nothing to save at K ≥ 2. [run]

### Outside pyperformance: importing pkg_resources

`pkg_resources` imports pyparsing, the case CODE_SIZE.md "Promotions" found with 36 sites of five or more values. The run imports it, then three times lists the working set and parses two requirements (scratch driver calling `promostats.shared_after`). [run]

- Code emitted: 4,741 KB.
- 159 polymorphic sites: 99 with two values, 24 with three, 11 with four, 25 with five.
- Own blocks 146.7 KB: same 11.6, consts 29.1, differs 106.0. Trees (109 outermost sites) 308.6 KB.
- Saved: K=1 131.9 KB (2.8%), K=2 59.9 KB (1.3%), K=3 31.5 KB (0.7%), K=5 0.1 KB.
- The big sites are pyparsing's `ParserElement` subclasses at `__init__` `STORE_ATTR` (2.4 KB per value) and at `LOAD_METHOD` in `__call__`, `parseImpl` and `leaveWhitespace`.
- **Megamorphic versions today:** 51 sites have one; they total 8.8 KB, against 21.9 KB for one average specialised copy at each of those sites. A generic copy is smaller than a specialised one.
- **But the code after a megamorphic version re-specialises.** It emitted 70 new promotion sites (20 at `CALL_METHOD`, 6 at `CALL_FUNCTION`), with 13.6 KB compiled after them. The attribute's value (a bound method, a function) is a new, unmarked run-time value, so the next call promotes it. The mark (D43) is on the type only. [run]; why: [read] (`pobject.c:need_type_mega`, `pfuncobject.c:pfunction_call`)

### What a generic operation costs [run]

A micro-benchmark: one loop per case with its own code objects; 24 objects cycling through k classes; per operation, the best of 7 runs of 20,000 × 24. Release build at `8081f96`, load average 3–4.

| k classes | method call `o.m()` | attribute `o.x` | CPython 3.7 |
|---|---|---|---|
| 1 | 2.6 ns | 2.9–3.0 ns | 47.1 / 17.7 ns |
| 2 | 2.6 | 2.8 | 50.0 / 25.1 |
| 4 | 2.6 | 2.8 | 50.7 / 27.0 |
| 6 | 4.6 | 4.1–4.2 | 50.6 / 26.4 |
| 8 | 6.8–7.0 | 5.4 | 50.9 / 26.0 |

- Up to four classes the chain of cases (D42) costs nothing measurable.
- Past the cut-off, five classes keep their specialised copies and the rest take the megamorphic version. From the mix, a generic method call costs about 14 ns and a generic attribute load about 10 ns, against 2.6–2.8 ns specialised [inferred from the averages]. So the generic path costs 7–11 ns more per operation, and is still 3–4 times faster than CPython.
- An addition on `int` or `float` (3.1 / 2.9 ns, also 2.9 ns mixed) has no generic path to compare (below).

## Which promotions are candidates

| promotion | where | what the cached operation would be | candidate? |
|---|---|---|---|
| type, for an attribute load or method load | `pobject.c:need_type_mega` < `PsycoObject_GetAttr`, `generic_getattr` (`LOAD_ATTR`, `LOAD_METHOD`) | keyed on the type: (version tag, descriptor, kind), with kind one of {data descriptor; instance dict then descriptor; plain function, for a method; other}, and for instance attributes the shared keys and the index (D31's `keys_guard_t`) | **yes**: richards' and deepcopy's copies are identical but for these constants |
| type, for an attribute store | `need_type_mega` < `PsycoObject_SetAttr`, `heap_setattr` | keyed on the type: the store kind (`store_kind`) and the guard (D39's `store_guard`, already shared per (type, name, kind)) | **yes**: the guard is already a run-time structure; only the type has to be found at run time |
| function value at a call | `pfuncobject.c:pfunction_call` (the code object first, then the function), `pmethodobject.c` | keyed on the code object: the entry point of a callee compiled for this call site's argument state | **later**: needs callees compiled at run time from the site's frozen state (QUESTIONS 45) |
| type, for a binary op or compare | `Psyco_NeedType` < `binary_op1`, `try_rich_compare` | keyed on (type, type): the slot function | **no**: the specialised copy keeps ints and floats virtual (D33), and a cached slot call boxes every result. raytrace's `int`/`float` sites run 2 M times per iteration and have two values: nothing to save, all loss |
| type for truth, iteration, unpacking, `len`, subscripts | `Psyco_NeedType` < `PsycoObject_IsTrue`, `GetIter`, `UNPACK_SEQUENCE`, ... | the slot | **no**: these values are used downstream (unpack_sequence), and the sites have two values |
| machine word | `psyco_atcompiletime` | none | **no**: no generic path, and 1.4% of promotion code |

[read] for where each is promoted, [run] for the data behind each verdict.

### What each cached operation looks like

- **`LOAD_METHOD`/`LOAD_ATTR` keyed on type.**
  - Emitted: load the type, compare it with the cache's entries (D42's `mov r10, imm64; cmp; je` cases, written into the stub pool by C), and load the entry's descriptor and data.
  - On a miss: a residual call to a C helper that does 3.7's `_PyObject_GenericGetAttrWithDict` steps once (`_PyType_Lookup`, kind, version tag) and adds an entry.
  - For a method of kind "plain function", push (function, self) as ceval does, instead of Ni's (NULL, bound method). That avoids allocating a bound method, which a generic `PyObject_GetAttr` does today.
  - The descriptor is a run-time value marked forgotten, so the call after it takes the generic path, or the call cache below. Otherwise the call promotes it again, as it does after megamorphic versions today (70 sites in the pkg_resources run).
  - Simplest first form: no inline cases at all, just one residual call to a helper holding a small per-site table. It costs one C call (about 5 ns [inferred]) but no code per entry.
- **`STORE_ATTR` keyed on type.** The helper finds `store_guard(tp, name, store_kind(tp, name))` and stores. With the table inline, the shared code tests the guard of the entry it found instead of a guard fixed at compile time. D39 already makes the guard valid for any type in the state that gave that kind, and repairs it at run time.
- **`CALL` on a function value keyed on the code object.**
  - Emitted: load `func_code`, compare it with the entries, and call the entry's code pointer with Ni's calling convention, passing the function's globals and defaults at run time.
  - On a miss: compile the callee for the site's argument state, which is the same for every entry, because nothing about the arguments depends on the function. This is a compile at run time from a frozen state, as `do_promotion_internal` does, but it produces an entry point rather than a continuation.
  - The return value is run-time either way, so the code after the call is shared naturally.
- **Binary op keyed on (type, type).** `call [entry + 8]` to the cached `nb_*` slot, then a `NotImplemented` check. Recorded for completeness only: it is not a candidate.

### What it reuses

- **The megamorphic version** (`dispatcher.c:detected_megamorphic_body`): it already compiles one continuation, from the site's frozen pre-promotion state (`fs->po`), with the value flagged `RunTime_Megamorphic`. Option D is that, reached at K instead of `MEGAMORPHIC_MAX`, plus a better operation in it. [read]
- **The chain of cases** (D42, `promotion_cache_fallback`): once the shared version exists, the site's chain ends in `jmp` to it, so every new value goes there without entering C. The same compare/je entries can serve as the operation's inline cache. [read]
- **The forgotten mark** (D43, `is_megamorphic`): it makes the operation in the shared copy take its generic or cached path, and stops merge points from letting the shared state into a specialised block (a flagged value never matches an unflagged one in a snapshot). [read]
- **Store guards** (D39) and **keys guards** (D31): per-site or per-(type, name, kind) run-time structures that compiled code reads and C repairs, with no compile decision reading them. A cache entry is the same kind of thing. [read]

## How the continuation is compiled once

- **The decision is made at run time, in `do_promotion_*`.** `lookup_count_previous_values` already counts the site's values there (the megamorphic test). At K, the dispatcher compiles the shared version instead of a new value's copy, as `detected_megamorphic_pyobj_site` does at five. No compile decision reads the count. [read]
- **One-way by construction.** After the switch, the chain's end is a `jmp` to the shared version and the move-to-front list holds its `-1` marker, so later values never compile a copy. Neither `psyco_block_serial` nor a new mark is needed: the switch is never replayed. [read]
- **Replays (D21).** A promotion ends its block, and `psyco_finish_promotion` asserts that no replay reaches one. The shared version's own respawn points replay from its own snapshots, where the value is flagged (the mark is in the source word, D43), so a replay takes the same generic or cached path. The cache contents are written only at run time, and the emitted code is the same whatever they hold, as with D31's keys guard. [read], [inferred] that nothing new breaks this
- **Merge and join points.**
  - The shared version's state carries the flagged value. It can match blocks compiled by an earlier shared or megamorphic version (flagged), but not the specialised copies' blocks: there the type is fixed, and the snapshot either lacks it or holds another one. So the shared and specialised paths stay apart until the value dies, as they do today. [read] (`dispatcher.c:compatible_vinfo`, `compatible_array`)
  - Letting a specialised copy drop its fixed type and jump into the shared block is SPECIALISATION step 3 (rule 1), not this.
  - At join points (D44, D49) nothing changes: only a respawned branch drops facts, and a forgotten value never matches an unflagged one.
- **Derived values must be forgotten too** (rule 3). The cached operation's result (a descriptor, a function) depends on the forgotten type, so it must be flagged, or the next call promotes it per value and the copying just moves downstream. The pkg_resources run measures that this happens today: 70 sites and 13.6 KB downstream of 51 megamorphic versions. [run]
- **Copies already compiled stay.** Ni never frees code (`do_promotion_internal_body`: "don't know what to do with reference to 'codebuf'"), and the values already seen keep their entries in the inline cache and chain. That costs nothing at run time and keeps their speed. Only values after the K-th share. [read]

## When to switch

- **K = 2** saves 18.1 KB over all 23 benchmarks (richards 5.1, deepcopy 4.7, deltablue 2.8, scimark 0.3), all at the 20 sites with three or more values, and 59.9 KB on the pkg_resources import (1.3%). It loses speed at every two-value site, where it saves nothing: raytrace's would run 2 M blocks per iteration generically, mdp's 362 k, spectral_norm's 324 k. Ruled out. [run]
- **K = 3** saves 5.6 KB over all 23 benchmarks (1.1 KB richards, 3.1 KB deepcopy) and 31.5 KB on the pkg_resources import (0.7%). [run]
- **K = 5, the megamorphic limit,** saves nothing more than today: the cut-off already shares from there (0.1 KB on pkg_resources). What it can change is the operation in the shared copy, its speed, and the derived values. [run]
- **Recommendation:** keep K = `MEGAMORPHIC_MAX` and improve what the shared copy does. Revisit K only with a workload where K = 3 or 4 shows more than about 1% of code; none of the 23 does. [inferred]
- **Per kind of promotion.** Only attribute and method sites (and later calls) switch at all. `Psyco_NeedType` sites have no cut-off today (CODE_SIZE.md option 4), and giving them a generic path is a separate backstop.

## Speed against code

- **The cost.** Each run of the shared copy pays for the operation's lookup instead of a compare/je, and loses whatever the code after it knew about the value.
  - For a fully generic operation, that is 7–11 ns per run (micro-benchmark above).
  - For a cached helper call, perhaps 3–5 ns. For inline cache entries, perhaps 1–2 ns, but the call after a method load is no longer direct or inlinable without the call cache. [inferred]
- **richards at K = 2:** 74,116 runs per iteration × 7–11 ns ≈ 0.52–0.82 ms, so +5 to +8% time for −5.1 KB (−4.1%). [inferred from [run] counts and timings]
- **richards at K = 3:** 24,658 runs per iteration × 7–11 ns ≈ 0.17–0.27 ms, on a warm iteration of about 10.7 ms (CODE_SIZE.md, "Polymorphic inline cache"). So +1.6 to +2.5% time for −1.1 KB (−0.9%) code. With a cached operation, perhaps +0.5–1%. Not worth it. [inferred from [run] counts and timings]
- **deepcopy at K = 3:** 14 runs per iteration, so no measurable cost, for −3.1 KB (−4.9%). Its polymorphism is fresh classes, five of them, so K = 5 catches them too, and 5 of its sites already go megamorphic (CODE_SIZE.md "Promotions"). [run]
- **deltablue at K = 3:** 749 runs × 10 ns ≈ 0.007 ms of 1.47 ms (0.5%), for 0.1 KB. [inferred]
- **Compile time** follows code (about 157 KB per ms, COMPILE_COST.md), so at most about 0.04 ms over all 23 benchmarks. [inferred]
- **How to measure** a candidate, with a temporary switch for K and the operation:
  - `tools/codesize.py <bench> -D` (code, trees, runs);
  - `tools/compilebench.py` (code, compile time, warm time, all 29);
  - `perf stat -e instructions:u,cycles:u,branch-misses:u` per warm iteration on richards, deltablue, deepcopy, raytrace and go, as D42 did;
  - the micro-benchmark above (k = 1…8) for the operation alone;
  - the pkg_resources import for a real program;
  - the checked build's replay check (`respawn_check`) on the seven replay-checked benchmarks, since the shared copy has respawn points of its own.

## Plan (if it is built)

Each step is measured as above and can stop the plan.

1. **Done: measurement** (`c3a0e22`, `8081f96`). `codesize.py -D`.
2. **Smallest useful step: a cached operation in the megamorphic version of attribute and method sites, its result forgotten.**
   - At K = 5 (unchanged), `generic_getattr` with `r == 0` makes a residual call to a helper with a per-site table, instead of `PyObject_GetAttr`. For a method of kind "plain function" it pushes (function, self).
   - The result is flagged `RunTime_Megamorphic`, so `pfunction_call` takes its generic call instead of promoting the function.
   - Expected effect:
     - pyperformance unchanged: no site there compiled a fifth value in the measured runs, and only 9 sites went megamorphic at all (CODE_SIZE.md "Promotions"), none in a warm loop [run], [inferred] for "unchanged";
     - pkg_resources: up to the 13.6 KB and 70 sites downstream of megamorphic versions gone;
     - megamorphic attribute loads maybe 10 → 5 ns and no bound-method allocation [inferred].
   - Risk: a generic call through a forgotten function is slower than a promoted one where a megamorphic site's methods are few. Measure the micro-benchmark at k = 6 and 8.
3. **The call cache** (CALL keyed on the code object → cached entry), so that the forgotten function from step 2 is called directly. This needs a callee compiled at run time from the call site's frozen state (QUESTIONS 45).
4. **Lower K for attribute and method sites only**, if a workload shows a gain (none of the 23 does: 5.6 KB at K = 3).
5. **A shared megamorphic table keyed on (type, operation)** (SPECIALISATION step 5), if step 2's per-site tables show up in profiles.

Binary ops, compares, truth and unpacking stay out: their copies are real specialisation (virtual ints and floats, downstream use) and their sites have two values.

## Risks and open questions

- **Speed.** Every step trades speed at sites that run warm for code at sites that mostly don't. On pyperformance the trade is negative at every K that saves code (richards above). [inferred]
- **Derived values.** Forgetting the attribute's result stops re-promotion, but a megamorphic site whose methods are few (the same base-class method for many subclasses, as in richards) would then call generically where today it promotes one function. A cache keyed on the descriptor, not the type, would keep that. (QUESTIONS 44)
- **The call cache** compiles callees outside the normal call path, at run time, from a frozen caller state. finfo, recursion depth (`DEFAULT_RECURSION`) and the tracing test (D46) must work there. (QUESTIONS 45)
- **Attribution.** `-D` misses code reached through pauses and respawns inside a value's continuation, so the trees are lower bounds (unpack_sequence's `tuple` 5 KB against `list` 120 KB). A pause or respawn record could carry the site and value it was emitted under. (QUESTIONS 46)
- **3.12.** CPython's specialising interpreter already keeps per-instruction inline caches (`LOAD_ATTR_INSTANCE_VALUE`, `LOAD_ATTR_METHOD_WITH_VALUES` and the like). Where a site is polymorphic there, Ni could start from the shared form at the first value, which is the K = 1 column (85 KB, 3.5% over all 23) with no foreknowledge needed (QUESTIONS 30). [inferred]
- **Whether to build it at all** on 3.7 is for the owner (QUESTIONS 43). GOAL.md puts option D in the 3.12 backlog unless it blocks the baseline, and these numbers don't say it should.

## Reproducing

```
NI_CODESTATS=1 python3.7 setup.py build_ext --inplace
PYTHONHASHSEED=0 python3.7 tools/codesize.py richards -D     # needs capstone
```

The pkg_resources run and the micro-benchmark were scratch scripts. They are described above with enough to rewrite them: `promostats.shared_after(ni, stats, ni._promostats(), total, top, normalise)` after the workload, and per-case code objects compiled with `exec`.
