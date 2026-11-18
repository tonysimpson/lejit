# Findings of the finished studies

What each study found and what was decided, with the decision (D) and the tool that measures it. The full write-ups, with their tables and the runs behind them, are in git: `git show e404100:doc/<NAME>.md` for STORES, CODE_SIZE, OPTION_D, CLOSURES, GENERATORS and COMPILE_COST. New measurements go in commit messages.

## Measuring

- **Instructions per warm iteration** (the speed figure used throughout): release build, `PYTHONHASHSEED=0`, `perf stat -e instructions:u` on three compiling iterations then N, minus three then none (or `tools/profile_bench.py` at two lengths), divided by the difference in iterations. go varies by ±3–4% between runs of one build; most others by under 1%. A move of about 1% that only libc's malloc carries is heap state, not code: check it as PROFILE.md "Heap state" says (D92).
- **Code and compile time:** `tools/compilebench.py [-k NAME] [-v]` on a release build: per benchmark, CPython's and Ni's first and warm iteration, Ni's compile time by entry kind (`ni._stats()`), code bytes, break-even. Its code column moves in whole 4 KB stub chunks.
- **Where code goes:** `NI_CODESTATS=1 python3.7 setup.py build_ext --inplace`, then `python3.7 tools/codesize.py BENCH` with `-s` (what differs between blocks at one merge point), `-d` (identical copies; needs capstone, see BUILD.md), `-p` (promotion sites, through `tools/promostats.py`; `-j FILE` also writes JSON), `-D` (code after sites with several values).
- **Same code:** `python3.7 tools/samecode.py A B` (below).
- **Whole programs, against CPython:** `tools/adversarial/run.py` (below, and its docstring). Every call is timed, never best-of; 5 fresh processes per mode; sizes calibrated on CPython (`sizes.txt`); trained on one input, timed on another; outputs compared call by call; losses reported before means.

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

## Differential fuzzing (`tools/fuzz/`)

