---
upstream_ref: 9bd5834
head_ref: e1fd808
written: 2026-11-16
---

# Files and subsystems removed since upstream Psyco

Scope: whole files/subsystems present in upstream Psyco (`9bd5834`, Armin Rigo,
2010-02-22, py27 head) that are absent or gutted at Ni HEAD (`e1fd808`). Mechanisms
removed *inside* surviving files are documented separately; they are only
mentioned here where a feature spanned a removed file and surviving hooks.

Path mapping: upstream `c/` became `ni/` in `58aff23`; `c/i386` was copied
verbatim to `c/x64` in `4b45ec8` (diff of all four i386/x64 files at that commit:
zero lines differ) and then edited; `py-support/` → `psyco/` (`52eff9f`) →
`ni/psyco/` (`56d4fa0`) → deleted (`58aff23`).

Evidence tags: **[read]** = seen in code/diff/commit message; **[inferred]** = my
deduction. Categories: (a) core specialiser design worth keeping, (b) CPython
translation a meta-compiler would regenerate, (c) backend, (d) tooling/packaging,
(e) obsolete (Py2-only, platform-specific, etc.).

Key removal commits [read]:

| commit | date | message |
|---|---|---|
| `52eff9f` | 2016-01-20 | Move to Ni |
| `012b48f` | 2018-07-22 | WIP on pointbreak based analyser |
| `892d5a6` | 2019-02-09 | Fixes and cleanup |
| `05f85f4` | 2019-02-09 | Removed old Makefile |
| `58aff23` | 2019-02-14 | Cleanup for Py3 |
| `a880444` | 2020-04-12 | Start of Python3 |
| `ee1161b` | 2020-04-24 | prangeobject needs fixing |

`58aff23` body [read]: "Removes a lot of the unneeded/unused functionality from
Psyco... Most of the code removed was related to debugging which can now be done
with nianalyser or dynamic profiling for code compilation which is not an
immediate goal for Ni. Compact object is broken! But it needed rethinking anyway."
`a880444` body [read]: mergepoints stripped to a minimal version; "Many
optimization have been removed"; "Compact object has been removed mainly because I
don't understand it so wasn't sure how to port it. It is very fast when used so
analysis should be done around if or how to reintroduce it."

## Summary table

| item | upstream path | removed in | category | restore difficulty |
|---|---|---|---|---|
| Compact objects (`psyco.compact`, `compacttype`) | `c/Objects/compactobject.[ch]`, `c/Objects/pcompactobject.[ch]`, `py-support/kdictproxy.py`, `py-support/classes.py` | `a880444` (C), `58aff23` (Py) | a | hard |
| Virtual Py2 int | `c/Objects/pintobject.[ch]` | `a880444` | a (pattern) + b (int slots) | medium |
| Virtual range/xrange | `c/Objects/prangeobject.[ch]` | ported in `a880444`, deleted `ee1161b` | b (+a pattern) | easy-medium |
| Type call / object instantiation | `c/Objects/ptypeobject.[ch]` | `a880444` | b | medium |
| `array` module meta-impl | `c/Modules/parray.c` | `a880444` | b | medium |
| Frame-introspecting builtins (`_psyco.globals/eval/locals/...`) | `c/Modules/ppsyco.c` | `58aff23` | b/e | low value |
| Bool / str meta-impls (gutted, files remain) | `c/Objects/pboolobject.c`, `c/Objects/pstringobject.c` | `a880444`, `7441415`/`58aff23` | b | medium |
| C-struct-as-PyObject helper | `c/cstruct.[ch]` | `a880444` | d (infrastructure) | trivial (superseded by PyCapsule) |
| Profiler / turbo (OSR entry) | `c/profile.[ch]`, `c/stats.[ch]`, `c/timing.h`, `c/alarm.[ch]`, `py-support/profiler.py`, `py-support/logger.py` | `58aff23` | a (selection policy, OSR) | hard |
| `_psyco` extension module entry | `c/psyco.c` | `58aff23` (replaced by `ni/ni.c`) | d | n/a (replaced) |
| Python package `psyco` (`full/bind/proxy/...`) | `py-support/*.py` | `58aff23` | d | medium |
| ivm backend + Prolog VM generator | `c/ivm/**` | `892d5a6` | c | hard, low value |
| i386 backend | `c/i386/*` | `892d5a6` (after copy to x64 in `4b45ec8`) | c | n/a (lives on as x64) |
| Build/source-list tooling | `c/files.py`, `c/depend.py`, `c/mkincl.py`, `c/Makefile`, `c/versionchecker.py`, `c/win32/psyco.vcproj` | `58aff23`, `05f85f4`, `892d5a6` | d | easy |
| Opcode coverage checker | `c/mychecker.py` | `58aff23` | d (relevant to meta-compiler) | easy |
| Memory debugging | `c/linuxmemchk.[ch]` | `58aff23` | d | easy |
| Code-buffer inspection tools | `py-utils/*` (`xam`, `httpxam`, `vinfo_dump`, `symbols`, `ivm*`, ...) | `012b48f` | d | medium (superseded by nianalyser) |
| Upstream test suite | `test/*.py` etc. | `52eff9f` | d | easy (needs Py3 port) |
| Docs | `doc/*`, `README.txt` | moved to `doc/psyco_legacy/` in `6a376a6`; `README.txt` deleted `52eff9f` | d | n/a |

