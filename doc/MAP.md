---
written: 2026-11-16
head_ref: e1fd808
---
# File map and coverage record

One row per file. **Every file has been read in full at least once.** The "reviewed" column gives the commit it was reviewed at; if the file has changed since, re-review it.

- Layers are `core`, `translated`, `backend`, `glue`, `tooling` and `obsolete` (see `GOAL.md`); a mixed file lists several. For the files split function by function (the inventory read at `b36a1ac`, now computed by `tools/metac/donecheck.py`, see `INVENTORY.md`), the layers follow line counts: the larger of core and translated first, a part under about a tenth of the file left out, and glue named only where it is most of the file. `b36a1ac` in the reviewed column is that inventory's read (every line, against 3.7.17).
- Only the complex files carry an explanatory comment at the top. Simple files have none, and their row here is the only summary.
- Breakdowns by function for the big mixed files are in `ARCHITECTURE.md` ("The mixed files, by section").
- Translation patterns are in `PATTERNS.md`; known bugs in `KNOWN_BUGS.md`.
- `*_gen.h` files are written into the build directory, not kept in git.

## Summary by layer

| Layer | Files | Notes |
|---|---|---|
| core | `vcompiler.*`, `dispatcher.*`, `codemanager.*`, `blockalloc.h`, `cf_flags.h`, `mergepoints.*`, `pycodegen.h`, `Python/frames.*`, `Python/pycheader.h`, `Python/pycompiler.h`, `Objects/pfuncobject.*`, and the layout headers `pboolobject.h`, `pclassobject.h`, `plistobject.h`, `pmethodobject.h`, `ptupleobject.h` | Design template: tidy, don't regenerate. |
| core + translated | `psyfunc.c`, `Objects/pobject.*`, the virtual-object files (`plistobject.c`, `ptupleobject.c`, `piterobject.*`, `prangeobject.*`, `pmethodobject.c`, `pclassobject.c`, `psliceobject.c`), `pfloatobject.h`, `plongobject.h` | Mostly core (virtual objects, call policy); the translated part is the slot logic. Split function by function by `tools/metac/donecheck.py` (`INVENTORY.md`). |
| translated + core | `Python/pycompiler.c`, `plongobject.c`, `pfloatobject.c`, `pbytesobject.c`, `punicodeobject.c`, `Python/pbltinmodule.c` | Mostly translation, with a core part (merge points and promotion in `pycompiler.c`; genexpr fusion, `super()` and the virtual `range` in `pbltinmodule.c`). |
| translated | `Objects/pabstract.c`, `pdictobject.c`, `pstructmember.c`, `pdescrobject.c`, `Modules/pmath.c`, `Python/pyruntime.c` | Meta-compiler output candidates. |
| backend | `x64/*`, `processor.h`, much of `codegen.c` | Rewrite behind a formal interface (`ARCHITECTURE.md`, Backend boundary). |
| glue | `ni.c`, `eval_hook.c`, `mechanisms/*/*.h` (D75), `initialize.h`, `compat2to3.h`, `psyco.h`, `platform.*`, `psyfunc.h`, `Python/pyver.h`, `Python/pycinternal.h`, `Objects/pabstract.h`, `pdictobject.h`, `pstructmember.h`, `psliceobject.h` | |
| effectively empty on Py3 | `Objects/pstringobject.*` (compiled out), `Objects/pboolobject.c` (empty) | |

## C sources

