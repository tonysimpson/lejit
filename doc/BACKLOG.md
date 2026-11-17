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
- **F27:** failing an allocation early in a first compile segfaults (found with `_testcapi.set_nomemory`). Where hasn't been looked at. Small to medium.
- **F9:** a `T_STRING_INPLACE` struct member gives `bytes`, not `str`. Small. **In flight** (strings agent).
- **F6:** `direct_compute_float` converts a float's bits numerically; wrong when a frame is rebuilt from compiled state. Small; needs a test that rebuilds a frame.
- **C9:** reference counts are lower than ceval's, so C code testing `Py_REFCNT(x) == 1` can behave differently. Fixed where seen; others may exist.
- **C11, C18:** compiled frames are only partly visible (`warnings` stacklevel, `f_lineno`, `f_back`), and a traceback doesn't keep a compiled function's locals alive.
- **C15 remainder:** `locals()`, `eval()` and friends reached indirectly (`f = locals; f()`) see only the arguments.
- **C17 remainder:** identity of virtual floats, ints and bound methods across calls (`f(x) is x` is False).

- **F37:** a `__setattr__` added to a class after a store was compiled is ignored (confirmed). Fixed by any store option except doing nothing (STORES.md option 1). Small.

**Possible, not shown to happen**
- **F10:** a user `__eq__` may run twice. **F11:** a C-NULL `kw` on the `use_proxy` path. Both from reading; each needs a test first.
- **F18:** exception details (a bare `raise` adds a traceback entry; dead Py2 frame-exception code).

**Latent or by design**
- **F12, F35, M10, M11, M7:** documented, harmless today.
- **F36:** holding a class's shared keys stops CPython re-sharing them (go's `Board`). Parked: the fixes tried cost more than the bug (KNOWN_BUGS F36).
- **C3/C14:** compiled code, promotion caches and the stub pool are rarely freed. Matters for long-running programs that keep creating code.
- **C19, C20:** `getrefcount` one lower; one GC object per compile.
- **B10:** code memory is RWX.
- **C1:** the stack-alignment test at merge points is needed but badly named, and it causes some duplicate blocks (see ideas).

## Improvement ideas

### Speed
- **F13, FASTCALL builtins.** **In flight.** Note: F23 already gave `PsycoCFunction_Call` a FASTCALL path for builtins with a meta; F13 is about the rest, and the agent should say what is left.
- **Inline recursion-depth count (C12).** **In flight.** About 2.6% of richards.
- **Attribute stores** (`doc/STORES.md`, QUESTIONS 39; owner decides). Stores are 30–38% of richards, 13% of deltablue, 11% of go. Option 1 (type guard, then `_PyObjectDict_SetItem` directly; fixes F37) gains about 8% on richards; an inline store gains about 21–22%. Option 4 (a per-site check of the live keys, no reference held) works in constructors and avoids F36. The dict version tag must be settled first.
- **Strings (D14).** **In flight:** measure, then `len`, indexing, iteration, `in`, constant folding. Later: concatenation buffers (old Psyco's design) if the measurement says so.
- **Unshared-dict loads with a known hash.** Done (`0965d06`). Next step, if go still shows `lookdict`: an inline probe of a combined dict for a constant key (medium).
- **Calls with `*args`/`**kw` and unpacking** (F19 remainder): `BUILD_TUPLE_UNPACK_WITH_CALL`, `BUILD_MAP_UNPACK_WITH_CALL` aren't compiled, so the whole function stays interpreted.
- **Calls to closures** (F8): compile, but the call from compiled code is the generic one.
- **Generators, option A** (GENERATORS.md): compiled resumable bodies. Deferred to the 3.12 entry design (QUESTIONS 33).
- **Inlining:** measured and not taken (STATUS, "Inlining measured"): +1–2% speed for +5–10% code.
- **Register calling convention** between compiled functions: unmeasured (QUESTIONS 35 asks for the call-sequence share first).
- **M9:** negative constants in qword stores go through `MOV R10, imm64`; a sign-extended imm32 would do. Tiny.

### Code size and compile time
- **A cap on blocks per merge point** (CODE_SIZE option 3): the general backstop. Next, now D33 is in.
- **Promotion code** is the largest remaining share (about a third before D33). Not analysed yet: which values, how many distinct each, which sites should go megamorphic sooner. Analysis first, then a fix.
- **Other two-form values at merge points** (QUESTIONS 38): ints outside -5..256, floats, stack slots; deltablue's +1.3% compiles after D33 is unexplained.
- **Type-set guards** (CODE_SIZE option 4): one copy of a base-class method for several subclasses (richards' `Task`). Research.
- **Promotion stubs** still use `psyco_call_code_builder` (about 70 bytes each); the D32 stubs could serve them too, but the promotion path passes a run-time value (`extraarg`), which the trampoline would have to carry.
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