## Items

### Compact objects — `psyco.compact`, `psyco.compacttype`, `psyco.classes.psyobj`

- Upstream: `c/Objects/compactobject.[ch]` (run-time type, 1033 lines),
  `c/Objects/pcompactobject.[ch]` (meta-implementation, 653 lines),
  `py-support/kdictproxy.py` (`compactdictproxy`, a `DictMixin` exposing
  `__dict__` via `compact.__getslot__/__setslot__/__delslot__/__members__`),
  `py-support/classes.py` (`psymetaclass(compacttype)`, `psyobj`, `__metaclass__`). [read]
- What it did [read]: `PyCompactObject` = `PyObject_HEAD` + `k_data` (heap buffer)
  + `k_impl` (`compact_impl_t*`). `compact_impl_t` is a tree of "layouts"
  (`attrname`, `vattr`, `datasize`, `extensions`, `next`, `parent`) rooted at
  `k_empty_impl`; adding an attribute follows/creates an extension via
  `k_extend_impl()` — essentially hidden classes/shapes. The distinctive part: each
  layout node stores a `vinfo_t` *pattern* (`vattr`) describing how the value is
  stored; `k_store_vinfo()` writes only run-time parts (raw `long` or `PyObject*`
  at stack-index offsets) and recurses into virtual sub-arrays, so compile-time
  constants take no storage and virtual objects (e.g. an int) are stored unboxed.
  `pcompactobject.c` (`pcompact_getattro`, `pcompact_setattro`, `pcompact_new`,
  `psy_k_load_vinfo`, `psy_k_store_vinfo`) lets the specialiser promote on
  `k_impl` and rebuild attributes as vinfos — attribute values keep their
  specialised representation across heap storage. `compacttype_new` forbids
  `__slots__`; registered in `_psyco` by `psyco_compact_init()`. Uses
  `pintobject.h` and `psyco_pobject_new()` from `ptypeobject.c`. [read]
- Removed: C in `a880444` (2020-04-12, "Start of Python3"); Py side
  (`kdictproxy.py`, `classes.py`) in `58aff23` (2019-02-14).
- Reason: **stated** — `58aff23` "Compact object is broken! But it needed
  rethinking"; `a880444` "removed mainly because I don't understand it ... It is very
  fast when used". **Inferred**: depended on Py2 `PyString` interning (`K_INTERN`),
  `PyInt` and the removed `ptypeobject.c`.
- Leftovers in HEAD [read]: comment in `ni/vcompiler.h` (near the vinfo
  invariants: "only possible through compactobject attrs"). No code hooks remain.
- Relevance: **(a)**. The most significant design loss: it is the only place Psyco
  carried specialised (virtual/constant/unboxed) representations into heap objects.
  Concept maps onto Py3 split-key dicts / per-type layouts; would need a redesign
  rather than a port.

### Virtual Py2 int — `c/Objects/pintobject.[ch]`

