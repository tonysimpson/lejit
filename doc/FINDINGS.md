# Findings of the finished studies

What each study found and what was decided, with the decision (D) and the tool that measures it. The full write-ups, with their tables and the runs behind them, are in git: `git show e404100:doc/<NAME>.md` for STORES, CODE_SIZE, OPTION_D, CLOSURES, GENERATORS and COMPILE_COST. New measurements go in commit messages.

## Measuring

- **Instructions per warm iteration** (the speed figure used throughout): release build, `PYTHONHASHSEED=0`, `perf stat -e instructions:u` on three compiling iterations then N, minus three then none (or `tools/profile_bench.py` at two lengths), divided by the difference in iterations. go varies by ±3–4% between runs of one build; most others by under 1%. A move of about 1% that only libc's malloc carries is heap state, not code: check it as PROFILE.md "Heap state" says (D92).
- **Code and compile time:** `tools/compilebench.py [-k NAME] [-v]` on a release build: per benchmark, CPython's and Ni's first and warm iteration, Ni's compile time by entry kind (`ni._stats()`), code bytes, break-even. Its code column moves in whole 4 KB stub chunks.
- **Where code goes:** `NI_CODESTATS=1 python3.7 setup.py build_ext --inplace`, then `python3.7 tools/codesize.py BENCH` with `-s` (what differs between blocks at one merge point), `-d` (identical copies; needs capstone, see BUILD.md), `-p` (promotion sites, through `tools/promostats.py`; `-j FILE` also writes JSON), `-D` (code after sites with several values).
- **Same code:** `python3.7 tools/samecode.py A B` (below).

## Attribute stores and load guards

