---
written: 2026-11-17
status: conclusion of the research for restoring compact objects (A11); nothing restored. The full study is `git show e404100:doc/history/compact-objects.md`.
---
# Compact objects

Upstream's `psyco.compact` (`9bd5834:c/Objects/compactobject.[ch]`, `pcompactobject.[ch]`) gave instances hidden-class layouts: a tree of `compact_impl_t` shared by objects with the same attributes in the same formats, with run-time parts stored unboxed at fixed offsets and compile-time parts taking no space. Compiled access promoted the layout on every access and then loaded and stored at fixed offsets. Layouts never generalised (Armin's 2002 idea that they could, like compiler states, was not built).

What went wrong, from the code and the psyco-devel mail (Psyco V2's C source is lost):
- `compact_dealloc` neither untracked the object nor used the trashcan (fixed in V2, the diff lost).
- Promotion on every access was more than the respawn and promotion engine could stand in V2. In the surviving code, `pcompact_getimpl` (`bc4b130`) turns a compile-time vinfo into a virtual one in place, with a compute function that is fatal, and relies on that state surviving into the snapshot [inferred].
- `malloc_stv` sizes its block for pointers, not structs: about 12 distinct constant compact objects overrun the heap [read].

For Ni: the main case compact objects served, attribute access on instances with a stable layout, is now covered by inline loads and stores on CPython's own shared keys (PEP 412; D31, D38). A restore on 3.7 would need interned unicode names, `PyObject_GC_UnTrack` and the trashcan in dealloc, `tp_finalize` and weakrefs allowed for, and must not port the in-place compile-time-to-virtual conversion: read the layout at compile time behind a guard, or promote a separate run-time load of it. Test respawn replays across many promotions, cycles and long chains, heap subclasses, `__del__` during a store, more than 12 constant objects, and an attribute changing format.