- What it did [read]: `psyco_computed_int` virtual source; `PsycoInt_FROM_LONG()`
  builds a *virtual* `PyIntObject` (array: `ob_type` compile-time, `ob_ival` = the
  vinfo) so integers stay in registers and are boxed only when they escape
  (`compute_int` / `direct_compute_int`). Meta-slots `pint_add/sub/mul/div/mod/pow/
  lshift/rshift/and/or/xor/neg/abs/invert/nonzero` with overflow fall-backs
  (`cimpl_ovf_int_lshift`, `cimpl_int_pow2`), `PsycoIntInt_RichCompare` yielding
  `PsycoBool_FromCondition`, `pint_base2op` (shared with `pboolobject.c`).
- Removed: `a880444`. `dc930b2` (2019-02-15, "Py3 Int > Long initial work") tried a
  shim first; its body [read]: "Don't think I can continue with this approach ... makes
  more sense to try and abstract the Psyco*Type* code ... so it can be slowly
  converted to PY3."
- Reason: **inferred** — Py3 has no `int`/`long` split; `PyInt_*` gone.
- Leftovers [read]: `ni/compat2to3.h` `NiCompatInt_FromLong/AS_LONG/AsLong` map to
  `PyInt_*` names; `ni/Objects/plongobject.c:PsycoLong_FROM_LONG` now boxes
  *immediately* via `psyco_generic_call(PyLong_FromLong)` — no virtual int exists.
  Callers in `pabstract.c`, `pobject.c`, `pycompiler.c`, `pbltinmodule.c` that used
  `PsycoInt_AS_LONG/AsLong/FROM_LONG` were rewritten or deleted in `a880444`.
- Relevance: **(a)** for the virtual-boxing pattern (needs a Py3 equivalent: virtual
  `PyLongObject` for single-digit/`long`-range values, with overflow to real
  `PyLong`), **(b)** for the per-slot translations.

### Virtual range — `c/Objects/prangeobject.[ch]`

- What it did [read]: upstream `PsycoXRange_NEW` (virtual `xrange`,
  `psyco_computed_xrange`) and `PsycoListRange_NEW` (virtual *list* for Py2
  `range()`, `psyco_computed_listrange`, `RANGE_START` slot after `LIST_TOTAL`),
  `prange_length`, `prange_item`; step == 1 only ("Needs implementation of division").
  Wired from `pbltinmodule.c` (`pbuiltin_range`, `prange_new`) and `plistobject.c`.
- History: ported to Py3 `PyRange_Type` in `a880444` (virtual `struct rangeobject`
  with `long start, step, len`, `PsycoRange_NEW`, `psy_rangeobject_init`), then
  deleted in `ee1161b` (2020-04-24, "prangeobject needs fixing", empty body).
- Reason: **stated** only as "needs fixing". **Inferred**: the private struct
  mirrored in the port uses `long` fields, but CPython 3.7 `rangeobject` holds
  `PyObject* start, stop, step, length`, so the field layout was wrong.
- Leftovers [read]: `ni/Python/pbltinmodule.c` still has `cimpl_range`,
  `get_len_of_range()`, `longobj_as_long()`, `parse_range_args()` — now unused (no
  `pbuiltin_range`, no `DEFMETA(range)`); `ni/Objects/pobject.c` still lists
  `&PyRange_Type` among types it treats specially.
- Relevance: **(b)** translation of `rangeobject.c`, plus the virtual-object pattern
  (a). Small, good first restoration once virtual `PyLong` exists.

### Type call / instantiation — `c/Objects/ptypeobject.[ch]`

- What it did [read]: meta-implementations for `PyType_Type.tp_call` (`ptype_call`,
  with `cimpl_call_tp_init`), `PyBaseObject_Type.tp_new`
  (`psyco_pobject_new`), `tp_init` (`pobject_init`), `PyType_GenericNew`,
  `PyType_GenericAlloc` (four `cimpl_alloc_{gc,nongc}_{heap,nonheap}` variants) and
  `slot_tp_init` (`pslot_tp_init`, address obtained by evaluating
  `type('X',(object,),{'__init__':...})` in `psy_typeobject_init`). Purpose
  (per `db8fe4e`, 2006): let Psyco see and inline the call to `__init__` when
  instantiating new-style classes. Also used by `pcompact_new`.
- Removed: `a880444`. Reason: **inferred** — Py3 typeobject changes; not needed for
  the minimal `a + b` milestone.