Tools: `tools/storeclasses.py BENCH` (plain CPython: what each `STORE_ATTR` does to the instance dict), `tools/store_micro.py ni|plain` (ns per store), `tools/store_cache_sim.py BENCH` (option 4's cache, simulated), `tools/loadguards.py BENCH... | SCRIPT` (`NI_CODESTATS` build: load guard failures and why), `tools/load_micro.py ni|plain`. Counters: `ni._stats()['store_misses']`, `['store_refreshes']`, `['store_type_changes']`, `['load_type_changes']`.

- **Found:** stores were 30–38% of richards, about 13% of deltablue, 11% of go and 10–15% of raytrace and chaos. Each benchmark has one main kind: richards and deltablue overwrite existing keys of a split (shared-key) dict, go mostly overwrites keys of combined dicts, raytrace and chaos fill new objects in `__init__`. Growing a class's shared keys never happens in a timed loop. Compiled stores ignored a `__setattr__` added to the class later (F37, fixed).
- **Built:** a type guard shared per (type, name, what the store does) and repaired at run time, then a direct `_PyObjectDict_SetItem`, or `_PyDict_SetItem_KnownHash` for a combined dict (options 1 and 2, D39). Then the inline store (option 4, D38): a cache per (class, name) that holds no keys reference and is checked against the live dict (present, split, keys of the cache's size, the name at its entry, GC tracking); it overwrites a set value, or fills the slot at `ix == ma_used` for stores to `self` in `__init__`; the version tag comes from Ni's own counter, from 2^63 (D35). The size check is load-bearing, not only a bounds check (`stores.size_mismatch`). The sites were then shrunk (D47): one hand-allocated emitter in caller-saved registers, every failed check calling one miss helper, a shared dealloc glue, and a 47-byte type guard that loads use too. Richards' code ended +10% over the baseline's (budget +20–25%).
- **Load guards** are shared and repaired the same way (D60). No benchmark changes a class after warm-up; the failures measured were classes with a metaclass, which 3.7 never gives a version tag (`typeobject.c:type_mro_modified`), so Ni sets the flag where CPython means to keep it (`pobject.c:enable_version_tag`): mdp −10.8% instructions.
- **Rules any store keeps:** every run-time condition before anything that can run Python code, so a replay decides the same (D21); incref, store, tag, then the old value's decref last; an untracked dict is tracked for a value that may be tracked; only an existing value or the slot at `ix == ma_used` stays split; deletes stay generic.
- **Open** (BACKLOG): checks shared between consecutive stores to one object (about 3% of richards' code; needs one guard per run of names and a `__del__` re-entry that a replay decides the same; the tests are in `tests/cases/stores.py`); present-name loads on the store cache (QUESTIONS 39); GC tracking inline; one dealloc glue for every decref; F55.

## Code size and specialisation

At the start, respawn stubs were about half of all code and promotion code about a third, and go compiled each instruction 13.5 times on average, mostly because `Board.useful`'s counters took different forms at one loop head. What was built, in order:

- **Small stubs (D32).** Respawns, coding pauses and promotion misses use 15-byte pooled stubs and one shared trampoline, entered by `jmp` (an unmatched `call` cost go 8% in mispredicted returns). Code fell by 35–45%.
- **One form for small ints at merge points (D33)**, and a known value matches a run-time one: go 1,057 → 426 KB, compile 13 → 3 ms. A **large int constant** becomes run-time from a merge point's second block (D41); computing a virtual to match a run-time object was tried and is not worth a change to replay.
- **A cap of 6 blocks per merge point (D40)**: of 4, 6, 8 and 12, the least code, with no benchmark compiling more than with no cap. **Forgotten values (D43)** past the cap are neutral on pyperformance, kept as a backstop for loops whose locals hold changing functions or classes.
- **Promotions** (`codesize.py -p`): 61–96% of code is compiled after a promotion, and promotions are 57–88% of compile time; 94% of what is promoted is types, and 79–100% of sites see one value, so lower cut-offs and promoting fewer kinds gain little. **Polymorphic inline cache (D42):** the inline compare keeps its first value and four more go in an out-of-line chain. **The megamorphic version is not one of the `MEGAMORPHIC_MAX` values (D66)**, and the cache never evicts a compiled value for it.
- **Respawned branches rejoin (D44, D49)** the block a split site already starts: richards with inline stores 222 → 161 KB, go −34% code without them, speed unchanged.
- **Unused facts at merge points (D50, D57):** a known field that nothing since the last merge point relied on is dropped when no block matches exactly: −1.8% code, speed unchanged. A promotion's own compare at its instruction keeps the fact, or it would loop.
- **eval_breaker after calls (D53)** is tested only after a C call that may block: half of D45's code cost gone.
- **A forgotten type is learnt again by a guard (D63)** against its hint, not a promotion: about half of D59's compile-time cost recovered.
- **Open** (BACKLOG, QUESTIONS): matching across promoted knowledge at merge points (the largest pool left; research); a cut-off for every `Psyco_NeedType` site; type-set guards, for one copy of a base-class method across subclasses (richards' `Task`); why chain placement moves richards' cycles and branch misses; the 16-byte alignment test (KNOWN_BUGS C1) starts up to 1.8% of blocks.

## Same code (`tools/samecode.py`, D70)

`python3.7 tools/samecode.py A B` says whether two builds emit the same machine code, per code object and bytecode instruction, and where each first differs. A and B are git revisions (built with `NI_CODESTATS=1` under `build/samecode/<sha>`, kept for next time), directories holding a `ni*.so`, or the `.so` files. `-k NAME` filters units, `--pyperf` adds the 23 pyperformance benchmarks, `--no-tests`/`--no-bench` drop the defaults, `-v` shows every differing instruction; exit status 1 if anything differs. Needs capstone and `nm`. About 2 minutes, plus about 7 s per revision built the first time.

- **Workload:** each `tests/cases` file, and each of the seven benchmarks until it compiles nothing new, in its own process, made deterministic (`PYTHONHASHSEED=0`, `setarch -R`, no `.pyc` writing, `random` seeded, `tempfile` names fixed). `test/eval_breaker` is timing-dependent: shown, not counted.
- **Compared:** each compiled copy, disassembled with every address named (other instruction copies, live Python objects by type, type fields, C symbols); "same" is D70's multiset per (code object, instruction).
- **Limits:** per instruction, not per meta function; code inlined from a callee counts under the callee; "first" is by bytecode offset, not compile order; only code as first emitted (later patches are not seen); C memory and heap instances compare by kind only.

## Shared code after polymorphic sites (option D)

Measured with `codesize.py -D`; not built. The upper bound over all 23 benchmarks is 18.1 KB (0.75%) at K = 2, where every two-value site also loses speed for nothing, and 5.6 KB (0.2%) at K = 3; 85 KB (3.5%) only with foreknowledge of which sites are polymorphic. The copies it would remove run often (richards: +5–8% time at K = 2). Polymorphic sites are rare and most see two values, each value's copy is short, and where a copy is large the value is used downstream. Decided: not on 3.7 (QUESTIONS 43; moved to 3.12 by D64). If it is built, the first step is a cached operation in the megamorphic version of attribute and method sites, its result forgotten. Open: QUESTIONS 44–46.

## Calls to closures (D58)

`tools/closurecalls.py [-k NAME]` (plain CPython, `sys.setprofile`) counts the closure calls a `CALL_*` makes directly; `tools/closure_micro.py CASE N` gives instructions per call under `perf stat`. The real cases are methods using zero-argument `super()` and closures made by factories (namedtuple); the generic call cost about 1,900 instructions more than a direct one. Now a constant function's cells are compile-time constants, a run-time closure has its code promoted (not the function) and its cells loaded from `func_closure`, and `super()` reads the compiled frame (`pbltinmodule.c:psuper_noargs`): richards_super −25.6% instructions. An inlined call holds a reference to each run-time cell (C49), and a call to the caller's own code is never inlined. Open (BACKLOG): callees with cells of their own, and `**kwargs` wrappers.

## Generators

Generator bodies are never compiled. The eval hook leaves at once for generator code (`21ae5a3`), which took pyperformance generators from 0.88× to 0.99× of CPython; what is left is PEP 523's indirect call. A generator expression consumed at once by `sum`, `any`, `all`, `min`, `max` (no key or default), `list`, `tuple` or `set` is compiled fused into the consumer (`psyfunc.c:psyco_call_genexpr`), only when the generator is a temporary: comprehensions 1.40× → 1.81×. Not done: `min`/`max` with key or default, `str.join`, `sorted`, `dict`. Open: compiled resumable bodies over the CPython frame (option A), only if a benchmark's bodies do real work between yields (nqueens' do), decided with the 3.12 entry design (QUESTIONS 33). Fusing a generator with a compiled `for` loop is not proposed: it interleaves two code objects' bytecode, close to tracing.

## Compile cost

`ni._stats()` gives time and count per way into the compiler (`analyse`, `call`, `pause`, `respawn`, `promotion`), outermost entry only, about 50 ns per entry. Compiling is cheap, about 157 KB of code per ms: Ni pays for itself within 1–3 iterations on almost every benchmark, which supports D29. Promotion is most of the compile time. Code size is the cost to watch more than time. Open (BACKLOG, QUESTIONS 35): instruction-cache effects (`perf stat` icache events), and the run-time cost of merge-point lookup and the promotion cache-hit path.

## Adversarial suite (`tools/adversarial/`)

First run at `a4bd69e` (release build, a busy machine, so ratios within about 10% are noise) [run]. Over 42 benchmarks written to make Ni look bad, warm time is 0.75 of CPython's and cold time 0.95.
- **Wrong answers:** `builtin_subclasses` and `frames_logging` differ from CPython; `frames_logging` also varies between calls.
- **Memory:** `classes_per_request` peaks at 569 MB against 11, and `reload_modules` at 379 against 15 (C14's remainder). Loading Ni costs about 9 MB of RSS in every process.
- **Compile before pay-off:** `import_heavy` is 4.4× slower cold, `many_functions` 13× on the first call and `short_script` 1.24× cold. Ni compiles code the first time it runs, module-level code included.
- **Slower warm:** `reload_modules` 3.2×, `classes_per_request` 2.45×, `exec_templates` 1.45× (compiling fresh code each time), `operator_overloading` 1.27× (Python dunder methods), `lambdas_in_loops` 1.19×, `class_counter` 1.11× (a guard broken on every call), `genexpr_unfused` 1.09×.
- **Faster:** `bytecode_vm` 11×, `class_switch` 6.5×, `deep_recursion` 5×, `text_tokeniser` 3×, the megamorphic benchmarks about 2×.

