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
- **C29:** a frame already running compiled when tracing starts gets no events: `pdb.set_trace()`/`breakpoint()` in a compiled function stops only in its callees, a tracer that sets `f_trace` on running frames sees nothing from them, and a profiler misses their `c_call`/`return` events. Needs a way out of compiled code mid-frame (large).
- **F9:** a `T_STRING_INPLACE` struct member gives `bytes`, not `str`. Small. **In flight** (strings agent).
- **C9:** reference counts are lower than ceval's, so C code testing `Py_REFCNT(x) == 1` can behave differently. Fixed where seen; others may exist.
- **C11, C18:** compiled frames are only partly visible (`warnings` stacklevel, `f_lineno`, `f_back`), and a traceback doesn't keep a compiled function's locals alive. C11 now reaches more code: with `with` and class statements compiling, unittest's `case.run` is compiled, so every test method is a compiled callee (`type()` takes `__module__` from `unittest.case`; CPython's `test_builtin`). Worth doing next among the frame bugs: the hook behind `PyEval_GetFrame` (`globals()`, `type()`'s `__module__`), then `f_lineno`.
- **C15 remainder:** `locals()`, `eval()` and friends reached indirectly (`f = locals; f()`) see only the arguments.
- **C17 remainder:** identity of virtual floats, ints and bound methods across calls (`f(x) is x` is False).

- **F37:** fixed (`e86f8ae`, STORES.md option 1).
- **F56, F57, F80:** fixed (`d16ab6b`..`8e32559`; KNOWN_BUGS, D54): every slot compiled code folds for a heap type (truth, number, subscript, `len`, `in`, call, `__index__`, operand order after `__bases__`) and a function's `__code__` are guarded; `%=`'s message. F57's guard costs up to 1.2% (below).
- **F10, F48, F50, F52, C25:** fixed (`0e191a3`..`9217bdb`; KNOWN_BUGS): compare slots run once, exhausted sequence iterators stay exhausted, a heap type's compare, hash and iteration slots and a function's defaults are guarded, C function results are checked. Rest of F50 is F56.
- **C30, C31, C39:** fixed (`63b4f95`, `5a39986`, `6192ed6`; KNOWN_BUGS): an inlined frame names its caller by a serial as well as its code object (a dead caller's address was reused at a join point), the call alignment test compares only the return-address slot, and the async-exception case waits for its target.
- **C24:** fixed (D45): compiled code tests `eval_breaker` at loop heads and after calls (since D53, only calls that may block).
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
- **F57** (fixed in `eca1509`, at a cost): the `__code__` guard is a load and a compare at every call of a promoted function, +0.3% to +1.2% instructions per warm iteration (hexiom +1.2%, richards +0.9%). Assigning `__code__` to a function already called from compiled code is rare (code reloaders, some test tools). Proposal: the owner chooses between keeping it and accepting F57 (drop `eca1509`, which is self-contained).

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

- **Fewer `eval_breaker` tests (D45):** done (`889fce6`, D53): after a call only if it made a C call that may block; code 2,381 → 2,322 KB over the 29, half of D45's cost. Left: `type_call` (668 of the 2,122 call compiles that keep a test in the seven benchmarks) could skip it for a class whose `__new__` and `__init__` are Python code, but only behind a guard, since `__init__` can be reassigned after the compile (CODE_SIZE.md, "Fewer eval_breaker tests after calls").

### Speed
- **F13, FASTCALL builtins:** done (`17b2a04`, D36): FASTCALL builtins are called directly (`cimpl_fastN`).
- **Inline recursion-depth count (C12):** done (`1de8b28`): richards −6% instructions, deltablue −4%.
- **Attribute stores** (`doc/STORES.md`, QUESTIONS 39). Done: options 1 and 2 (`e86f8ae`, D39), option 4 (`80a60ce`, D38) and its sites shrunk (`4398496`..`2606291`, D47): richards +10% code over the baseline, 161 M instructions against base's 227 M. Follow-ups: (a) **checks shared between consecutive stores to one object** (STORES.md, "Step 2"; about 3% of richards' code; needs a guard per run of names, and `__del__` re-entry that a replay decides the same; tests already in `stores.py`); (b) **present-name loads on the shared cache** (STORES.md, "Loads (D31)"), measured separately as D38 asks; (c) GC tracking inline (a conditional call to `PyObject_GC_Track` instead of a miss); (d) **one dealloc glue for every decref** (`decref_dealloc_calling` inlines its call at each site, as the store did before `4398496`). F55 (a promoted type kept after a call assigns `__class__`) is open.
- **Strings (D14).** Done: `len`, indexing, constant `in` and `==` (`9abd6a3`, `f1612c5`, D37); slices of str, bytes and list through a virtual slice (`b251646`, D56: pyflate −12% instructions). Measured and not worth it on this set (PROFILE.md, "Slices, join and %"): `str.join` (already a direct `PyUnicode_Join`), `%` with a constant format (at most 0.3% of deltablue), concatenation buffers. Left if a string-heavy benchmark appears: tuple slices and steps, run-time `in`, iteration, virtual characters (QUESTIONS 40).
- **Unshared-dict loads with a known hash.** Done (`0965d06`). Next step, if go still shows `lookdict`: an inline probe of a combined dict for a constant key (medium).
- **Opcode coverage:** done for functions (`353dc79`, `41542aa`, `93b201f`): `with`, `a, *b = x`, class statements, `del` of a cell and `@` compile, and no stdlib function outside `test/` and `idlelib` is held back by an opcode (was 408 of 14,269). Still interpreted: generators other than generator expressions, class bodies (LOAD_NAME/STORE_NAME, and LOAD_CLASSDEREF with them), module code, and functions calling `locals()`/`eval()` and friends (C15). `with` is a residual call per `__enter__`/`__exit__`; nothing measured uses it in a hot loop.
- **Calls with `*args`/`**kw` and unpacking** (F19 remainder): done (`7fdf9a7`). Every BUILD_*_UNPACK opcode compiles; the map and set forms are residual helpers. No pyperformance benchmark used them. If one ever matters: a virtual dict for `**` of known dicts.
- **Calls to closures** (F8): compile, but the call from compiled code is the generic one. **In flight.**
- **Generators, option A** (GENERATORS.md): compiled resumable bodies. Deferred to the 3.12 entry design (QUESTIONS 33).
- **Inlining:** measured and not taken (STATUS, "Inlining measured"): +1–2% speed for +5–10% code.
- **Register calling convention** between compiled functions: unmeasured (QUESTIONS 35 asks for the call-sequence share first).
- **M9:** negative constants in qword stores go through `MOV R10, imm64`; a sign-extended imm32 would do. Tiny.

### Code size and compile time
- **The specialisation policy** (`doc/SPECIALISATION.md`) orders the items below that change how many copies are made: forgotten values, rejoining, matching across an unused fact, shared code after polymorphic sites, a megamorphic table, one budget per function.
- **Shared code after polymorphic promotion sites** (option D; design and measurements in `doc/OPTION_D.md`, QUESTIONS 43–46). Once a promotion site has seen K values, compile the code after it once with the value forgotten, with only the operation depending on it through a cache, as JavaScript inline caches do. Measured upper bound on pyperformance, all 23 benchmarks: K = 2 saves 18.1 KB (0.75%; richards 5.1, deepcopy 4.7, deltablue 2.8) but slows every two-value site for nothing; K = 3 saves 5.6 KB (0.2%); only with foreknowledge (K = 1) is it 85 KB (3.5%). The copies it would remove run often (richards, estimated: +5–8% time at K = 2, +1.6–2.5% at K = 3). Recommended: not on 3.7 (owner, QUESTIONS 43). If built, step 1 is a cached operation in the megamorphic version of attribute and method sites with its result forgotten: no change on pyperformance, and it removes the re-promotion downstream of megamorphic versions (pkg_resources import: 70 sites, 13.6 KB). A shared megamorphic table keyed on (type, operation), as in V8, is the follow-up if megamorphic sites still show in profiles.
- **Forgotten values**: done (D43, CODE_SIZE.md "Forgotten values"); neutral on pyperformance, where the cap meets almost no varying constants. Left open: forgetting a forgotten value's type too (SPECIALISATION rule 3; needs a generic path for `Psyco_NeedType`), and marking the values that partial matches un-promote before the cap.
- **A cap on blocks per merge point** (CODE_SIZE option 3): done (`932f416`, D40), 6 blocks.
- **Unused facts at merge points** (D50, D57): done for every known field of a run-time value (`96b9777`, `0b808fa`), −1.0% instructions compiled, speed unchanged; C35 was `IS_A_JUMP` (`2063657`). Left: promotions at a merge point, whose own compare must keep the fact.
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