- Leftovers: none found.
- Relevance: **(b)** — direct translation of `typeobject.c` paths; important for
  performance of OO code.

### `array` module — `c/Modules/parray.c`

- What it did [read]: `psyco_initarray()` registers meta-module `"array"`;
  per-typecode `p_{c,b,BB,h,HH,l,f,d}_getitem/setitem`, `parray_item`,
  `parray_ass_item`, `parray_new`; `sq_length` via `psyco_generic_mut_ob_size`.
- Removed: `a880444`. Reason: **inferred** — Py3 `arraymodule.c` struct and
  typecode descriptor table differ.
- Leftovers: none. Relevance: **(b)**.

### Frame-introspecting builtins — `c/Modules/ppsyco.c`

- What it did [read]: `psyco_initpsyco()` defines meta-versions of
  `_psyco.globals/eval/execfile/locals/vars/dir/input`, which upstream
  `support.py` patched into `__builtin__` so code compiled by Psyco (which has no
  real frame) still got correct `locals()` etc. `psyco_fast_to_locals` builds a
  virtual dict from fast locals unless `MP_FLAGS_CONTROLFLOW`.
- Removed: `58aff23`. Reason: **stated** (general) "unneeded/unused".
- Leftovers [read]: `ni/Python/frames.h` comment "psyco_get_locals ... implemented in
  psyco.c"; `ni/Python/frames.c` still has `psyco_get_globals`/`visit_get_globals`.
- Relevance: **(b)/(e)** — Py2 `execfile`/`input`; Py3 frame model differs.

### Gutted files still present: `pboolobject.c`, `pstringobject.c`

- `ni/Objects/pboolobject.c` [read]: reduced to an empty `psy_boolobject_init()`
  (upstream 110 lines: bool slot meta-impls sharing `pint_base2op`); emptied in
  `a880444`. `pboolobject.h` keeps `PsycoBool_FromCondition`.
- `ni/Objects/pstringobject.c` [read]: 1225 lines, entire body wrapped in
  `#ifdef IS_PY3K #else ... #endif` (since `7441415`, 2019-02-14) and
  `IS_PY3K` is always defined in `ni/compat2to3.h`, so it compiles to nothing; not
  listed in `ni/initialize.h`. Contained virtual chars, `USE_CATSTR` lazy
  concatenation, slicing etc. for Py2 `str`.
- Relevance: **(b)**; the lazy-concatenation/virtual-string ideas are (a).

### `cstruct` — `c/cstruct.[ch]`

- What it did [read]: `PyCStruct_Type`, a PyObject wrapping an arbitrary C struct
  with a destructor and a `cs_key` used for hashing/equality, so C side-data could
  be stored in dicts keyed by another object (`PyCStruct_DictGet`). Used by
  `stats.c` (`PyCodeStats` per code object), `profile.c` (`ceval_events_t`),
  `frames.c` (`PyFrameRuntime`), `ivm/iprocessor.c` (`PyVMStack`).
- Removed: `a880444`. Reason: **inferred** — remaining user (`frames.c`) switched to
  PyCapsule (`frame_runtime_capsule_new` in HEAD `ni/Python/frames.c`) [read].
- Relevance: **(d)**, superseded.

### Profiler and "turbo" selection — `profile.c`, `stats.c`, `timing.h`, `alarm.c` (+ `profiler.py`, `logger.py`)

