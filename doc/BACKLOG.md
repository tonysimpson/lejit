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
- **C24:** compiled code never drops the GIL or runs signal handlers: another thread waiting on a compiled busy loop starves, Ctrl-C can't stop a compiled loop, and a handler's exception surfaces outside its `try`. Found by the CPython test survey (2026-11-17); `test_signal`, `test_threading`, `test_io`, `test_unittest`, `test_sys`, `test_asyncio` excluded for it. Medium (core: an `eval_breaker` test at loop heads). The most serious open item.
- **C23 (partly fixed, `5f1dbf6`; calls from compiled code remain):** `sys.settrace`/`setprofile` don't see compiled code; a debugger's jump out of a loop never happens (`test_sys_settrace` ran to 20 GB). Small fix (interpret while tracing), but it is a decision (QUESTIONS 32; `tools/interpreted.py` relies on the current behaviour).
- **F10:** a user `__eq__`/`__ne__` runs twice (now seen: CPython's `test_compare`). Small to medium (`pobject.c`).
- **F48:** an exhausted sequence iterator restarts when the sequence grows (`piterobject.c`). Medium.
- **F49, F50:** builtins read from `globals()['__builtins__']` at each lookup; a slot deleted after compiling (`del C.__next__`) not seen. Small to medium; rare.
- **C25:** C functions called directly skip `_Py_CheckFunctionResult` (a buggy extension returning NULL without an exception segfaults). Small.
- **F51:** checked build only: a respawn replay aborts on a forced constant tuple (`test_subclassinit`). Small (the replay check).
- **F9:** a `T_STRING_INPLACE` struct member gives `bytes`, not `str`. Small. **In flight** (strings agent).
- **C9:** reference counts are lower than ceval's, so C code testing `Py_REFCNT(x) == 1` can behave differently. Fixed where seen; others may exist.
- **C11, C18:** compiled frames are only partly visible (`warnings` stacklevel, `f_lineno`, `f_back`), and a traceback doesn't keep a compiled function's locals alive. C11 now reaches more code: with `with` and class statements compiling, unittest's `case.run` is compiled, so every test method is a compiled callee (`type()` takes `__module__` from `unittest.case`; CPython's `test_builtin`). Worth doing next among the frame bugs: the hook behind `PyEval_GetFrame` (`globals()`, `type()`'s `__module__`), then `f_lineno`.
- **C15 remainder:** `locals()`, `eval()` and friends reached indirectly (`f = locals; f()`) see only the arguments.
- **C17 remainder:** identity of virtual floats, ints and bound methods across calls (`f(x) is x` is False).

- **F37:** fixed (`e86f8ae`, STORES.md option 1).
- **C22, F41–F47:** fixed by the CPython test survey (2026-11-17; KNOWN_BUGS), including two crashes (C22, F45).

**Possible, not shown to happen**
- **F11:** a C-NULL `kw` on the `use_proxy` path. From reading; needs a test first.
- **F18:** exception details (a bare `raise` adds a traceback entry; dead Py2 frame-exception code).

**Latent or by design**
- **F12, F35, M10, M11, M7:** documented, harmless today.
- **F27 remainder:** running out of memory in the middle of a compile aborts the process on purpose ("psyco: out of memory"), as upstream; the merge-point analysis and frame registration now fail cleanly. Raising MemoryError instead would mean unwinding a half-built compilation. Medium.
- **F36:** holding a class's shared keys stops CPython re-sharing them (go's `Board`). Parked: the fixes tried cost more than the bug (KNOWN_BUGS F36).
- **C3/C14:** compiled code, promotion caches and the stub pool are rarely freed. Matters for long-running programs that keep creating code.
- **C19, C20:** `getrefcount` one lower; one GC object per compile.
- **B10:** code memory is RWX.
- **C1:** the stack-alignment test at merge points is needed but badly named, and it causes some duplicate blocks (see ideas).

## Improvement ideas

### Speed
- **F13, FASTCALL builtins.** **In flight.** Note: F23 already gave `PsycoCFunction_Call` a FASTCALL path for builtins with a meta; F13 is about the rest, and the agent should say what is left.
- **Inline recursion-depth count (C12).** **In flight.** About 2.6% of richards.
- **Attribute stores** (`doc/STORES.md`, QUESTIONS 39). Options 1 and 2 done (`e86f8ae`, D39): richards 13%, go 8% fewer instructions. Before them, stores were 30–38% of richards, 13% of deltablue, 11% of go. Next: an inline store, estimated at a further 13–14% of richards. Option 4 (a per-site check of the live keys, no reference held) works in constructors and avoids F36. The dict version tag must be settled first.
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
- **Shared code after polymorphic promotion sites** (option D, queued after forgotten values). Once a promotion site has seen K values, compile the code after it once with the value unknown, and make only the operation itself value-dependent (through a cache, as JavaScript inline caches do: they cache how to do the operation, not the rest of the function). Psyco today compiles a copy of the rest of the block per value. Builds on the forgotten-values mark. Design write-up first. A shared megamorphic table keyed on (type, operation), as in V8, is the follow-up if megamorphic sites still show in profiles.
- **Forgotten values**: done (D43, CODE_SIZE.md "Forgotten values"); neutral on pyperformance, where the cap meets almost no varying constants. Left open: forgetting a forgotten value's type too (SPECIALISATION rule 3; needs a generic path for `Psyco_NeedType`), and marking the values that partial matches un-promote before the cap.
- **A cap on blocks per merge point** (CODE_SIZE option 3): the general backstop. Next, now D33 is in.
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
