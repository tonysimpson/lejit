# Open questions

Unknowns found during indexing (2026-11-16). When one is answered, move the answer into the relevant doc (or `DECISIONS.md`) and delete it here.

## For the owner (intent and history)
Questions 1–6 were answered on 2026-11-16: see `DECISIONS.md` D10–D15.

## Design questions for the meta-compiler
(Q11 and Q24, on the target version, were answered by D17.)
7. **Per-slot meta signature:** for each type slot, which arguments are `vinfo_t*` (v) and which raw compile-time values (l)? It exists only implicitly at call sites today (PATTERNS P09). Where should this table live, and can it be derived?
8. **`Cf*` flags per CPython function:** how much can be derived from headers, docs or return types, and how much needs annotation (`CfPure` especially)?
9. **Restart safety** (PATTERNS rules §1): can a generator prove it, or must translated code be structured (for example "all promotions before any effects")?
10. **Virtual-object choices** (P18): annotation format? (Which constructors to virtualise, field layout, nested weights.)
26. **First meta-compiler target: virtual small integers on 3.7?** (owner's idea, 2026-11-16.) Generate the compile-time form of `Objects/longobject.c:long_add`/`long_sub`/`long_richcompare` from their one-digit fast paths (`MEDIUM_VALUE`): a virtual int held as a machine word, overflow check, fallback to the residual call (D13). Measurable (integer benchmarks are at about 2× now, all residual calls) and on a baseline that 50 CPython test modules check. 3.12's `_PyLong_Add` has the same shape (`_PyLong_IsCompact`), so it should carry over to D17. To settle: does this replace D19's stage order (descriptors first)? [inferred]

## Decoupling the core (D16)
21. **Host data structures:** the core keeps snapshots, the meta table and promotion caches in CPython lists, dicts and capsules (`dispatcher.c`, `mergepoints.c`, `codemanager.c`). Should it replace them with its own C structures?
22. **Where `PsycoObject.pr` and the `LOC_*` slots go:** today the core state embeds Python frame state (`Python/pycheader.h`). What is the language-neutral interface: an opaque per-language payload plus callbacks for "compare", "snapshot" and "unwind"?
23. **Refcounting and object layout in the backend** (`x64/ipyencoding.h`): these should become generated "runtime-model" operations that the backend only emits. What is the minimal set (incref, decref plus dealloc call, type load, ...)?
25. **Psyco runtime helpers in translated files** (`psyfunc.c` has ~100 CPython API uses): which parts are core calling machinery and which are translated `call_function`/`_PyFunction_FastCall` logic?

## Correctness questions found by reading (unverified: no build)
12. Is mixed-type arithmetic (int + float) a compile-time TypeError? `pabstract.c:binary_op1` depends on Py2's `Py_TPFLAGS_CHECKTYPES`. Does any caller avoid it?
13. `PsycoObject_RichCompare`: can a user `__eq__` run twice (residual slot returns NotImplemented, then residual `PyObject_RichCompare`)?
14. How are compile-time doubles stored in `source_known_t.value`? This decides the fix for `pfloatobject.c:direct_compute_float` (numeric conversion vs bit-cast).
15. `pfuncobject.c:compute_function`: `SET_FIELD` uses `||` instead of `&&`, and stores without `FMUT|FPYREF`. Is that a refcount bug?
16. `run_time_result_check` pushes RAX without updating `stack_depth`. Is that the cause of known alignment problems (see `compare_return_address_in_a_stupid_way`)?
17. Is the ±2 GB proximity of code buffers guaranteed anywhere?
18. R12 is left out of `RegistersLoop`. Deliberate?

## Housekeeping
19. Should `tools/` scripts be fixed (stale `../c/` paths, Py2 `test_c_encoding`) or removed?
20. `MANIFEST` lists upstream paths. Delete or regenerate?