- What it did [read]:
  - `profile.c`: hooks into CPython's profile/trace callbacks
    (`extended_SetProfile/SetTrace`, `set_ceval_hook`, `call_ceval_hooks`), three
    run-time strategies `psyco_rs_profile`, `psyco_rs_fullcompile`,
    `psyco_rs_nocompile`; `psyco_turbo_frame()` sets a `PyTrace_LINE` hook
    (`turbo_go`) so an *already running* frame switches into compiled code at the
    next line — i.e. on-stack replacement; `psyco_turbo_code`,
    `psyco_turbo_frames` (walk all thread states).
  - `stats.c`: `PyCodeStats` (a cstruct keyed by code object) with `st_charge`
    (decaying usage), `st_mergepoints`, `st_codebuf`, `st_globals`; state machine
    documented in `profile.c` ((NULL,NULL) normal / (NULL,rec) compile next /
    (None,NULL) failed / (codebuf,globals) done); `psyco_stats_top`, `statwrite`
    tunables.
  - `timing.h`: measures "time" as `PyThreadState.tick_counter` (opcode count).
  - `alarm.c`: `_psyco.alarm`, a thread-based timer used by `profiler.py` for
    `memory=`/`time=` limits.
  - `py-support/profiler.py`: `Profiler`, `FullCompiler`, `RunOnly`,
    `ChargeProfiler`, `ActiveProfiler`, `PassiveProfiler`,
    `ActivePassiveProfiler`; wraps `sys.settrace/setprofile` and
    `thread.start_new_thread`. `logger.py`: `psyco.log()` output.
  - User-facing: `psyco.profile()`, `psyco.background()`, `psyco.runonly()`,
    `psyco.full(memory=, time=)`, `psyco.stop()`, `psyco.log()`,
    `_psyco.stattop/statread/statwrite/statdump/getcharge/setcharge`.
- Removed: `58aff23`. Reason: **stated** — "dynamic profiling for code compilation
  ... is not an immediate goal for Ni". **Inferred**: `tick_counter` no longer
  exists in Py3; PEP 523 frame hook (`ni/eval_hook.c`, `37d0f1e`) replaced the
  trace-hook approach.
- HEAD replacement [read]: `ni/eval_hook.c:NiEval_EvalFrame` compiles every non-module
  frame on first call (no charge/threshold), caching in `co_to_mp` /
  `co_to_entry_point` dicts created in `ni/ni.c:PyInit_ni`. OSR entry is gone:
  `PsycoObject_FromFrame` (upstream `c/Python/frames.c`) no longer exists.
- Leftovers [read]: none of the stats API; `ni/Python/frames.c:psyco_threadstate_getframe`
  remains with its install line commented out.
- Relevance: **(a)** — compile-selection policy and OSR are core JIT design (not
  CPython translation). Restoration hard: needs counters on code objects (e.g.
  `co_extra`) and a Py3 OSR path.

### `_psyco` module — `c/psyco.c`

- What it did [read]: `init_psyco()` and `PsycoMethods`: `proxycode`, `unproxycode`,
  `getframe`, the `ppsyco` builtins, `profiling`, `turbo_frame`, `turbo_code`,
  stat*/charge functions, `memory`, `cannotcompile`, `setfilter`, `finalizer`,
  `dumpcodebuf`; constants `PYVER`, `PSYVER`, `ALL_CHECKS`, `VERBOSE_LEVEL`,
  `MEASURE_ALL_THREADS`, `PROCESSOR`. Also debugging: `psyco_trace_execution`,
  `vinfo_dump`, `po_dump`, `Psyco_dumpcodebuf`.
