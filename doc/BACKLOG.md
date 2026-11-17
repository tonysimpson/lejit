---
written: 2026-11-17
head_ref: 2966391
status: index; the detail lives in the docs each line points to
---
# Backlog: open bugs and improvement ideas

One list of what is still open, swept from every doc on 2026-11-17. Each line points to where the detail is; update that doc first, then this list. Sizes are rough guesses in AI elapsed time. **In flight** means an agent branch exists (not merged yet).

## Open bugs

From `KNOWN_BUGS.md`, ordered by what they can do to a user's program.

**Wrong results or crashes, reachable today**
- **C30, C31:** CPython's `test_timeit` (since `e26c2a3`) and `test_shutil` segfault under Ni after a run of earlier tests; not reduced. Reduce first (`tools/nianalyser/crash_report.py`), then fix.
- **C29:** a frame already running compiled when tracing starts gets no events: `pdb.set_trace()`/`breakpoint()` in a compiled function stops only in its callees, a tracer that sets `f_trace` on running frames sees nothing from them, and a profiler misses their `c_call`/`return` events. Needs a way out of compiled code mid-frame (large).
- **F56:** a heap type's other slots are still folded (a `__bool__` or `__len__` added later, the number slots, `__bases__`); the rest of F50. `Psyco_HeapSlot` covers them; IsTrue's fold is worth keeping. Small to medium; rare.
- **F57:** a function's `__code__` changed after a compiled call to it isn't seen. Small (F52's shape).
- **F9:** a `T_STRING_INPLACE` struct member gives `bytes`, not `str`. Small. **In flight** (strings agent).
- **C9:** reference counts are lower than ceval's, so C code testing `Py_REFCNT(x) == 1` can behave differently. Fixed where seen; others may exist.
- **C11, C18:** compiled frames are only partly visible (`warnings` stacklevel, `f_lineno`, `f_back`), and a traceback doesn't keep a compiled function's locals alive. C11 now reaches more code: with `with` and class statements compiling, unittest's `case.run` is compiled, so every test method is a compiled callee (`type()` takes `__module__` from `unittest.case`; CPython's `test_builtin`). Worth doing next among the frame bugs: the hook behind `PyEval_GetFrame` (`globals()`, `type()`'s `__module__`), then `f_lineno`.
- **C15 remainder:** `locals()`, `eval()` and friends reached indirectly (`f = locals; f()`) see only the arguments.
- **C17 remainder:** identity of virtual floats, ints and bound methods across calls (`f(x) is x` is False).

- **F37:** fixed (`e86f8ae`, STORES.md option 1).
- **F10, F48, F50, F52, C25:** fixed (`0e191a3`..`9217bdb`; KNOWN_BUGS): compare slots run once, exhausted sequence iterators stay exhausted, a heap type's compare, hash and iteration slots and a function's defaults are guarded, C function results are checked. Rest of F50 is F56.
- **C24:** fixed (D45): compiled code tests `eval_breaker` at loop heads and after calls.
- **C22, F41–F47:** fixed by the CPython test survey (2026-11-17; KNOWN_BUGS), including two crashes (C22, F45).

**Left for the owner: accept, or move to the 3.12 backlog** (point-2 sweep of GOAL.md, 2026-11-17; F49, F51/F35 and F18 (1) fixed then. Proposals only; not decided.)
- **C9** (reference counts lower than ceval's; accepted in `test_importlib`: marshal writes no FLAG_REF for a code object with refcount 1, so `.pyc` bytes differ, loads equal). Fixed where seen; a general fix owns a reference per stack slot, a cost on every push. Proposal: accept for 3.7, look again in 3.12.
- **C14** (compiled code keeps classes, bound methods and module dicts alive; `test_gc`, `test_module`, `test_weakref`). Needs code freeing. Proposal: move to 3.12 (the entry design decides what owns compiled code).
- **C15 remainder** (`f = locals; f()` sees only the arguments). Needs the locals in the frame, as C11 does. Proposal: move to 3.12 with C11.
- **C17 remainder** (`f(x) is x` False for virtual ints, floats, bound methods). The language promises no identity for new numbers; forcing them costs the main speed-up. Proposal: accept.
- **C18** (a traceback doesn't keep a compiled function's locals alive; `test_exceptions`). Same root as C11 (locals not in the frame). Proposal: move to 3.12 with C11.
- **C19, C20** (`getrefcount` one lower; one GC object per compile; `test_generators`, `test_ctypes`, `test_socket`, `test_descr`, `test_tracemalloc`). By design. Proposal: accept.
- **F72** (new): a module's `__builtins__` replaced for good after a call into it from another module compiled isn't seen at that call site. Exact needs a guard per cross-module call. Proposal: accept.
- **F27 remainder** (running out of memory mid-compile aborts on purpose): seen 2026-11-17 in `test_queue` (`--all`) when the run's address-space limit was 4.0e9 bytes instead of `MEMORY_LIMIT`'s 4 GiB: 100 thread stacks leave no room and a compile allocation fails (`vcompiler.h` OUT_OF_MEMORY). `baseline-3.7` the same. Proposal: move to 3.12 (unwinding a half-built compile), and keep the test limit at 4 GiB.
- **C39** (new): `eval_breaker.async_exc_from_thread` is flaky on `baseline-3.7` too (timing). Proposal: make the case robust to a late async exception, or mark it; owner of the C30/C31 work, since it touches the same paths.

**Possible, not shown to happen**
- **F11:** dormant: the function has no caller.
- **F18 (3):** dead Py2 frame-exception code (the bare-`raise` traceback entry is fixed, `fafd71e`).

**Latent or by design**
- **F12, F35, M10, M11, M7:** documented, harmless today.
- **F27 remainder:** running out of memory in the middle of a compile aborts the process on purpose ("psyco: out of memory"), as upstream; the merge-point analysis and frame registration now fail cleanly. Raising MemoryError instead would mean unwinding a half-built compilation. Medium.
- **F36:** holding a class's shared keys stops CPython re-sharing them (go's `Board`). Parked: the fixes tried cost more than the bug (KNOWN_BUGS F36).
- **C3/C14:** compiled code, promotion caches and the stub pool are rarely freed. Matters for long-running programs that keep creating code.
- **C19, C20:** `getrefcount` one lower; one GC object per compile.
- **B10:** code memory is RWX.
- **C1:** the stack-alignment test at merge points is needed but badly named, and it causes some duplicate blocks (see ideas).

## Improvement ideas

- **Fewer `eval_breaker` tests (D45):** they add 5% to code, [inferred] mostly after calls, which outnumber loop heads. A call that compiles to no residual call (folded, or a virtual such as `range()`) needs no test after it. Small; measure code size and instructions.

### Speed
- **F13, FASTCALL builtins.** **In flight.** Note: F23 already gave `PsycoCFunction_Call` a FASTCALL path for builtins with a meta; F13 is about the rest, and the agent should say what is left.
- **Inline recursion-depth count (C12).** **In flight.** About 2.6% of richards.
- **Attribute stores** (`doc/STORES.md`, QUESTIONS 39). Done: options 1 and 2 (`e86f8ae`, D39), option 4 (`80a60ce`, D38) and its sites shrunk (`4398496`..`2606291`, D47): richards +10% code over the baseline, 161 M instructions against base's 227 M. Follow-ups: (a) **checks shared between consecutive stores to one object** (STORES.md, "Step 2"; about 3% of richards' code; needs a guard per run of names, and `__del__` re-entry that a replay decides the same; tests already in `stores.py`); (b) **present-name loads on the shared cache** (STORES.md, "Loads (D31)"), measured separately as D38 asks; (c) GC tracking inline (a conditional call to `PyObject_GC_Track` instead of a miss); (d) **one dealloc glue for every decref** (`decref_dealloc_calling` inlines its call at each site, as the store did before `4398496`). F55 (a promoted type kept after a call assigns `__class__`) is open.
- **Strings (D14).** **In flight:** measure, then `len`, indexing, iteration, `in`, constant folding. Later: concatenation buffers (old Psyco's design) if the measurement says so.
- **Unshared-dict loads with a known hash.** Done (`0965d06`). Next step, if go still shows `lookdict`: an inline probe of a combined dict for a constant key (medium).
- **Opcode coverage:** done for functions (`353dc79`, `41542aa`, `93b201f`): `with`, `a, *b = x`, class statements, `del` of a cell and `@` compile, and no stdlib function outside `test/` and `idlelib` is held back by an opcode (was 408 of 14,269). Still interpreted: generators other than generator expressions, class bodies (LOAD_NAME/STORE_NAME, and LOAD_CLASSDEREF with them), module code, and functions calling `locals()`/`eval()` and friends (C15). `with` is a residual call per `__enter__`/`__exit__`; nothing measured uses it in a hot loop.
- **Calls with `*args`/`**kw` and unpacking** (F19 remainder): done (`7fdf9a7`). Every BUILD_*_UNPACK opcode compiles; the map and set forms are residual helpers. No pyperformance benchmark used them. If one ever matters: a virtual dict for `**` of known dicts.
- **Calls to closures** (F8): compile, but the call from compiled code is the generic one.
- **Generators, option A** (GENERATORS.md): compiled resumable bodies. Deferred to the 3.12 entry design (QUESTIONS 33).
- **Inlining:** measured and not taken (STATUS, "Inlining measured"): +1–2% speed for +5–10% code.
- **Register calling convention** between compiled functions: unmeasured (QUESTIONS 35 asks for the call-sequence share first).
- **M9:** negative constants in qword stores go through `MOV R10, imm64`; a sign-extended imm32 would do. Tiny.

### Code size and compile time
- **The specialisation policy** (`doc/SPECIALISATION.md`) orders the items below that change how many copies are made: forgotten values, rejoining, matching across an unused fact, shared code after polymorphic sites, a megamorphic table, one budget per function.
- **Shared code after polymorphic promotion sites** (option D; design and measurements in `doc/OPTION_D.md`, QUESTIONS 43–46). Once a promotion site has seen K values, compile the code after it once with the value forgotten, with only the operation depending on it through a cache, as JavaScript inline caches do. Measured upper bound on pyperformance, all 23 benchmarks: K = 2 saves 18.1 KB (0.75%; richards 5.1, deepcopy 4.7, deltablue 2.8) but slows every two-value site for nothing; K = 3 saves 5.6 KB (0.2%); only with foreknowledge (K = 1) is it 85 KB (3.5%). The copies it would remove run often (richards, estimated: +5–8% time at K = 2, +1.6–2.5% at K = 3). Recommended: not on 3.7 (owner, QUESTIONS 43). If built, step 1 is a cached operation in the megamorphic version of attribute and method sites with its result forgotten: no change on pyperformance, and it removes the re-promotion downstream of megamorphic versions (pkg_resources import: 70 sites, 13.6 KB). A shared megamorphic table keyed on (type, operation), as in V8, is the follow-up if megamorphic sites still show in profiles.
- **Forgotten values**: done (D43, CODE_SIZE.md "Forgotten values"); neutral on pyperformance, where the cap meets almost no varying constants. Left open: forgetting a forgotten value's type too (SPECIALISATION rule 3; needs a generic path for `Psyco_NeedType`), and marking the values that partial matches un-promote before the cap.
- **A cap on blocks per merge point** (CODE_SIZE option 3): the general backstop. Next, now D33 is in.
- **Unused facts at merge points** (D50): done for types (`96b9777`), −1.0% instructions compiled, speed unchanged. Left: other known fields of run-time values (blocked on KNOWN_BUGS C35), and promotions at a merge point, whose own compare must keep the fact.
- **Respawned branches rejoin** (D44, D49): done (`f229d14`) for split sites; their own restrictions lifted (`6f2c227`), measured neutral. Left: a small case for C27 (only `test_enum` reproduces it); respawn points that aren't split sites (error paths, overflow); measure how much they copy first. Small.
- **Promotion code** (CODE_SIZE.md, "Promotions"): 61–96% of code is compiled after a promotion, and promotions are 57–88% of compile time. 94% of what is promoted is types, and 79–100% of sites see one value, so cut-offs and promoting fewer kinds gain little. The copies come from many sites at one instruction, on paths entered from pauses and respawns that don't match at merge points: option 2 there (matching across promoted knowledge), research, after the cap. The polymorphic inline cache (option 3) is done (`ff1bce8`, D42): misses now equal the values compiled. Left open: why chain placement moves richards' cycles and branch misses (CODE_SIZE.md, "Polymorphic inline cache"), and whether sites with no cut-off should get one (option 4); a full chain there replaces its last case.
- **Other two-form values at merge points** (QUESTIONS 38): ints outside -5..256, floats, stack slots; deltablue's +1.3% compiles after D33 is unexplained.
- **Type-set guards** (CODE_SIZE option 4): one copy of a base-class method for several subclasses (richards' `Task`). Research.
- **The alignment test at merge points** (C1) rejects matches that differ only in stack alignment mod 16. Measuring how many blocks it causes is cheap; removing it means aligning at calls instead.
- **Merge-point lookup is a linear scan**, and meta lookup allocates a `PyLong` key per lookup (ARCHITECTURE, design observations). Compile-time costs; not measured.
- **Compile-time C calls through libffi** call `ffi_prep_cif` every time (QUESTIONS 36).
- **Instruction-cache effects:** unmeasured (COMPILE_COST); `perf stat` icache events would show whether smaller code now pays off in speed.

### Tooling and tests
- **Encoder from `@asm` annotations** (QUESTIONS 37): owner wants it after compile-time metrics, which now exist.
- **`v_mem_mov.h` annotations, a lint for unannotated emission, and pointbreak checks of emitted code** (BUILD, "Design notes").
- **Stale tools** (QUESTIONS 19) and `MANIFEST` (QUESTIONS 20).
- **A quiet re-baseline** of pyperformance and compile cost once the in-flight branches are merged: timings since the morning of 2026-11-17 were taken with agents running.

### Research (the project goal)
- **Meta-compiler** (METACOMPILER.md, GOAL.md): next slices after ints, floats and the bytecode slice; the stage-order question (QUESTIONS 26); per-slot signatures, `Cf*` flags, restart safety, virtual-object annotations (QUESTIONS 7–10). Today's hand-written work (shared keys, D31–D33, strings) adds to what the generator must reproduce: worth recording each as a pattern in PATTERNS.md.
- **Decoupling the core** (QUESTIONS 21–25, D16).
- **3.12 port** (PORT312.md; QUESTIONS 29–33).