Random programs (BUILD.md, Fuzzing; what they cover is in `gen.py`'s docstring) run plain and with Ni, compared exactly. About 299,000 programs have run (about 240,000 on the checked build, 59,000 on a release build), each calling its functions over several rounds with arguments of varying types: 41 mismatches, all below. About 91% of the generated functions compile; the rest are generator functions, which Ni never compiles (Generators, above). Mismatches, by cause [run]:
- **B18** (fixed, `0b53f42`): `integer_cmp` folded an unsigned compare of a value with itself to false, so `s[len(s)]` returned `'\x00'`. Once, in the first 1000 seeds.
- **M12** (fixed, `99b81d7`): a constant index of 2**28 or more into a large array read the wrong address (a 2 GB list crashed). Once (seed 320), seen as `iencoding.h`'s range warning on stderr, which `run.py` counts.
- **G21** (fixed on baseline, `9d72c8c`): a known tuple times a constant of 2**31 or more items crashed, the generated `tuplerepeat` keeping the size in an `int`. Four times (three checked, one release), each a lambda made in a loop multiplying its tuple default by 2**31 or 2**32.
- **F117** (fixed, `979be8b`): a float folded from constants was one object for every run of its code, so `{NAN + 1.5 for ...}` had one NaN. Six times, always a NaN in a set or dict built in a loop.
- **C18 and C56** (known): a generator's `finally` printing when the generator is freed, which Ni does at return (CPython keeps the frame until the handler that caught the exception ends) and in another order: 29 in seeds 100000..129999. The generator no longer prints from finalisers.

Nothing else: no wrong arithmetic at the 2**31, 2**63 or float edges, no unpacking, slicing, closure, exception-handling or class-protocol difference. Not generated: frames, tracing and finaliser order (C11, C15, C18, C29, C56), threads, imports, I/O, subclasses of builtins, metaclasses, descriptors other than `property`, `async`.

## Memory per code buffer (`tools/memrepro.py`, C61, D129)

`tools/memrepro.py DIR N` compiles N distinct small functions and reports the RSS growth per code buffer; run under heaptrack with `PYTHONMALLOC=malloc`, its leaked allocations say where the memory goes. At 2000 functions (113,782 buffers, 232 bytes of code each), release build [run]:

| Per buffer | before (`cbd536d`) | after (`a7cb8e9`) |
|---|---|---|
| Live compiler states kept by pauses and promotion sites (vinfo blocks, PsycoObjects, arrays) | 1204 B | 0 (frozen: 170 B) |
| Snapshots at merge points (`fz` pseudo-code, `pyc` data) | 434 B | 432 B |
| Owner records and stub records (with the parked state in them, after) | 322 B | 411 B |
| Executable extents: code, then slack | 233 + 143 B | 232 + 142 B |
| CodeBufferObjects, proxies included | 200 B | 200 B |
| GC objects, sk blocks, owner buffer arrays | 177 B | 150 B |
| **RSS growth** | **2832 B** | **1940 B (−31%)** |

Parking cost no pyperformance benchmark more than 0.4% cold or warm, as none parks more than 4096 states (mdp 2667). A program that is all compiling, as the reproducer is, runs 5% more instructions, freezing. sympy's simplify tests peak at 1584 MB against 2682 MB (plainly 144 MB). What is left, largest first, with what it would take [inferred]: snapshots (a varint encoding of each `Source`, about 150 B, but in the hot compare path); slack in the executable extents (143 B); the parked state embedded in each record, though most records have none (a separate allocation, about 90 B); a CodeBufferObject per proxy; an arena for owner records (about 12 B each).

## Adversarial suite (`tools/adversarial/`)

Programs written blind to make Ni look bad, each aimed at one weakness, never tuned on Ni's results. `run.py` runs each in 5 fresh processes per mode at two run lengths, n calibrated on CPython (`sizes.txt`, rewritten only by `--calibrate`) for about 10 ms and 1 s of work per run (`--long` adds about 2 minutes). A process trains with 5 calls of `main(n, 0)` and times 15 of `main(n, 1)`, whose data and often types differ. It reports total process time, the first call, the median and worst timed call, spreads between and within processes, and each process's warm-up shape (flat, warm-up, slowdown, no steady state, by thirds after Barrett et al. 2017). Each call's result or exception and traceback, output, warnings, finaliser order (`finalisers`) and, with `--trace-midway`, trace events must match CPython's. The score leads with the count of losses, failures and wrong answers and the worst five; failures enter the geometric means as 10×.

Last full run at `a4bd69e`, by the earlier runner (best of 5 warm calls on one input, one length; release build, a busy machine, so ratios within about 10% are noise) [run]. Over 42 benchmarks, warm time is 0.75 of CPython's and cold time 0.95.
- **Wrong answers:** none: `builtin_subclasses`, `frames_logging` and `finalisers`, which differed, match CPython over 5 training and 5 timed calls at `N` [run].
- **Memory:** `classes_per_request` peaks at 569 MB against 11, and `reload_modules` at 379 against 15 (C14's remainder). Loading Ni costs about 9 MB of RSS in every process.
- **Compile before pay-off:** `import_heavy` is 4.4× slower cold, `many_functions` 13× on the first call and `short_script` 1.24× cold. Ni compiles code the first time it runs, module-level code included (D12), so this is compile speed.
- **Slower warm:** `reload_modules` 3.2×, `classes_per_request` 2.45×, `exec_templates` 1.45× (compiling fresh code each time), `operator_overloading` 1.27× (Python dunder methods), `lambdas_in_loops` 1.19×, `class_counter` 1.11× (a guard broken on every call), `genexpr_unfused` 1.09×.
- **Faster:** `bytecode_vm` 11×, `class_switch` 6.5×, `deep_recursion` 5×, `text_tokeniser` 3×, the megamorphic benchmarks about 2×.


## Sanitizers (`tools/sanitize.sh`)

Under AddressSanitizer and UBSan (BUILD.md, Sanitizers), the behaviour tests, the compile bounds, the smoke test and 370 of the 371 CPython test modules `--all` runs (not `test_threading`: an ASan fork hang, QUESTIONS 52) give no ASan report from Ni, with each vinfo, source_known and vci link a block of its own, free executable memory poisoned and `detect_stack_use_after_return` on. UBSan found two `memcpy`s from NULL of 0 bytes, which every compile made (`aed3df0`). LeakSanitizer found `psyco_compatible` dropping the differences of a rejected partial match, about 14 blocks a compile (`dfe1a39`, test `freeing.recompiled_loops_free`); its other reports at exit are memory kept for good or reached only through emitted code. Not covered: the emitted code itself, and anything the tests don't reach. [run]

## Other projects' test suites (`tools/thirdparty/`)

Twelve pure-Python projects' own suites, each at its last release for 3.7 (`tools/thirdparty/projects.py`, `lock.txt`), run plainly and with Ni for the whole process on a release build (BUILD.md). Last run at `edfcdcd`, both modes at once (`-j 2`) [run]:

| Project | Tests | Plain s | Ni s | Ni-only failures |
|---|---|---|---|---|
| attrs 23.2.0 | 1299 | 6.7 | 7.5 | 0 |
| more-itertools 9.1.0 | 572 | 1.3 | 1.7 | 1 (C60) |
| pyparsing 3.1.4 | 1717 | 20 | 23 | 13: 12 (C11 `f_locals`), 1 diagram |
| Jinja2 3.1.6 | 908 | 1.7 | 2.0 | 0 |
| MarkupSafe 2.1.5, pure | 53 | 0.2 | 0.4 | 0 |
| docutils 0.20.1 | 375 | 2.0 | 2.5 | 0 |
| networkx 2.6.3 | 4086 | 42 | 41 | 0 |
| sympy 1.10.1, 9 packages (run at `a7cb8e9`) | 4529 | 586 | 448 | 0 |
| python-dateutil 2.9.0.post0 | 2095 | 2.8 | 2.9 | 0 |
| toolz 0.12.1 | 180 | 0.5 | 0.8 | 0 |
| sortedcontainers 2.4.0 | 300 | 4.7 | 1.7 | 1 (C20) |
| Django 3.2.25, all, sqlite | 14684 | 276 | 359 | 2 (C11 `f_locals`) |

No test fails plainly and passes with Ni.
- **Wrong answers fixed:** code run by `exec` with its own locals mapping was compiled and its `from m import *` and `del x` used the globals (`8347596`; sympy's `lambdify` and `Sum.evalf`, 9 tests); the inline attribute store never made an untracked instance dict tracked when it stored a GC value of unknown type, so cycles through it were never collected (`edfcdcd`; sympy reached the 4 GB limit).
- **C11:** the line numbers, `f_back` through C and warning stack levels that failed pyparsing's parse actions (`_trim_arity` compares lines from `traceback.extract_stack`), and Django's and sympy's stack-level tests, are fixed (D119). Left: `f_locals` of compiled frames, read by pyparsing's `autoname_elements` (6 classes × 2 tests) and by Django's debug page and a test's `inspect.currentframe().f_back.f_locals`; pyparsing's railroad diagram for `OneOrMore(stopOn=)` differs.
- **Memory (C61):** the sympy run aborted at the 4 GB limit after about 630,000 code buffers (3 GB of RSS) until compiler states were parked; it now completes with 1,036,438 ("Memory per code buffer" above).
- **Time:** pyparsing takes 23 s with Ni, 20 s plainly. Django, many short tests over much code, is 1.3× slower; sympy is 1.4× faster and sortedcontainers 2.8×.