- Removed: `58aff23`, replaced by `ni/ni.c` (module `ni`, empty method table,
  exports only `co_to_mp`, `co_to_entry_point`, `error`; `psyco_thread_dict`,
  `psyco_out_of_memory`, `psyco_fatal_error` carried over) [read]. Debug parts
  removed earlier in `5db1615`/`d763a87`/`4118b8b`/`63ab501`/`a985a43` ("use
  nianalyser").
- Leftovers [read]: `ni/psyfunc.h:is_proxycode` + comment "Use _psyco.proxycode()",
  used in `ni/psyfunc.c`; `ni/Python/frames.c` comments on `psyco_proxycode()`;
  `ni/initialize.h` comments "used internally by psyco.c"; `ni/dispatcher.c:psyco_top_array_count`
  "for psyco.c"; `ni/Python/pyver.h` `PSYCO_VERSION_HEX 0x010600f0`.
- Relevance: **(d)**; proxy/bind is superseded by the PEP 523 hook.

### Python package `psyco` — `py-support/`

- Files [read]: `__init__.py` (exports `full, profile, background, runonly, stop,
  cannotcompile, log, bind, unbind, proxy, unproxy, dumpcodebuf, setfilter,
  compact, compacttype`), `core.py` (those functions; `bind` rewrites
  `func_code` to a proxy, tunables `default_watermark=0.09`, `halflife=0.5` s,
  poll freqs), `profiler.py`, `logger.py`, `classes.py`, `kdictproxy.py`,
  `support.py` (version info, `warning`, emulated `sys._getframe` via
  `PythonFrame`/`PsycoFrame`, `patch()` of builtins).
- History: moved to `psyco/` (`52eff9f`), `ni/psyco/` (`56d4fa0`), deleted with
  `ni/__init__.py`/`ni/__main__.py` (Ni wrapper whose `engage()` called
  `psyco.full()`) in `58aff23`.
- Reason: **stated** (general cleanup); **inferred** Py2-only code.
- Relevance: **(d)**, with `support.py` frame emulation relevant to correctness
  of `sys._getframe` under JIT (a).

### ivm backend — `c/ivm/**`

- What it did [read]: portable "virtual machine" backend for non-i386 hosts:
  `iencoding.[ch]`, `idispatcher.[ch]`, `iprocessor.c` (`PyVMStack` cstruct),
  `ipyencoding.[ch]`, `ivm-insns.[ch]`. Instruction set and interpreter are
  generated by SWI-Prolog (`prolog/vm.pl`, `vmwriter.pl`, `ccode.pl`,
  `optimize.pl`, `detcheck.pl`, `insns.pl`, `insnset.py`) into
  `insns-switch.i`, `insns-threaded.i`, `insns-igen*.i`, `insns-table.py`;
  `mode_combine.pl` lists superinstructions, derivable from profiles via
  `py-utils/ivmoptimize.py`/`ivmextract.py` on `psyco.dump`. `localsetup.py`
  copies `.default` generated files.
- Ni history [read]: `54a16e9` (2016-01-20) first 64-bit port *used ivm*, with
  hand-hacked generated files ("The generated code isn't very good"); `ab718d8`
  "Set x86_64 to use ivm for now"; superseded by native x64 from `4b45ec8`;
  deleted in `892d5a6` (2019-02-09).
- Leftovers [read]: `ni/codegen.c` `#error "this was meant for ivm, but now ivm also
  defines HAVE_CCREG."` and a comment "should never occur with the ivm backend";
  `ni/Python/frames.c` comment "with ivm the end pointer's value is ...".
- Relevance: **(c)**. Low value for an x64 target; the Prolog generator is an
  interesting precedent for a generated backend.

### i386 backend — `c/i386/*`

- Copied byte-for-byte to `c/x64/` in `4b45ec8` (2018-01-18, "Initial moving
  things about") [read: zero diff lines for `iencoding.h`, `idispatcher.c`,
  `iprocessor.c`, `ipyencoding.h`], then evolved; i386 deleted in `892d5a6`.
  Also added and deleted in the same span: `c/dummybackend/*` (Ni stub backend,
  `4b45ec8` → `892d5a6`).
- Leftovers: i386 comment in `ni/Python/frames.c`; x64 files keep i386 structure.
- Relevance: **(c)**; upstream i386 is the reference for what x64 changes meant.

### Build and source-list tooling

- `c/files.py` [read]: canonical list of sources and their init functions
  (`Source`, `Object('x')` → `Objects/px.c`/`psy_x_init`, `Module`) used to
  regenerate `initialize.h` and the MANIFEST; `c/depend.py`/`mkincl.py` produced
  `dyn-Makefile`; `c/Makefile` (deleted `05f85f4`); `c/versionchecker.py`
  (hex ↔ version string); `c/win32/psyco.vcproj` (deleted `892d5a6`).
- Removed: `58aff23` (except as noted). Build is now `setup.py` globbing
  `ni/*.c`, `ni/{Python,Modules,Objects}/*.c`, `ni/x64/*.c` [read].
- Leftovers [read]: `ni/initialize.h` is now hand-maintained (no generator);
  `NI_ALL_STATIC` path in `setup.py` still `#include`s every `.c` via it.
- Relevance: **(d)**; a `files.py`-style registry is a natural output of the
  planned meta-compiler.

### Opcode coverage checker — `c/mychecker.py`

- [read] Regex-scans `pycompiler.c` for `case OPCODE:`, `BINARY_OPCODE(...)`,
  `MISSING_OPCODE(...)` and compares with `dis.opname`. Removed `58aff23`.
  HEAD has `tools/gen_opcodes_h.py` instead (Ni's `opcodes.h` was itself deleted
  in `a880444`).
- Relevance: **(d)**, directly useful to a meta-compiler's validation step.

### Memory debugging — `c/linuxmemchk.[ch]`

- [read] `HEAVY_MEM_CHECK` replaced `PyMem_MALLOC/REALLOC/FREE` with
  electric-fence style `memchk_ef_*`. Removed `58aff23`. No leftovers.
  Relevance: **(d)**; ASan/Valgrind cover it.

### Code-buffer inspection tools — `py-utils/`

- [read] `xam.py` (objdump-based disassembly of dumped code buffers),
  `httpxam.py` (web browser of code buffers + `vinfo` trees, needs
  `psyco.dumpcodebuf()` dumps), `vinfo_dump.py`, `symbols.py`
  (address → symbol from `psyco.dump`), `xamsupport.c`, `ivmdump.py`,
  `ivmextract.py`, `ivmoptimize.py`, `manifest.py`, `cvs.py`, `win32/README.TXT`.
- Removed: `012b48f` (2018-07-22). Reason: **inferred** — replaced by the gdb
  breakpoint-based `tools/nianalyser` (commit title "pointbreak based analyser").
- Relevance: **(d)**. The `vinfo` tree viewer has no HEAD equivalent.

### Upstream test suite — `test/`

- [read] `bpnn.py`, `pystone.py`, `pystone-jit.py`, `life*.py`, `test1/3/5.py`,
  `test_base.py`, `test_misc.py`, `test_compactobject.py`, `fulltester.py`,
  `regrtester*.py`, `classicregrtest.py`, `btrun.py`, `file1-basetests`,
  `file2-basetests.uu`. Deleted in `52eff9f` (2016-01-20). Ni then added a CPython
  regression pack (`6a9f8d6`) and removed it in `f1fdf76` (2018-06-26, "Remove old
  tests"). HEAD tests are `tools/test_*` harnesses for asm/encoding.
- Relevance: **(d)**; `test_base.py`/`test_misc.py` encode semantic edge cases
  worth porting.

### Docs

- `doc/Makefile`, `doc/psycoguide.tex`, `doc/README.txt` moved (not deleted) to
  `doc/psyco_legacy/` in `6a376a6`; upstream `README.txt` deleted `52eff9f`. [read]

## Remote branches

Merge-base and counts are against local `master` (= `e1fd808`). [read]

| branch | head | ahead/behind | content |
|---|---|---|---|
| `origin/POC_knee_deep_in_Py3` | `3de9063` 2019-02-15 | 9 / 26 | Pre-rebase duplicates of master `e5b0f73`..`9ee2f56` (same messages and timestamps; `git diff origin/POC_knee_deep_in_Py3 9ee2f56` is empty). No unique content. |
| `origin/gdb_support` | `92a5985` 2017-01-10 | 4 / 86 (base `a62789d`, 2016) | **Unmerged.** `606c70e`: `ni/__init__.py` exports `psyco.proxy`, `psyco.bind`, adds `timeit_iter`. `e693ba9`: indent fix in `c/profile.c`. `08f0427`: `EBP_IS_RESERVED 1` in `c/i386/iencoding.h` so GDB can unwind. `92a5985` ("INCOMPLETE ... from Feb 2016"): `c/debug_info/debug_info.[ch]` (records inline/normal function starts, line numbers, stack depth per address during compilation), hooks in `pycompiler.c`/`vcompiler.[ch]`/`codemanager.c`, a GDB JIT reader plugin `gdb_jit_reader/src/ni_gdb_jit_reader.c` (needs the author's patched GDB), plus a committed `.so`, `ni_symbol.dump` and a notebook. Targets i386/Py2 layout; re-adds nothing removed, but the debug-info design is relevant to symbolising JIT code (perf/GDB). |
| `origin/POC_x64_hack_and_slash` | `3b0335e` 2018-06-26 | 0 / 54 | Fully merged. |
| `origin/WIP_tests_and_fixes` | `06e5dbc` 2018-06-30 | 0 / 45 | Fully merged. |
| `origin/nianalyser` | `ea5c6ef` 2019-01-21 | 0 / 41 | Fully merged. |
| `origin/python3` | `04826fb` 2020-04-15 | 0 / 13 | Fully merged. |