| file | layer | what it is | CPython counterpart | reviewed |
|---|---|---|---|---|
| `ni/blockalloc.h` | core | Fixed-size block allocator macros for vinfo_t and source_known_t. | none | `e1fd808` |
| `ni/cf_flags.h` | core | Cf* flags describing a C function for residual calls: purity, new ref, return type, errors. | none (CPython C API conventions) | `56e5faf` |
| `ni/codegen.c` | core + backend | codegen.h's code: condition-code vinfos, psyco_generic_call (CfPure folding), integer emitters, rich compare. | Objects/object.c | `56e5faf` |
| `ni/codegen.h` | core | Code generation API: condition codes, psyco_generic_call (residual calls), integer ops, promotion pseudo-exceptions. | none | `fa3dae7` |
| `ni/codemanager.c` | core | Executable memory as free extents; owners (D102): what a code object's compiled code holds, released with it; compile guards (D103). | none | `bc3c590` |
| `ni/codemanager.h` | core | CodeBufferObject: emitted machine code plus the snapshot it was compiled from; the owner and compile guard APIs. | none | `0b808fa` |
| `ni/compat2to3.h` | glue | NiCompat* macros mapping Py2 names (PyString/PyInt) onto Py3 (PyUnicode/PyLong). | Include/ | `e1fd808` |
| `ni/dispatcher.c` | core | Snapshot compression, psyco_compatible(), respawn, promotion dispatch and inline-cache chains, stubs and their records, per owner. | none | `a6a687b` |
| `ni/dispatcher.h` | core | Snapshots (FrozenPsycoObject), compatibility and join API, respawn and promotion helpers. | none | `f229d14` |
| `ni/eval_hook.c` | glue | Entry into compiled code: `ni_entry_point` compiles on the first call (interpreted if memory runs out), `NiCode_Run` runs it. | Python/ceval.c | `f254b5d` |
| `ni/initialize.h` | glue | Calls every file's init; with ALL_STATIC=1 includes all .c files into one unit. | none | `b251646` |
| `ni/mechanisms/entry/frame_eval_hook.h` | glue (mechanism snippet) | 3.7's entry (D75): the PEP 523 hook `NiEval_EvalFrame` and its installation. | Include/pystate.h | `36b5beb` |
| `ni/mechanisms/frames/frame_object.h` | glue (mechanism snippet) | 3.7's frames (D75): `ni_frame_t` is `PyFrameObject`, with inline accessors. | Include/frameobject.h, Include/pystate.h | `36b5beb` |
| `ni/mechanisms/tracing/tstate_use_tracing.h` | glue (mechanism snippet) | 3.7's tracing test (D75): `ni_tracing_now` at run time, `ni_tracing_test` emitted. | Include/pystate.h | `ee76274` |
| `ni/mechanisms/eval_breaker_runtime/ceval_loop_top.h` | translated (mechanism snippet) | 3.7's eval_breaker address and handler (D77), copied from ceval's loop top. | Python/ceval.c, Include/internal/ceval.h | `f254b5d` |
| `ni/mechanisms/eval_breaker/int32_address.h` | glue (mechanism snippet) | `ni_eval_breaker_test`: compiled code's 32-bit test of eval_breaker at its address (D77). | Include/internal/ceval.h | `f254b5d` |
| `ni/mechanisms/tstate_runtime/runtime_address.h`, `ni/mechanisms/tstate/runtime_address.h` | glue (mechanism snippets) | The current thread state (D77): its address and one load from it in compiled code. | Include/internal/pystate.h, Include/pystate.h | `f254b5d` |
| `ni/mechanisms/recursion/depth_up.h` | translated (mechanism snippet) | The recursion count (D77): at run time and emitted for compiled calls. | Include/ceval.h, Include/pystate.h | `f254b5d` |
| `ni/mechanisms/exceptions/curexc_triple.h` | glue (mechanism snippet) | The exception state (D77): inline PyErr_Occurred, get/put of raised and handled exceptions. | Include/pystate.h, Python/errors.c | `f254b5d` |
| `ni/mechanisms/store_gc/gc_refs.h` | backend (mechanism snippet) | `NI_GC_UNTRACKED_TEST`: the inline store's test that the dict is untracked (D77). | Include/objimpl.h | `f254b5d` |
| `ni/mechanisms/eval_breaker_runtime/{ceval_loop_top_signals,static_handler_copy,internal_handler_copy,handler_call}.h`, `ni/mechanisms/eval_breaker/tstate_word.h`, `ni/mechanisms/{tstate_runtime,tstate}/thread_local.h`, `ni/mechanisms/recursion/{remaining_down,py_remaining_down}.h`, `ni/mechanisms/exceptions/{curexc_triple_info_value,current_exception}.h`, `ni/mechanisms/store_gc/gc_next.h` | glue (stubs) | One `#error "... not written yet"` each, saying what the variant must do (D77). | 3.8–3.13 | `f254b5d` |
| `ni/mechanisms/entry/{frame_eval_hook_interp,vectorcall}.h`, `ni/mechanisms/tracing/{cframe_use_tracing,monitoring}.h`, `ni/mechanisms/frames/{interpreter_frame_cframe,interpreter_frame}.h` | glue (stubs) | One `#error "... not written yet"` each, saying what the variant must do. | 3.11–3.13 | `36b5beb` |
| `ni/mergepoints.c` | core | Finds merge points, dead locals, join points and eval_breaker tests in a code object; the opcode whitelist. | none (copies `NEXTOPARG` from Python/ceval.c) | `b36a1ac` |
| `ni/mergepoints.h` | core | Merge point tables: where control paths join and compiled states are cached. | none | `e26c2a3` |
| `ni/ni.c` | glue | Module entry (PyInit_ni), per-code `co_extra` records, `_stats` etc. and debug builds' `_fail_after`, out-of-memory and fatal-error handlers, `ni_trace_*` hooks. | none | `6cd0c26` |
| `ni/platform.c` | glue | Allocates executable (RWX) memory: mmap on Unix, VirtualAlloc on Windows. | none | `1e65d32` |
| `ni/platform.h` | glue | Prototype for psyco_allocate_executable_buffer(). | none | `e1fd808` |
| `ni/processor.h` | backend (interface) | Prototypes for running compiled code: psyco_processor_run, RUN_ARGC, aligned-call glues. | none | `4398496` |
| `ni/psyco.h` | glue | Global definitions: build switches, sizes, assertions, linkage macros, the `co_extra` record and its owner; includes `layouts_gen.h`. | none | `f254b5d` |
| `ni/psyfunc.c` | core + translated | Python calls at compile time: build the callee frame, inline small functions or compile and call. | Python/ceval.c, Objects/call.c, Include/ceval.h | `e11f2a3` |
| `ni/psyfunc.h` | glue | Declarations for psyfunc.c and the generated binder (D85). | none | `e11f2a3` |
| `ni/pycodegen.h` | core | Emits Py_INCREF/Py_DECREF and reference-ownership helpers on vinfo_t. | Include/object.h | `fa3dae7` |
| `ni/vcompiler.c` | core | vinfo lifetime, field get/put with load folding, coding pauses, the psyco_compile*() entry points. | none | `603363c` |
| `ni/vcompiler.h` | core | Central data model: Source, vinfo_t, field descriptors, PsycoObject (compiler state). Start here. | none | `ee76274` |
| `ni/Modules/pmath.c` | translated + glue | ceil and floor of an exact float; registers the generated math functions. | Modules/mathmodule.c | `cddaf8d` |
| `ni/Objects/pabstract.c` | translated + core | The hand-written rest of the abstract protocol (`**`, `iter()`, next, int subscripts). | Objects/call.c, Objects/abstract.c, Objects/tupleobject.c, Objects/listobject.c | `351a4d5`, `935df0b` |
| `pabstract_gen.h` | translated (generated) | The abstract protocol's metas and run-time copies, by `gen2/abstract.py` (D74, D104). | Objects/abstract.c | `df8385b` |
| `ptupleobject_gen.h` | translated (generated) | The tuple slots `TRANSLATED_SLOTS` names, as metas, by `gen_abstract.py --unit tuple`. | Objects/tupleobject.c | `935df0b` |
| `plistobject_gen.h`, `pdictobject_gen.h`, `pbytesobject_gen.h` | translated (generated) | The length slots of list, dict and bytes and their inits, by `gen_abstract.py` (D84). | Objects/listobject.c, Objects/dictobject.c, Objects/bytesobject.c | `5169c24` |
| `punicodeobject_gen.h`, `pstructmember_gen.h`, `pdescrobject_gen.h` | translated (generated) | Metas for str's slots, PyMember_GetOne and member_get, by `gen_abstract.py` (D89). | Objects/unicodeobject.c, Python/structmember.c, Objects/descrobject.c | `ccb5735`, `03ce514` |
| `pobject_gen.h` | translated (generated) | object.c's functions in other types' slots, by `gen_abstract.py` (D93). | Objects/object.c | `9e58f97` |
| `virtual_fields_gen.h`, `ptupleobject_virtual_gen.h`, `psliceobject_virtual_gen.h`, `pclassobject_virtual_gen.h` | translated (generated) | Virtual objects (D80) by `gen_virtual.py`: descriptors and, per type, force and constructor. | Objects/tupleobject.c, Objects/sliceobject.c, Objects/classobject.c | `a6a687b` |
| `psyfunc_gen.h` | translated (generated) | `_PsycoEval_EvalCodeWithName`: argument binding at compile time (D85), by `gen_call.py`. | Python/ceval.c | `e11f2a3` |
| `ni/Objects/pabstract.h` | glue | Declarations of the abstract protocol's metas (generated and hand-written). | Include/abstract.h | `351a4d5`, `935df0b` |
| `ni/Objects/pboolobject.c` | translated | `PsycoBool_FromLong` and `PsycoBool_FromZeroOrOne`. | Objects/boolobject.c | `58e52de` |
| `ni/Objects/pboolobject.h` | core | Virtual bools: a virtual int of type bool whose word is a condition. | Objects/boolobject.c | `b36a1ac` |
| `ni/Objects/pclassobject.c` | core + translated | PyMethod tp_call meta (prepends self, re-dispatches); virtual methods generated. | Objects/classobject.c | `a6a687b` |
| `ni/Objects/pclassobject.h` | core | Declarations: `PsycoMethod_New(func, self)`, the generated vinfos, the tp_call meta. | Include/classobject.h | `a6a687b` |
| `ni/Objects/pdescrobject.c` | translated + glue | The method descriptor `__get__` meta; includes the generated member_get. | Objects/descrobject.c | `ccb5735` |
| `ni/Objects/pdescrobject.h` | glue | Placeholder header for descriptor metas; declares nothing. | Include/descrobject.h | `e1fd808` |
| `ni/Objects/pdictobject.c` | translated | Residual-call wrappers for dict creation; registers the generated `dict_length`. | Objects/dictobject.c | `351a4d5`, `0f88313` |
| `ni/Objects/pdictobject.h` | glue | API for (non-virtual) dict creation through residual calls. | Include/dictobject.h | `351a4d5`, `0f88313` |
| `ni/Objects/pfloatobject.c` | translated + core | Virtual floats: representation, float/float comparisons, helpers; includes the generated slots. | Objects/floatobject.c | `87b5ff5` |
| `ni/Objects/pfloatobject.h` | core + translated | Field descriptor and virtual-float builder (one vinfo holds the double). | Include/floatobject.h | `ccb5735` |
| `ni/Objects/pfuncobject.c` | core | Virtual function objects and tp_call/tp_descr_get metas that inline Python calls. | Objects/funcobject.c | `a6a687b` |
| `ni/Objects/pfuncobject.h` | core | PyFunctionObject field descriptors and the virtual-function and call API. | Include/funcobject.h | `ee76274` |
| `ni/Objects/piterobject.c` | core + translated | Virtual sequence iterators and their tp_iternext meta. | Objects/iterobject.c | `9e58f97` |
| `ni/Objects/piterobject.h` | core + translated | seqiterobject descriptors (private struct re-declared) and virtual iterator constructor. | Objects/iterobject.c | `ee76274` |
| `ni/Objects/plistobject.c` | core + translated | Virtual short lists, item get/set and concat metas, tp_iter as sequence iterators. | Objects/listobject.c | `b36a1ac`, `5169c24` |
| `ni/Objects/plistobject.h` | core | Field descriptors and API for virtual short lists (at most VLIST_LENGTH_MAX items). | Include/listobject.h | `ee76274` |
| `ni/Objects/plongobject.c` | translated + core | Virtual ints: representation, comparisons, hash, word shortcuts, `PsycoNumber_AsSsize_t`. | Objects/longobject.c | `351a4d5` |
| `plongobject_gen.h` | translated (generated) | The int slots, word representation (D83), by `gen_abstract.py`'s long unit. | Objects/longobject.c | `a0c8ea9` |
| `pfloatslots_gen.h` | translated (generated) | The float slots and `PsycoFloat_AsDouble`, by `gen_abstract.py` (D87). | Objects/floatobject.c | `87b5ff5` |
| `pmathmodule_gen.h` | translated (generated) | The math functions, by `gen_builtins.py`'s math unit (D87). | Modules/mathmodule.c | `cddaf8d` |
| `ni/Objects/plongobject.h` | core + translated | Field descriptors, virtual-int constructor, `PsycoLong_Sign`. | Include/longintrepr.h, Include/longobject.h | `351a4d5` |
| `ni/Objects/pmethodobject.c` | core + translated | Virtual C-function objects and the PyCFunction tp_call meta, dispatching on ml_flags. | Objects/methodobject.c | `f254b5d` |
| `ni/Objects/pmethodobject.h` | core | PyCFunctionObject descriptors, virtual C-method builder, METH_FASTCALL meta convention. | Include/methodobject.h | `ee76274` |
| `ni/Objects/prangeobject.c`, `.h` | core + translated | Virtual ranges (words) and range iterators. | Objects/rangeobject.c | `9e58f97` |
| `ni/Objects/psliceobject.c` | core + translated | Virtual slices (D56); `PsycoSlice_Unpack` gives step-1 bounds as words. | Objects/sliceobject.c, Python/ceval.c | `a6a687b` |
| `ni/Objects/psliceobject.h` | glue | Declarations of `PsycoSlice_New` and `PsycoSlice_Unpack`. | Include/sliceobject.h | `b36a1ac` |
| `ni/Objects/pobject.c` | core + translated | Generic object protocol metas (truth, attributes, hash, compare); attribute loads and stores behind type guards. | Objects/object.c | `9e58f97` |
| `ni/Objects/pobject.h` | core + translated | Compile-time PyObject API: ob_type/ob_size descriptors, type promotion (`Psyco_NeedType` etc.). | Include/object.h | `351a4d5`, `935df0b` |
| `ni/Objects/pbytesobject.c` | translated + core | `bytes` metas: `len()`, inline `b[i]`, `b[i:j]` of a virtual slice. | Objects/bytesobject.c | `ee76274`, `5169c24` |
| `ni/Objects/pstringobject.c` | core + translated | Py2 str metas with virtual strings; compiled out on Py3. | Objects/stringobject.c (Py2) | `b36a1ac` |
| `ni/Objects/pstringobject.h` | core + translated | Py2 virtual-string API (char, slice, bufstr layouts); empty on Py3. | Include/stringobject.h (Py2) | `b36a1ac` |
| `ni/Objects/punicodeobject.c` | translated + core | `str` metas (D14): `==`/`!=`, inline `s[i]` for compact ASCII, slices; includes generated slots. | Objects/unicodeobject.c, Include/unicodeobject.h | `03ce514` |
| `ni/Objects/pstructmember.c` | translated (generated) | Includes the generated `PyMember_GetOne` (`pstructmember_gen.h`). | Python/structmember.c | `ccb5735` |
| `ni/Objects/pstructmember.h` | glue | Declaration of PsycoMember_GetOne. | Include/structmember.h | `b36a1ac` |
| `ni/Objects/ptupleobject.c` | core + translated | PsycoTuple_Load; includes the generated virtual tuple and slots. | Objects/tupleobject.c | `935df0b` |
| `ni/Objects/ptupleobject.h` | core | API for virtual and compile-time tuples (descriptors generated, D80). | Include/tupleobject.h | `603363c` |
| `ni/Python/frames.c` | core | Maps machine-code stack frames to Python frames for `sys._getframe` and tracebacks. | none (but `ni_getframe`: Python/sysmodule.c) | `f254b5d` |
| `ni/Python/frames.h` | core | Stack frame info (stack_frame_info_t) and frame registration API. | none | `6f8009a` |
| `ni/Python/pbltinmodule.c` | translated + core | Registers the generated builtins; hand metas for min, max, sum, round etc. and type_call (`range`, `super`). | Python/bltinmodule.c, Objects/abstract.c, Objects/typeobject.c | `cddaf8d` |
| `ni/Python/pbltinmodule.h` | glue | Declares `PsycoObject_IsInstance`. | Python/bltinmodule.c | `58e52de` |
| `ni/Python/pycheader.h` | core | pyc_data_t (language-specific compiler state) and the LOC_* slots in po->vlocals. | Include/frameobject.h | `56e5faf` |
| `ni/Python/pycinternal.h` | glue | Includes CPython internal headers and opcode names. | Include/opcode.h | `e1fd808` |
| `ni/Python/pycompiler.c` | translated + core | The bytecode compiler: ceval's main loop at compile time, block unwinding, exceptions, calls. | Python/ceval.c | `ab0f53b` |
| `pycompiler_gen.h` | translated (generated) | 75 opcode cases included in `pycompiler.c`'s switch, by `tools/metac/gen_ceval.py`. | Python/ceval.c | `ab0f53b` |
| `pycompiler_copies_gen.h` | translated (generated) | Copies of the ceval helpers and arms the generated cases call (`gen_ceval:build_copies`). | Python/ceval.c | `e17f6fc` |
| `mechanism_<name>_gen.h`: entry, tracing, frames, eval_breaker_runtime, eval_breaker, tstate_runtime, tstate, recursion, exceptions, store_gc | glue (generated) | One per mechanism, by `mechanisms.py`: detector answers and the snippet `VARIANTS` chose. | the definitions the detectors read | `f254b5d` |
| `layouts_gen.h`, `runtime_layouts_gen.h` | translated (generated) | Offsets, sizes and `_Static_assert`s for each CPython field Ni relies on (D76). | Include/** (the structs `LAYOUTS` and `RUNTIME` name) | `f254b5d` |
| `ni/Python/pycompiler.h` | core | Pseudo-exceptions, promotion helpers, the meta registry (Psyco_META*), pyc_data_t snapshots. | Python/errors.c | `f254b5d` |
| `ni/Python/pyruntime.c` | glue | The only file built with Py_BUILD_CORE: home of two mechanisms' snippets (D77). | none itself | `f254b5d` |
| `ni/Python/pyver.h` | glue | Python-version feature switches. | none | `4a0d275` |
| `ni/x64/idispatcher.c` | backend + core | psyco_unify(): emits moves and refcounts to match a snapshot, then jumps to it. | none | `a4b946e` |
| `ni/x64/idispatcher.h` | backend | Dispatcher hooks: promotion inline cache and chain, jumps and patching, unify blocks, stubs. | none | `a599bd2` |
| `ni/x64/iencoding.c` | backend + glue | C calls (SysV at run time, libffi at compile time), psyco_call_psyco, cc and ALU emitters. | none (encodes CPython error conventions) | `f254b5d` |
| `ni/x64/iencoding.h` | backend | x86-64 encoders, register and cc model, call macros, jumps: the main core-to-backend API. | none | `2063657` |
| `ni/x64/iinitialize.h` | glue | Runs psyco_processor_init(); under ALL_STATIC includes the x64 `.c` files. | none | `e1fd808` |
| `ni/x64/iprocessor.c` | backend | Run-time entry glue, the stub trampoline, stack frame walking, aligned-call glues. | none | `ee76274` |
| `ni/x64/ipyencoding.c` | backend | function_return(), decref emitters, the inline attribute store (D47), the type guard. | Include/object.h, Objects/dictobject.c | `0d378fb` |
| `ni/x64/ipyencoding.h` | backend (CPython object layout leaks in: ob_refcnt, ob_type->tp_dealloc) | Py_INCREF/Py_DECREF emitters, frame prologue/epilogue macros, store and guard prototypes. | Include/object.h | `0d378fb` |
| `ni/x64/v_mem_mov.h` | backend | v_mem_mov(): one sized load/store, for field get/put on run-time objects. | none | `f53ef68` |
| `tools/test_mcg_template.c` | tooling | Template: expands one iencoding.h macro and prints its bytes, for test_asm. | none | `e1fd808` |
| `tools/asm_microbenchmarks/bench_baseline.c` | tooling | Micro-benchmark: empty loop (baseline). | none | `e1fd808` |
| `tools/asm_microbenchmarks/bench_compile_time_immed.c` | tooling | Micro-benchmark: MOV reg, imm64 in loop. | none | `e1fd808` |
| `tools/asm_microbenchmarks/bench_compile_time_reg.c` | tooling | Micro-benchmark: CMP reg, reg in loop. | none | `e1fd808` |
| `tools/asm_microbenchmarks/bench_compile_time_stack.c` | tooling | Micro-benchmark: CMP reg, [rsp] in loop. | none | `e1fd808` |
| `tools/asm_microbenchmarks/benchmark.h` | tooling | Harness macros: emit a counted loop with iencoding.h macros and time it. | none | `e1fd808` |

## Other files

| file | layer | what it is | reviewed |
|---|---|---|---|
| `setup.py` | glue | distutils build of the `ni` extension; runs `generate.py` first, with `PYTHONHASHSEED=0` (D68). | `ac8f978` |
| `tests/run.py`, `tests/child.py`, `tests/cases/*.py` | tooling | Differential behaviour tests with compile and leak checks (BUILD.md, Tests). | `ccb5735` |
| `tests/encoding/check.py` | tooling | Checks `@asm`-annotated emitters in `iencoding.h` against GNU `as`. | `6ca580a` |
| `tests/smoke.py` | tooling | Stdlib smoke test: about 30 modules, output must match with and without Ni. | `6ca580a` |
| `tests/compile_bounds.py` | tooling | Fails if warm code keeps starting code buffers (`ni._stats()`); run by check.sh. | `932f416` |
| `tests/cpython_tests.py` | tooling | Runs CPython's test modules with and without Ni and compares counts and exit status. | `fbb5f4a` |
| `tests/mechanisms.py` | tooling | Checks the mechanism detectors' answers on five CPython versions (D75). | `f254b5d` |
| `tests/parse_gate.py` | tooling | Migration step 1: every mirrored CPython `.c` file preprocesses and parses on 3.7 and 3.8, with right line numbers; run by check.sh. | `7f2955a` |
| `tests/effect_snippets.py` | tooling | Snippet tests of `effects.py` (G1–G4, each flag, the `Cf*` mapping), no build; run by check.sh. | `041bbd1` |
| `tests/gen2_snippets.py` | tooling | Snippet tests of the new translator: C read after abstract.c, the meta or refusal out, for each pass and action and G6–G10, G15; no build; run by check.sh. | `df8385b` |
| `tools/check.sh` | tooling | Runs the parse gate, the effect and translator snippets, the generators under two hash seeds against the baseline, the 3.8 dry run, `donecheck.py`, a checked build and every suite. | ac8f978 |
| `tools/metac/ctokens.py`, `cslots.py` | tooling | A C tokeniser, and the shared slot driver (`Target`, `GenError`, `text`). | `7b21dea` (ctokens), `9eae61d`, `2a71def` (cslots: `PyNumberMethods` from any `Include/**/*.h`) |
| `tools/metac/gen_ceval.py` | tooling | Writes `pycompiler_gen.h` from instruction definitions, checked against `dis` (D69). | `ac8f978` |
| `tools/metac/ceval_inline.py` | tooling | Inlines a static function of `ceval.c` into the case that calls it (D82). | `fa3dae7` |
| `tools/metac/gen_abstract.py`, `cparse.py` | tooling | General translation (D74): `cparse` reads C; `gen_abstract` translates the functions Ni declares a meta for in the units it still has (not abstract.c). | `5bcffd4` |
| `tools/metac/gen2/` | tooling | The new translator (GENERATOR_DESIGN), on the front end's AST: rules (`vocabulary`, `types`, `conventions`, `scope`, `policy`), C typing and printing (`ctyping`, `cprint`), analyses (`liveness`, `bta`), path state (`values`), the walk and its actions (`walk`, `actions`, `resume`), the C builder (`cbuild`); `abstract.py` writes `pabstract_gen.h` (D104–D107). | `df8385b` |
| `tools/metac/scalars.py` | tooling | Scalar code (D87): the `double` representation, copy-through into C helpers, static inlining. | `ac8f978` |
| `tools/metac/conditions.py`, `pointers.py` | tooling | Translator rules (D89): `ConditionRules` and `PointerRules`. | `ccb5735` |
| `tools/metac/ceval_reader.py` | tooling | Reads `ceval.c`'s `TARGET` cases as `bytecodes.c` definitions (D69). | `96eeba2` |
| `tools/metac/generate.py` | tooling | The one entry point for generation (D68): checks, runs every generator (gen2's included), writes headers and `report.txt`, compares it with `expected/` (D98). | `df8385b` |
| `tools/metac/gen_virtual.py` | tooling | Virtual objects (D80): from each immutable `TYPES` virtual's constructor. | `ac8f978` |
| `tools/metac/gen_call.py` | tooling | The call path (D85): argument binding as a C function the compiler runs. | `ac8f978` |
| `pbltinmodule_gen.h` | translated (generated) | The builtins' metas (Python/bltinmodule.c), by `gen_builtins.py` (D86). | `58e52de` |
| `tools/metac/gen_builtins.py` | tooling | Builtins (D86): writes `pbltinmodule_gen.h`; also the math unit. | `ac8f978` |
| `tools/metac/csource.py` | tooling | Names definitions in a C source tree, for the policy table's keys (D71); a lookup in a file it can't balance raises. | `ac8f978` |
| `tools/metac/policy.py` | tooling | The policy table's vocabulary and checks against the source and Ni (D71); a gen2 row has no shape (D106). | `5bcffd4` |
| `tools/metac/policy_table.py` | tooling (data) | The policy table (D71): entries keyed by CPython definitions, rules, limits, types, layouts; abstract.c's generated rows only name gen2 (D106). | `5bcffd4` |
| `tools/metac/effects.py` | tooling | Effect rules (D71, D96): per C function of libpython and the mirrored files, on the front end's AST, pure, readonly, no Python, no blocking, no raise; mapped onto `Cf*` flags; the vocabulary's effects from `gen2/vocabulary.py`. | `7f2955a` |
| `tools/metac/cpython_src.py`, `cpython_sha256.txt` | tooling | The CPython source for a version, local or a sha256-checked tarball. | `ac8f978` |
| `tools/metac/mechanisms.py` | tooling | Mechanism detectors (D75); `VARIANTS` maps answers to snippets. | `f254b5d` |
| `tools/metac/gen_layouts.py` | tooling | Stage 0 (D76): finds and checks each `LAYOUTS` struct, writes the layout headers. | `9869ad9` |
| `tools/metac/donecheck.py` | tooling | GOAL's done test (D90, D97): each Ni mirror of CPython, generated or hand-written and why, from `report.txt`. | `ac8f978` |
| `tools/metac/expected/*.txt` | tooling | Each version's expected `report.txt`, which `generate.py` compares (D98). | `df8385b` |
| `tools/metac/status_diff.py` | tooling | Compares two versions' status files key by key, for a port review (D98). | `ac8f978` |
| `tools/metac/hand_table.py` | tooling (data) | Why each hand-written mirror is still hand-written (D90). | `18c250d` |
| `tools/metac/cases_generator/` | tooling (vendored) | CPython 3.12.9's DSL lexer and parser, unchanged. | `69900ac` |
| `tools/metac/front.py` | tooling | The new front end (GENERATOR_DESIGN "Front end"): `gcc -E` with the target's `pyconfig.h` and build flags (a file's own Makefile rule and Setup line included), the vocabulary shim spliced in and checked, parsed by pycparser (D94, D95); which files libpython links and which Ni mirrors. The vocabulary comes from `gen2/vocabulary.py`; `parse` takes rules' C read after a file. | `df8385b` |
| `tools/metac/shim/prelude.h` | tooling | The GNU prelude (`-include`) that `front.py` uses; the vocabulary shim is written from `gen2/vocabulary.py` into the cache (D104). | `7f2955a` |
| `tools/metac/third_party/pycparser/` | tooling (vendored) | pycparser 3.11 from PyPI's sdist, unchanged (D94). | `a3433b4` |
| `tools/proto312/` | tooling | 3.12 feasibility prototype (`PORT312.md`); evidence only, not part of Ni. | `92c51c6` |
| `tools/without_fix.sh` | tooling | Runs a command with changes stashed, to show a test fails without its fix (D21). | `4a5a3e5` |
| `tools/demo.py` | tooling | Demo: n-queens, n-body, ASCII Mandelbrot, word counts, timed with and without Ni. | `e4f21aa` |
| `tools/bench.py` | tooling | Benchmarks against plain CPython: ints, floats, builtins, isinstance, demos. | `29fe738` |
| `tools/profile_bench.py` | tooling | Runs one pyperformance benchmark warm under Ni for `perf record` (`PROFILE.md`). | `36a6939` |
| `tools/storeclasses.py` | tooling | Classifies benchmark `STORE_ATTR`s by effect on the dict (`FINDINGS.md "Attribute stores"`). | `730d24a` |
| `tools/store_micro.py` | tooling | Cost of one attribute store with or without Ni (`FINDINGS.md "Attribute stores"`). | `730d24a` |
| `tools/loadguards.py` | tooling | How often load guards fail in warm code, and why (`FINDINGS.md "Attribute stores"`). | `6cd0c26` |
| `tools/load_micro.py` | tooling | Loads on a changing class or one with a metaclass (`FINDINGS.md "Attribute stores"`). | `6cd0c26` |
| `tools/store_cache_sim.py` | tooling | Simulates FINDINGS.md "Attribute stores" option 4's per-(class, name) store cache over benchmark stores. | `074d210` |
| `tools/pyperf37.py` | tooling | Runs the vendored pyperformance benchmarks under 3.7 with and without Ni. | `88b1fa1` |
| `tools/benchmarks.sh` | tooling | Release build plus the benchmark tools, saved in `build/bench/`; `adversarial` only when named (`--long` passed on), with its data file beside the table. | `a86aeef` |
| `tools/codesize.py` | tooling | Code emitted by kind, opcode, function, instruction; promotion sites (`FINDINGS.md "Code size"`). | `8081f96` |
| `tools/samecode.py` | tooling | Whether two builds emit the same machine code, per instruction (D70; `FINDINGS.md "Code size"`). | `8b8ffea` |
| `tools/promostats.py` | tooling | The promotion-site reports for `codesize.py` (`-p`, `-D`), from `ni._promostats()`. | `8081f96` |
| `tools/compilebench.py` | tooling | Compile cost against pay-off per benchmark (`FINDINGS.md "Compile cost"`). | `f598d22` |
| `tools/adversarial/run.py` | tooling | Runs the adversarial benchmarks with and without Ni, many fresh processes per mode and run length: every call timed (total, first, median, worst, spreads, warm-up shape), trained on variant 0 and timed on variant 1, outputs compared call by call, losses first; `--calibrate` writes `sizes.txt`; `--data` writes one line per benchmark and measure. | `a86aeef` |
| `tools/adversarial/sizes.txt` | tooling | n per run length for each benchmark, calibrated on plain CPython by `run.py --calibrate`. | `a86aeef` |
| `tools/adversarial/*.py` | tooling | One benchmark per file aimed at a Ni weakness (its docstring names it and what variant 1 changes), `main(n, variant=0)` and `N` for running it alone; written blind, not to be tuned on Ni's results. | `a86aeef` |
| `tools/closurecalls.py` | tooling | Counts calls straight to closures per benchmark iteration (`FINDINGS.md "Calls to closures"`). | `b11de13` |
| `tools/closure_micro.py` | tooling | One loop calling a function or each kind of closure, for `perf stat` (`FINDINGS.md "Calls to closures"`). | `b11de13` |
| `tools/interpreted.py` | tooling | Per benchmark, the most-called functions still interpreted under Ni, with the reason. | `5f1dbf6` |
| `tools/pyperformance/` | tooling | 23 pure-Python benchmarks from pyperformance 1.11.0 (MIT). | `88b1fa1` |
| `tests/encoding/v_mem_mov_test.c`, `run_v_mem_mov.sh` | tooling | Executes `v_mem_mov`'s code and checks the loads and stores. | `de820e8` |
| `setup.cfg`, `MANIFEST` | glue | Packaging metadata. | `e1fd808` |
| `.clang-format` | tooling | PEP 7-style formatting. | `e1fd808` |
| `README.md` | doc | Original Ni README, plus a 2026 status note. | `e1fd808` |
| `AUTHORS`, `LICENSE`, `COPYING.txt` | doc | MIT licence; Psyco and Ni authors. | `e1fd808` |
| `doc/psyco_legacy/*` | doc | *The Ultimate Psyco Guide* (LaTeX) and links to Armin Rigo's papers and talks. | `e1fd808` |
| `tools/nianalyser/` | tooling | Trace analyser: breakpoints on the `ni_trace_*` hooks, disassembly. | `17c3ed8` |
| `tools/test_harness` | tooling | Counts `NiEval_EvalFrame` against `NiCode_Run` hits under `pointbreak`. | `e1fd808` |
| `tools/nianalyser/dump_function.py` | tooling | Prints the annotated machine code generated for one function (BUILD.md, Debug). | `28f3bae` |
| `tools/nianalyser/crash_report.py` | tooling | Explains a crash in emitted code (BUILD.md, Debug). | `2f23846` |
| `tools/test_runner` | tooling | unittest main; imports `ni` if `NI_ENABLED` is set. | `e1fd808` |
| `tools/asm`, `tools/test_asm`, `tools/asm_test_cases.txt` | tooling | Compare encoder-macro bytes with gas output. | `e1fd808` |
| `tools/test_c_encoding` | tooling | Expand one macro and disassemble it (Python 2). | `e1fd808` |
| `tools/gen_opcodes_h.py` | tooling | Generates an opcode-name table. | `e1fd808` |
