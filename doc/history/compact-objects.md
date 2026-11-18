---
written: 2026-11-17
status: research for restoring compact objects (A11); nothing restored yet
---
# Compact objects: how upstream's worked and what broke in V2

Sources: `../psyco-history` (mail archive, the hg mirror, wayback material) and upstream's code at `9bd5834:c/Objects/compactobject.[ch]` and `pcompactobject.[ch]`. The V2 C source (Tismer, 2009–10) is lost, so V2's fixes are known only from mail.

## How they worked

- **Layout** [read] (`compactobject.h`): `PyCompactObject` is `PyObject_HEAD`, then `char *k_data` (a `PyMem_Realloc`'d buffer), then `compact_impl_t *k_impl`.
  - A `compact_impl_t` holds `attrname`, `vattr`, `datasize`, `extensions`, `next` and `parent`.
  - The layouts form a tree from `k_empty_impl`; you walk `parent` to list the attributes.
  - `k_extend_impl` reuses a child with the same name, size and value pattern (`k_same_vinfo`). They are hidden classes.
- **Storage** [read]: `vattr` is a vinfo pattern. A run-time part is one word at an offset into `k_data` (`k_fix_run_time_vars`). A compile-time part takes no space; a virtual part recurses. So an int is stored unboxed and a constant costs nothing. The run-time path (`compact_set`) specialises only ints and `None`.
- **Compiled access** [read] (`pcompactobject.c`):
  - `pcompact_getattro`/`pcompact_setattro` are metas of the type's `tp_getattro`/`tp_setattro`. Both promote `k_impl` on every access (`pcompact_getimpl`), then emit loads and stores at fixed offsets (`psy_k_load_vinfo`, `psy_k_store_vinfo`).
  - A store that changes an attribute's format builds a new layout (`k_duplicate_impl`). Growing the buffer is an inline `PyMem_Realloc` call.
- **Generalisation** [read]: none. Armin's 2002 idea of layouts generalising like compiler states (mail `2002-04-21_999591`) was never built.
- **Type** [read]: `Py_TPFLAGS_HAVE_GC|BASETYPE`, `compact_traverse`/`compact_clear` over the run-time references, and `__dict__` as a getset over `py-support/kdictproxy.py`.
  - The metaclass `compacttype_new` forces `__slots__=()` and adds `psyco.compact` as a base.
  - `compact_dealloc` has no `PyObject_GC_UnTrack` and no trashcan, on every hg branch.

## What went wrong

- **GC untrack and trashcan** (mail `2009/2009-03-01_21708293.md`, V2 r62204: "Corrected the missing GC untracking, and added trashcan support"; the diff is lost).
  - `subtype_dealloc` re-tracks the object before calling a GC base's `tp_dealloc`, so every heap subclass reaches `compact_dealloc` still tracked [inferred].
  - Long attribute chains recurse without the trashcan.
- **"Promotion on every access"** (same mail): "The re-spawn/promotion engine cannot yet stand the demands of compact objects."
  - The symptom: the tests pass but Psyco crashes; V2's `setup.py` says "compact objects are not yet reliable".
  - "Say good-bye to 'The Bug (TM)'" (`2009-05-25_22535656.md`) gives no mechanism. The fix was in V2's lost core.
- **Likely causes in the surviving code** [inferred]:
  - `bc4b130` (2006, Armin) changed `pcompact_getimpl`. To promote `k_impl` on a compile-time object, it turns the vinfo's CompileTime source into a VirtualTime one in place, using `compute_stv_never`, which calls `psyco_fatal_msg`. It also stores the run-time impl in the vinfo's array, and relies on that mutated state surviving into the snapshot for the restarted instruction.
  - So anything that computes, compares or merges that state first can hit the fatal error or mismatch: merge points, `psyco_forking`, or a respawn replay through several such promotions.
  - It also rebuilds the constant without `SkFlagPyObj` after an `sk_decref`, so compiled code may hold a pointer it doesn't own.
- **Heap overflow, never fixed** [read]: `malloc_stv` allocates `64 * sizeof(struct source_tmp_virtual_s*)`, which sizes the block for pointers, not structs. It holds about 12 entries before it overruns: after about 12 distinct constant compact objects, the heap is corrupted.
- **Other** [read]:
  - `9a1e2b7` added a test of circular compact objects.
  - V2 r73974 (`psyco-checkins-wayback2015/2010/2010-04-22_25069762.md`) fixed a use-after-free in `genstate_dealloc`, incompletely.
  - `58aff23` (Armin): "Compact object is broken!"

## For a 3.7 restore

- **Python 2 and i386 specifics** [read]: names must be interned unicode (`PyUnicode_InternInPlace`; `compacttype_new`'s `"SO!O!"` becomes `"UO!O!"`). `PyInt` becomes Ni's virtual long. The Py2 flags (`Py_TPFLAGS_HAVE_CLASS`, `__members__`, `__metaclass__`, `UserDict.DictMixin`) go. Instantiation needs `psyco_pobject_new` (the removed `ptypeobject.c`).
- **Ni would never call the metas** [read]: `pobject.c:PsycoObject_GetAttr` is a plain residual `PyObject_GetAttr`, and `PsycoObject_SetAttr` goes generic for every unicode name. Both must dispatch on `tp_getattro`/`tp_setattro` again, as `9bd5834:c/Objects/pobject.c` did. The same is needed for attribute lookup in general (A9).
- **Dealloc on 3.7** [inferred]: use `PyObject_GC_UnTrack` and `Py_TRASHCAN_SAFE_BEGIN/END`, and allow for `tp_finalize` (PEP 442) and weakrefs (forcing `__slots__=()` removes `__weakref__`).
- **Order:**
  1. The run-time type, tested uncompiled.
  2. Meta dispatch in `PsycoObject_GetAttr`/`SetAttr`.
  3. A read-only getattr meta.
  4. Setattr and instance creation.
- **Don't port the in-place CompileTime-to-VirtualTime conversion.** For a compile-time object, read `k_impl` at compile time behind a guard, or promote a separate run-time load of it.
- **Tests to write:**
  - many promotions in one function and in loops, with a rarely taken branch so that respawn replays across them;
  - cycles and long chains;
  - heap subclasses, and `__del__` running during a store;
  - more than 12 distinct constant compact objects;
  - an attribute changing format (int, then float, then object);
  - layout explosion from compile-time attribute values.
