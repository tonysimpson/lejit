---
written: 2026-11-17
head_ref: 3cd02f7 (measurements); 819da55 (option 4 design)
status: options 1 and 2 implemented (D39); option 4 implemented (D38, D47; sections "Option 4: results" and "Shrinking the store sites")
---
# Instance attribute stores: measurements and options

Loads of instance attributes are inline since D31. Stores still go through C. This note measures what stores cost, sorts them by what they do to the instance dict, and lists ways to make them cheaper. The decision is the owner's.

Tags: **[read]** checked in code, **[run]** measured, **[inferred]** reasoned.

## Summary

- A store costs about 10.5 ns under Ni [run]. Inline, it would cost an estimated 1.5–2 ns [inferred].
- Stores are roughly 30–38% of richards, 13% of deltablue, 11% of go, and 10–15% of raytrace and chaos (the last two mostly in constructors). [run], [inferred]
- Each benchmark has one main kind of store: richards and deltablue overwrite existing keys of a shared-key (split) dict, go mostly overwrites keys of combined dicts (70%; the rest split), raytrace and chaos fill new objects in `__init__`. Growing a class's shared keys never happens in the timed loop. [run]
- An inline store into an existing slot would save about 7.5 ns per store: about 22% of richards and 8% of deltablue. Extending it to fill new objects in order adds about 4% to raytrace and chaos, but only with a guard that doesn't hold the keys (option 4). [inferred]
- Found on the way: compiled stores ignore a `__setattr__` added to the class after compiling (KNOWN_BUGS F37). Every option below except "do nothing" fixes it with the type guard. [run]
- **Done:** options 1 and 2 (`e86f8ae`, D39): richards −13%, go −8% instructions per iteration; see Results. Fixes F37.
- **Done:** option 4 (`80a60ce`, D38, D47): a further richards −31%, deltablue −10%, chaos −5.6%, go −5.0%, raytrace −4.1%, pyflate −3.5% instructions per iteration, about twice the estimates. Code grew much more than estimated on richards (+85%, estimate +20–25%): a failed check compiles its own copy of the rest of the code (see "Option 4: results"). [run]
- **Done:** the store sites shrunk (`506a916`, `0590a3c`, `bd5361a`, D47; "Shrinking the store sites"): with D44's joins, richards' code is +10% over the baseline's 116 KB (budget +20–25%), at 161 M instructions per iteration against option 4's 177 M; all 29 benchmarks 0.8% smaller than base. [run]

## The path today

[read], `pobject.c:PsycoObject_SetAttr` at `3cd02f7`:

- The type is promoted (`Psyco_NeedType`). The name is a compile-time `str`, so it is interned and the code calls the type's `tp_setattro` directly. For ordinary classes that is `PyObject_GenericSetAttr(obj, name, value)`, a C call with no guard.
- `tp_setattro` is read at compile time. A heap type's slot changes when the class gains `__setattr__`, and the compiled code then calls the old slot. This is F37. `PsycoObject_GetAttr` avoids the same trap by testing `tp_getattro` only after A9's guard.
- `DELETE_ATTR` takes the same path with `NULL`.

What CPython 3.7 then does for each store [read] (`Objects/object.c:_PyObject_GenericSetAttrWithDict`, `Objects/dictobject.c`):

1. `_PyType_Lookup(tp, name)` (method cache). A descriptor whose type has `tp_descr_set` (a data descriptor: `property`, `__slots__` members) wins and handles the store.
2. `_PyObject_GetDictPtr`, then `_PyObjectDict_SetItem(tp, dictptr, name, value)`:
   - No dict yet and the class has cached keys: `new_dict_with_shared_keys` makes a split dict on them.
   - Otherwise `PyDict_SetItem`, then the re-share rule: if the dict used the class's keys before the store and not after (the store resized it), the class gets the dict's new keys only if `dk_refcnt == 1` (`make_keys_shared`); otherwise the class stops sharing keys (F36).
3. `insertdict`:
   - `MAINTAIN_TRACKING`: an untracked dict becomes GC-tracked if the key or value may be tracked.
   - The lookup goes through `dk_lookup` (`lookdict_split` for split dicts).
   - **Order rule.** A split dict converts to combined (`insertion_resize`) if it stores a key whose value is still `NULL` at index `ix != ma_used`, or a new key while `ma_used != dk_nentries`. So a split dict can only fill the shared keys in their order.
   - A new key in a split dict appends to the *shared* keys object (`dk_nentries++`, `dk_usable--`), which every dict on those keys sees.
   - An existing key: `values[ix] = value`; `ma_used++` if the slot was empty.
   - Every modification sets `ma_version_tag = DICT_NEXT_VERSION()`, a process-wide counter, then `Py_XDECREF(old_value)`, which can run `__del__`.
4. Deleting from a split dict always converts it to combined, and `_PyObjectDict_SetItem` then drops the class's cached keys.

Two facts that constrain inline code [read]:

- `pydict_global_version` is `static` in `dictobject.c`. It is in libpython's full symbol table here (`nm`, local `b`), not in the dynamic one, so Ni can't reach it with `dlsym`. Ni relies on the tags: `pycompiler.c`'s cached `LOAD_GLOBAL` (D25) compares `ma_version_tag` of globals and builtins. Any dict can be a function's globals, so an inline store must change the tag too.
- In 3.7 nothing else in CPython reads `ma_version_tag` except `_testcapi`. [read] (grep of `Objects`, `Python`, `Modules`)

## Measurements

### Where the time goes

Method: `perf record -m 16 -F 4999 -g` on `tools/profile_bench.py <bench> 4`, release build at `3cd02f7`, self time. Others were benchmarking at the same time, so only shares are used. The `-g` call chains end at libpython (no frame pointers), and `--call-graph dwarf` resolved only about 10% of chains, so time is attributed by function. [run]

| self time, % of samples | richards | deltablue | raytrace | go | chaos |
|---|---|---|---|---|---|
| store-only: `_PyObject_GenericSetAttrWithDict`, `PyObject_GenericSetAttr`, `_PyObjectDict_SetItem` | 9.2 | 3.9 | 2.5 | 3.0 | 2.7 |
| `insertdict` | 5.6 | 3.2 | 2.8 | 4.1 | 2.8 |
| `lookdict_split` | 15.1 | 5.9 | 5.0 | 1.3 | 3.7 |
| `PyDict_SetItem` | 2.5 | 1.0 | 0.9 | 1.3 | 0.5 |
| `_PyType_Lookup` | 4.7 | 6.6 | 2.5 | 1.8 | 2.9 |
| `lookdict_unicode_nodummy` (combined dicts) | 0.0 | 6.5 | 4.1 | 28.5 | 5.0 |

- The shared rows have other callers. In richards they are almost all stores: the loads' fallback (`_PyDict_GetItem_KnownHash`, `cimpl_dict_get_known`) is 0.1%, and the dwarf chains that resolved name a store 147 times and a load 5 times. So stores are about 38% of richards' samples (the profile includes the three compiling iterations). [run]
- In deltablue `_PyType_Lookup` is also class-attribute loads (`type_getattro`, 3.1%). In go `lookdict_unicode_nodummy` is mostly loads from unshared dicts (the dwarf chains: 79 from `_PyDict_GetItem_KnownHash`, 15 from stores). [run]
- Aside: in raytrace and chaos some `insertdict` and `dictresize` time comes from `frames.c:psyco_frame_start_register`, which records every entry into compiled code from the interpreter in a per-thread dict. That is a dict store, not an attribute store, and is outside this note. [run] (dwarf chains; weak evidence)

### Cost of one store

`tools/store_micro.py`, two runs each, same session. Loops of four stores minus an empty loop. [run]

| | Ni | plain CPython |
|---|---|---|
| existing key, split dict | 10.2–11.6 ns | 13.1–13.7 ns |
| existing key, combined dict | 10.3–11.4 ns | 12.8–19.0 ns |
| `C()` then three stores, minus `C()` (creates the dict, fills it, frees it) | 48–49 ns | 49–51 ns |

Under `perf`, the split loop's samples are: `insertdict` 27.7%, `_PyObject_GenericSetAttrWithDict` 17.4%, `lookdict_split` 12.7%, `_PyType_Lookup` 10.4%, `_PyObjectDict_SetItem` 9.9%, `PyDict_SetItem` 4.7%, `_PyObject_GetDictPtr` 1.7%, `PyObject_GenericSetAttr` 0.9%; emitted code 3.6%. [run]

Cross-check: richards makes 371,567 stores per iteration and an iteration takes about 12.4 ms under Ni, so at 10.5 ns stores are 31% of it. That agrees with the 38% above, which also counts compiling. [inferred]

### What the stores do

`tools/storeclasses.py <bench>` runs one iteration of a vendored benchmark under plain CPython 3.7 with opcode tracing and classifies each `STORE_ATTR` before it runs (the owner is read from the value stack with ctypes). One untraced iteration runs first. [run]

| % of stores | richards | deltablue | raytrace | go | chaos |
|---|---|---|---|---|---|
| stores per iteration | 371,567 | 21,506 | 1,601,798 | 776,489 | 220,066 |
| split, existing key, value set (A) | 99.97 | 84.0 | 1.9 | 29.8 | 4.6 |
| split, key in shared keys, slot empty, next in order (B) | 0.02 | 13.2 | 63.4 | 0.3 | 63.6 |
| creates the dict (shared keys) (G) | 0.01 | 2.9 | 34.7 | 0.1 | 31.8 |
| combined, existing key (E) | – | – | – | 69.6 | – |
| combined, new key | – | – | – | 0.2 | – |
| split, new key appended to the shared keys (C) | 0 | 0 | 0 | 0 | 0 |
| split, out of order or no room (converts to combined) | 0 | 0 | 0 | 0 | 0 |
| data descriptors, `__slots__`, `__setattr__`, class attributes | 0 | 0 | 0 | 0 | 0 |

- Every site sees one kind of store at least 97.6% of the time (100% in four benchmarks). [run]
- Every store to a split dict used the class's current keys. [run]
- Some richards sites see three or four classes (`runTask`, `running`); Ni promotes the type, so each class gets its own path. [read] (`Psyco_NeedType`)
- B and G are constructors: raytrace's `Vector.__init__` and `Ray.__init__`, chaos's `GVector.__init__`, deltablue's `__init__`s. [run]
- go's combined stores are `Square`'s, whose class loses its shared keys in plain CPython too (F36). Under Ni `Board` also loses them (F36), so its stores (2.8% of go's) become combined. The tracer can't run under Ni, so the table is plain CPython's. [inferred]

Static `STORE_ATTR` sites: richards 70, deltablue 65, raytrace 36, go 60, chaos 22, against 101, 273, 239, 225 and 177 `LOAD_ATTR` and `LOAD_METHOD` sites. [run] (`dis` over the benchmark files)

## Rules any faster store must keep

These apply to every option except "do nothing".

1. **Type guard.** Use A9's guard (`pobject.c:type_changed`, a per-site `attr_guard_t`): `tp_flags` and `tp_version_tag` as compiled. After it, at compile time: `tp_setattro == PyObject_GenericSetAttr` (fixes F37), `tp_dictoffset > 0`, and the name's class attribute (`_PyType_Lookup`) is either absent or an instance of a static type without `tp_descr_set`. A class attribute whose type is a heap type must take the generic path: that type can gain `__set__` without changing the class's tag. A data descriptor (`__slots__`, `property`) takes the generic path (none in these benchmarks). [read], [inferred]
2. **Respawn determinism (D21, D31).** The emitted conditions must not depend on state a replay may see differently. As for loads: the expected values live in a per-site struct read at run time, a replay uses a static struct that only trashed code reads, and the block ends after the site (`split_after_global`). Every run-time condition (guard, dict present, keys, old value, `ma_used`, GC tracking) comes before anything that can run Python code (the decref of the old value). Then a respawn from any of them sees the type as the guard just saw it. [inferred]
3. **`ma_version_tag` changes on every modification.** Ni can't call `DICT_NEXT_VERSION()`. Choices: (a) Ni's own counter starting at 2^63: tags stay unique and never repeat, and the only readers (Ni's own guards) compare for equality; (b) find `pydict_global_version` in libpython's `.symtab` at start-up, exact but it breaks on a stripped libpython; (c) a C call, which defeats the point. (a) costs one load, one increment and two stores. [read], [inferred]
4. **Refcounts.** Incref the new value, store, set the tag, then decref the old value, in CPython's order. The decref can run `__del__` and must be last. The key's reference doesn't change. [read]
5. **GC tracking.** If the dict is untracked (`gc_refs >> 1 == -2`: in 3.7 the low bit is the finalised flag, and `gc_refs` sits at `dict - 16` in the 32-byte `PyGC_Head`) and the value may be tracked, the dict must be tracked. The collector untracks dicts that hold only atomic values (`_PyDict_MaybeUntrack`), so this happens in practice. When the value's type is known at compile time and is atomic (`int`, `float`, `str`, `None`, `bool`), skip the check; otherwise test and go to the helper. A missed track only leaks cycles; it frees nothing live. [read], [inferred]
6. **Order rule and `ma_used`.** Only a store to an existing value, or to the empty slot at `ix == ma_used` (then `ma_used++`), may stay split without touching the keys. Anything else goes to the helper, which converts or appends as CPython does. [read]
7. **Keys and F36.** Inline code never changes a keys object, so it never needs the re-share rule. A guard that holds a reference to the keys (D31) inherits F36; one that doesn't hold a reference doesn't (option 4). [inferred]
8. **Deletes** stay on the generic path: they always convert the dict and drop the class's keys. [read]

## Options

Gains are estimates: stores per iteration × saving per store ÷ Ni's warm iteration time (richards 12.4 ms, deltablue 1.67 ms, raytrace 190 ms, go 71 ms, chaos 26.5 ms, from the profiling runs). Code size growth is scaled from D31 by the ratio of store to load sites and is rough. [inferred]

### 0. Do nothing

Stores stay a C call. Richards keeps about a third of its time in them. F37 still needs its own fix: test `tp_setattro` behind a type guard, which is most of option 1 anyway.

### 1. Type guard, then call `_PyObjectDict_SetItem` directly

Emitted code: A9's type guard; on success a call to a small Ni helper with `(obj, tp, name, value)`, which does `_PyObjectDict_SetItem(tp, (PyObject **)((char *)obj + tp_dictoffset), name, value)` and turns a `KeyError` into `AttributeError` as `_PyObject_GenericSetAttrWithDict` does. On guard failure, `PyObject_SetAttr` (it re-reads `tp_setattro`).

- Skips `PyObject_GenericSetAttr`, `_PyObject_GenericSetAttrWithDict`, `_PyType_Lookup` and `_PyObject_GetDictPtr`: about 31% of the micro loop, so about 3 ns, minus about 0.5 ns for the guard. Every instance store benefits, including dict creation, combined dicts and appends. [inferred]
- Keeps every dict invariant, because CPython's own `_PyObjectDict_SetItem` does the work. Only rule 1 (and F37) is new. Exported: `T _PyObjectDict_SetItem` in libpython's dynamic symbols. [run] (`nm -D`)
- Gain: richards 8%, deltablue 3.5%, raytrace 2.3%, go 3%, chaos 2.3%. [inferred]
- Cost: one guard per site (about 30 bytes) and an extra argument. Code a few % larger; compile time unchanged in practice. [inferred]
- Risks: low. A class attribute of a heap type wrongly accepted (rule 1) would skip its `__set__`.
- Tests: class gains `__setattr__` after compiling (F37); class gains a property of the stored name; a class attribute that is an instance of a user class which later gains `__set__`; `__slots__` member; store to an object without `__dict__` (AttributeError text); `o.__dict__` replaced by a dict subclass; a store that creates the dict; the first instance growing the class's keys (re-share kept).

### 2. Known-hash store for combined dicts

Emitted code, after option 1's guard: load the dict at `tp_dictoffset`; if non-NULL and `ma_values == NULL` (combined), call `_PyDict_SetItem_KnownHash(dict, name, value, hash)` with the hash computed at compile time; else option 1's helper.

- For a combined dict `_PyObjectDict_SetItem` does exactly `PyDict_SetItem` on it, so this is the same store. [read]
- Skips also `_PyObjectDict_SetItem` and `PyDict_SetItem`: about 4 ns of 10.5. [inferred]
- Gain: go about 3% against today (541,000 stores × 4 ns ≈ 2.2 ms of 71 ms), about 1% more than option 1 alone; nothing elsewhere. [inferred]
- Cost: two loads and a test per site; small.
- Risks: low. Must not be used for split dicts: it would skip the re-share rule.
- Tests: combined dict (after a `del`), dict replaced by `o.__dict__ = {...}`, a dict with a non-`str` key added through `__dict__`, a key whose `__eq__` raises.

### 3. Inline store into an existing shared-key slot, behind D31's keys guard

Emitted code, after option 1's guard, when `shared_key_slot` finds the name in the class's keys at index `ix`:

```
mov  rD, [obj + tp_dictoffset]     ; dict
test rD, rD ; jz helper
mov  rK, [rD + ma_keys]
cmp  rK, [guard.keys] ; jne helper        ; D31's keys_guard_t
mov  rV, [rD + ma_values]
mov  rO, [rV + 8*ix]               ; old value
test rO, rO ; jz helper                   ; (option 4 handles the empty slot)
mov  rX, [rD - 16] ; sar rX, 1 ; cmp rX, -2 ; je helper   ; untracked dict; only if the value may be tracked
inc  qword [value]                 ; if the value isn't already owned
mov  [rV + 8*ix], value
mov  rT, [ni_dict_version] ; inc rT ; mov [ni_dict_version], rT
mov  [rD + ma_version_tag], rT
dec  qword [rO] ; jz dealloc       ; last: may run __del__
```

- The helper is option 1 (or 2).
- Keeps rules 1–8 as above. Holds the keys like D31, so it adds no new F36 case, but inherits it.
- A name absent from the keys at compile time can't be inline: a site compiled before the class's keys hold the name always calls the helper. Ni compiles a function on its first call, so `__init__` is usually compiled while its class's keys are still empty. In practice this option covers kind A only. [read] (`BUILD.md`, `shared_key_slot`), [inferred]
- Saving: about 9 ns of C, minus 1.5–2 ns inline: about 7.5 ns per store. [inferred]
- Gain: richards 22%, deltablue 8%, go 2.4%, raytrace and chaos under 0.5%. [inferred]
- Cost: about 60–70 bytes per site plus the helper call. Scaled from D31 (richards 150 → 200 KB for loads, with 0.7 store sites per load site): richards maybe +25% code, deltablue +9%, the others +2–4%. Compile time up perhaps 5–10% (D31 added 12% to richards). [inferred]
- Risks: the version-tag scheme (rule 3); a wrong GC check (leaks only); a decref placed before a condition (breaks respawn determinism).
- Tests: everything under option 1, plus: `o.x = o.x` (same object); old value freed by the store (and with a `__del__` that changes the class); dict untracked by `gc.collect()` then given a list (`gc.is_tracked(o.__dict__)` must match CPython); a function whose globals are an instance's `__dict__`, with a cached `LOAD_GLOBAL` after an inline store (the tag must change); `o.__dict__` replaced; instances on old keys after the class re-shared; a respawn from each condition (`ALL_CHECKS` replay check).

### 4. Inline store behind a run-time cache that holds no keys

The same store as option 3, but the guard checks the live keys instead of a held pointer. Per site, a cache that the helper fills on a miss:

```
cache: { dk_size, koff = byte offset of entries[ix].me_key, ix }

mov  rD, [obj + tp_dictoffset] ; test ; jz helper
mov  rV, [rD + ma_values] ; test rV, rV ; jz helper    ; combined dicts first
mov  rK, [rD + ma_keys]
mov  rX, [cache.dk_size] ; cmp [rK + dk_size], rX ; jne helper
mov  rX, [cache.koff]    ; cmp [rK + rX], name ; jne helper
mov  rI, [cache.ix]
mov  rO, [rV + 8*rI]
test rO, rO ; jz fill_in_order
... store as option 3 ...
fill_in_order:
cmp  rI, [rD + ma_used] ; jne helper
... store, then inc qword [rD + ma_used] ...
```

- Sound without a reference: the dict holds its keys, so they are alive while checked; equal `dk_size` fixes the entries' offset; `me_key == name` at that entry proves `ix` is the name's index, since shared keys only append and `new_keys_object` zeroes unused entries. The name is an interned `str` from `co_names`. [read] (`new_keys_object`, `insertdict`), [inferred]
- The cache is filled at run time, so it works for `__init__` compiled with empty keys, and for classes whose keys changed after compiling. Emitted code depends on nothing but constants (type, `tp_dictoffset`, the cache's address), so respawns are simple. No F36 effect. [inferred]
- Covers A and B. The store that creates the dict (G) stays on option 1's helper; the helper could fill the cache from the new dict so the next stores are inline.
- Saving: about 7 ns per store (two more loads than option 3). [inferred]
- Gain: richards 21%, deltablue 9%, raytrace 3.9% (A and B: about 1,045,000 stores) plus 0.8% from option 1 on dict creation, chaos 4% plus 0.7%, go 2.3%. [inferred]
- Cost: about 80–90 bytes per site, slightly more than option 3. Compile time as option 3. [inferred]
- Risks: as option 3, plus cache thrash when one class's instances use two key sets (old keys after a re-share). The fill could stop after a few misses.
- Tests: option 3's, plus: `__init__` compiled on the first instance (stores must become inline from the second; count helper calls); attributes set in a different order in one instance (converts to combined: must go to the helper); two key sets for one class; a dict whose keys are resized between calls.
- The same check would also serve loads of names in the keys, and could replace D31's held reference for them. Absent names still need D31's guard. [inferred]

### 5. Append the next key to the shared keys inline

Kind C: no store in the five benchmarks does it after the first iteration. [run] It writes the shared keys object (index slot by probing, entry, `dk_usable`, `dk_nentries`) and interacts with F36. Not worth doing.

### 6. Create the dict inline

Kind G is 35% of raytrace's and 32% of chaos's stores. The allocation (`new_dict_with_shared_keys`, `new_values`) is static in CPython, so inline creation would copy CPython's free lists. `PyObject_GenericGetDict` (exported) creates the dict, and option 1 already removes the lookup overhead. The rest is allocation, which inline code can't avoid. Not worth doing beyond option 1. [read], [inferred]

## Gains side by side

Percent of warm iteration time saved, estimated. [inferred]

| option | richards | deltablue | raytrace | go | chaos |
|---|---|---|---|---|---|
| 1. guard + `_PyObjectDict_SetItem` | 8 | 3.5 | 2.3 | 3 | 2.3 |
| 1 + 2. known hash for combined | 8 | 3.5 | 2.3 | 4 | 2.3 |
| 1 + 3. inline, D31 keys guard (A) | 22 | 8.5 | 2.4 | 4.5 | 2.5 |
| 1 + 4. inline, run-time cache (A, B) | 21 | 9 | 4.7 | 4.4 | 4.7 |
| 1 + 2 + 4 | 21 | 9 | 4.7 | 5.3 | 4.7 |

Option 1 is assumed to cover what the others leave to the helper. go's figures ignore the extra combined stores F36 causes under Ni.

## Recommendation

For the owner to decide.

- Option 1 is cheap, safe and fixes F37. It is also the fallback every inline option needs, so it would come first whichever way.
- Most of the gain is in an inline store to an existing slot. Option 3 is the smallest step from D31. Option 4 costs slightly more per site but also speeds up constructors and avoids F36, and its check could later replace D31's held reference for loads. If one inline option is chosen, option 4 looks the better long-term choice.
- Option 2 helps only go, a little.
- Options 5 and 6 aren't worth doing on these measurements.
- Before choosing between 3 and 4, it would be worth settling rule 3 (the version-tag counter), which both need: a recorded decision would do.

## Option 4: design

Written 2026-11-17 at `819da55`. It builds on option 1's type guard and helper, and keeps D21 (replay determinism), D29 (compile speed), D31 (`keys_inline`) and D35 (Ni's version counter). Implemented in `80a60ce`, with the additions in "Option 4: results".

### Scope

- **Inline:** kind A (overwrite a set value of a split dict) and kind B (fill the empty slot at `ix == ma_used`, the constructor case). B is worth having: it is 63% of raytrace's stores, 64% of chaos's and 11% of deltablue's, and every B store in the simulation below hits. [run]
- **Not inline:** creating the dict (G), appending a key to the shared keys (C: no store in the five benchmarks does it after warm-up [run]), combined dicts, deletes. These take option 1's helper, or option 2's known-hash call for combined dicts (see "Miss paths").

### The cache

One cache per (class, name), not per site. It holds no reference to any keys object, and its contents affect neither soundness nor the emitted code.

```
typedef struct {
  Py_ssize_t dk_size;   /* keys size this entry was computed for; 0 = empty */
  Py_ssize_t koff;      /* byte offset of entries[ix].me_key in keys of that size */
  Py_ssize_t ix;        /* the name's index in such keys */
  PyObject *name;       /* the interned name (owned by the table's key) */
  uint64_t *version;    /* &ni_dict_version (D35), so the code needs no imm64 for it */
} store_cache_t;        /* 40 bytes, PyMem_Malloc'd, never freed */
```

- `koff = 40 + dk_size * ixsize(dk_size) + 24 * ix + 8`, with `ixsize` 1, 2, 4 or 8 as in `new_keys_object`. Any size works, and the emitted code computes no offsets. [read] (`Objects/dictobject.c:new_keys_object`, `DK_ENTRIES`)
- The table is a C dict from `(type address, name)` to the cache's address, kept for the life of the process. At compile time Ni looks a cache up, or makes one and fills it from the class's `ht_cached_keys` if the name is there (else `dk_size = 0`).
- **Why per (class, name) and not per site.** A respawn replays compilation, and a miss branch is compiled during a replay. D31's per-site guard is a static dummy in a replay, so a miss branch compiled then would refresh the dummy, and the real cache would never be refreshed. A table lookup returns the same cache in the original compile and in every replay. The promoted class is held by the compiled code (`need_type_mega`), so its address can't be reused while the code lives. A dead class's address reused by a new class only gives that class a useless hint. [read] (`pobject.c:instance_dict_get`, `generic_getattr`), [inferred]
- Sharing one cache between all sites that store a name on one class is right: they all see the same keys. Two classes at one site are two promoted types, so two caches.

### The check, and why it is sound without a reference

At run time, after option 1's type guard:

1. `D = *(obj + tp_dictoffset)` is non-NULL.
2. `V = D->ma_values` is non-NULL (D is split).
3. `K = D->ma_keys`, and `K->dk_size == c->dk_size`.
4. `*(PyObject **)((char *)K + c->koff) == c->name`.

Then `ix = c->ix` is the name's index in D, and `V[ix]` is its value slot.

Facts it rests on:

- **S1.** A dict is split iff `ma_values != NULL`. Its keys are then either `Py_EMPTY_KEYS` (after `PyDict_Clear`: `dk_size` 1, no entries) or a keys object from `new_keys_object` (`dk_size >= 8`) shared through `ht_cached_keys`, `make_keys_shared` or `PyDict_Copy`'s split copy. Split keys hold only exact `str` keys. [read] (`empty_keys_struct`, `PyDict_Clear`, `insertdict`, `make_keys_shared`)
- **S2.** In a split dict's keys, `entries[i]` for `i < dk_nentries` is never rewritten. Entries are only appended, at `dk_nentries` (`insertdict`, `PyDict_SetDefault`). Every other write to `me_key` (`delitem_common`, `_PyDict_Pop`, `dict_popitem`) first converts a split dict to combined with `dictresize`, which gives that dict new keys. [read] (every `me_key =` in `dictobject.c`)
- **S3.** `new_keys_object` zeroes all `USABLE_FRACTION(size)` entries, so `me_key` is NULL at and past `dk_nentries`. `make_keys_shared` removes dummies first. [read]; [run]: 207 unused entries in 29 classes' keys, all zero.
- **S4.** A split dict's values array has `USABLE_FRACTION(dk_size)` slots, so any `ix < dk_nentries` is in bounds. [read] (`new_dict_with_shared_keys`, `make_keys_shared`, `PyDict_Copy`)
- **S5.** A dict's keys are unique. For an exact `str` key, `lookdict_split` returns the index whose `me_key` is identical (or equal, which an identical key also is). [read]
- **S6.** The frame holds `obj`, `obj` holds D, and D holds K. Between the loads and the writes nothing runs Python code, so all three stay alive and unchanged. [inferred]

Argument:

- **The read in step 4 is in bounds.** Only the miss helper writes a cache. It computes `koff` from a real keys object of size `c->dk_size` with `ix < dk_nentries <= USABLE_FRACTION(size)`. Every keys object of that size has the same layout, so once step 3 has passed, `K + koff` is inside K. The initial `dk_size = 0` matches no keys object (S1). The fields are written together by C code that runs no Python code in between, so no reader sees a half-written cache.
- **Step 4 proves the index.** If `ix >= K->dk_nentries`, the entry is zero (S3) and can't equal the name. So `ix < dk_nentries`, the entry is the name's (S5), and it can't change while D is split (S2).
- **So the store is CPython's.** `insertdict` would find the same `ix` with `old_value = V[ix]`. For A it does `V[ix] = value`, a new tag, then the old value's decref. For B it requires `ix == ma_used` (otherwise it converts the dict), then does `V[ix] = value`, `ma_used++` and a new tag. `_PyObjectDict_SetItem`'s extra steps don't apply: D exists, and the keys don't change, so the re-share rule does nothing. `_PyObject_GenericSetAttrWithDict`'s descriptor lookup is settled at compile time behind the type guard (rule 1). [read]
- **Address reuse can't fool it.** The check never compares a keys address. It reads the live keys through the dict that holds them, so a keys object at a reused address is judged by its own contents. [run]: 2,000 classes were made and dropped, with attributes set in random orders. 1,998 got keys at the address of an earlier class's freed keys, and in 1,341 of those `y` was at a different index. A pointer check would have passed all 1,341; the entry check at the old index passed none.
- **A wrong cache only costs speed.** A stale or wrong cache fails step 3 or 4 and goes to the helper.

A variant checks `dk_size <= 0xff` and `ix < dk_nentries` instead of equal sizes, so keys of other sizes with the name at the same index also hit. It needs one more compare and only helps a class whose instances use keys of two sizes. The simulation saw no size misses, so it isn't worth it. [run]

### Filling the cache

- **At compile time,** only when the cache is made: from `ht_cached_keys` if the name is in it. The emitted code never depends on the contents (D21).
- **At run time,** the miss helper `cimpl_store_attr_miss(obj, name, value, cache)`:
  1. does option 1's store (`_PyObjectDict_SetItem` on `obj`'s dict pointer, with `KeyError` turned into `AttributeError`);
  2. on success, re-reads `*dictptr` (a `__del__` run by the store may have replaced the dict). If the dict is split and the name is in its keys (identity scan of `entries[0 .. dk_nentries)`), it writes `dk_size`, `koff` and `ix`. Otherwise it leaves the cache alone.
- **In `__init__`,** compiled while the class's keys don't yet hold the names: on the first instance every store misses. The first creates the dict, the others append to the shared keys (C), and each refreshes its cache. From the second instance on, the first store still creates the dict (G, helper) and the rest are B hits. [run] (simulation: chaos 140,002 B hits, 70,001 dict creations, no other misses)
- **No thrash limit is needed so far.** The simulation saw no size or entry misses after warm-up in any of the five benchmarks. A class whose instances alternate between two key sets would miss and refresh every time (a scan of a few entries). If that shows up, the helper can stop refreshing after N misses. [run]

### Emitted code

In `PsycoObject_SetAttr`, after option 1's guard and compile-time checks, when `keys_inline(po, tp)` (D31, based on the block serial) is true and `tp_dictoffset > 0`. If `keys_inline` is false (the class has lost its keys), only option 1 or 2 is emitted. `po->pr.split_after_global = 1`, as for every guarded site.

Written as x86-64; registers are illustrative, Ni allocates them. `PyDictObject`: `ma_used` +16, `ma_version_tag` +24, `ma_keys` +32, `ma_values` +40. Keys: `dk_size` +8, `dk_nentries` +32. `PyTypeObject.tp_flags` +168. [read] (`dictobject.h`, `dict-common.h`, `object.h`; the dict offsets `tools/storeclasses.py` already uses)

```
  mov  rD, [obj + tp_dictoffset]        ; 4
  test rD, rD ; jz  miss_nodict         ; 3 + 6   condition 1
  mov  rV, [rD + 40]                    ; 4       ma_values
  test rV, rV ; jz  miss_combined       ; 3 + 6   condition 2
  mov  rK, [rD + 32]                    ; 4       ma_keys
  mov  rC, imm64 cache                  ; 10
  mov  rX, [rK + 8]                     ; 4       dk_size
  cmp  rX, [rC + 0]  ; jne miss         ; 3 + 6   condition 3
  mov  rX, [rC + 8]                     ; 4       koff
  mov  rX, [rK + rX]                    ; 4
  cmp  rX, [rC + 24] ; jne miss         ; 4 + 6   condition 4
  mov  rI, [rC + 16]                    ; 4       ix
  ; GC tracking (rule 5), only if the value's type isn't known to be non-GC:
  mov  rX, [value + 8] ; mov rX, [rX + 168]   ; ob_type->tp_flags
  shr  rX, 14 ; and rX, 1                     ; Py_TPFLAGS_HAVE_GC
  mov  rY, [rD - 16] ; sar rY, 1 ; cmp rY, -2 ; sete ; movzx   ; dict untracked
  and  rX, rY ; jnz miss                ; ~30 bytes  condition 5
  mov  rO, [rV + 8*rI]                  ; 4       old value
  test rO, rO ; jz  fill                ; 3 + 6   condition 6 (B is the rare side)
  ; A: overwrite
  inc  qword [value]                    ; 3       unless Ni hands over a reference it owns
  mov  [rV + 8*rI], value               ; 4
  mov  rP, [rC + 32]                    ; 4       &ni_dict_version
  mov  rT, [rP] ; inc rT ; mov [rP], rT ; 10
  mov  [rD + 24], rT                    ; 4       ma_version_tag (D35)
  dec  qword [rO] ; jz dealloc          ; 3 + 6   last: may run __del__
  ; end of block (split_after_global)

fill:                                   ; B, compiled when first reached
  cmp  rI, [rD + 16] ; jne miss         ;         ma_used; condition 7
  inc  qword [value] ; mov [rV + 8*rI], value
  inc  qword [rD + 16]                  ;         ma_used++
  ; version tag as above; no decref
```

- **Order.** All conditions (1–7) come before the first write. The only thing that can run Python code is the old value's decref, and it is last. So a respawn from any condition replays the same code with the type as the guard saw it (rule 2). The incref, store, tag and decref follow `insertdict`'s order; nothing can observe where the incref sits relative to the checks. [read]
- **`ma_used`** is unchanged for A and incremented for B. **`dk_usable`, `dk_nentries` and the keys** are never written.
- **GC tracking.** The condition is "the value's type has `Py_TPFLAGS_HAVE_GC` and the dict is untracked". It covers `_PyObject_GC_MAY_BE_TRACKED(value)` conservatively: an untracked tuple goes to the helper, which decides exactly. The key is a `str` and never needs tracking. In 3.7 `gc_refs` is at `dict - 16`, and an untracked object's `gc_refs >> 1` is -2 ([run]: -4 untracked, -6 tracked). Compare for equality with -2: during a collection (a `__del__` or weakref callback run by the collector) a tracked object's value can be anything else. When Ni knows the value's type at compile time, the condition is dropped (not GC) or reduced to the untracked test (GC). Testing only "dict untracked" would not do: in raytrace 57% and in chaos 68% of stores go to an untracked dict holding only floats. [read] (`Include/objimpl.h`, `MAINTAIN_TRACKING`), [run]
- **Built from Ni's primitives,** as `plistobject.c`'s list store is: `psyco_get_field` and `psyco_get_field_offset` for the loads, `integer_cmp` and `runtime_condition_f` for each condition, `psyco_vinfo_condition` for the branch-free GC test, `psyco_put_field_array` with `FPYREF` for the store (it increfs or hands over the reference), and `psyco_decref_v` for the old value. New field definitions: `DICT_ma_used`, `DICT_ma_version_tag`, `KEYS_dk_size`, and the cache's five fields (all `FMUT`, so run-time loads).
- **Likely sides.** A is the likely side of condition 6, fixed at compile time (never taken from the cache). B is compiled by a respawn the first time a constructor reaches it, and then runs as straight code.

**Count and size, A path:** about 19 instructions for the checks without the GC test (28 with it), plus 9 for the store: **28–37 instructions on a hit.** About 125 bytes without the GC test and 155 with it, plus a 15-byte pooled stub (D32) for each of the 6 conditions on the A path (90 bytes). B adds about 30 bytes and one stub at sites that reach it. Option 1 alone is the type guard (about 50 bytes), a call with four arguments (about 30) and its stubs. **So option 4 adds about 150–190 bytes per compiled copy of a site,** not the 80–90 estimated in "Options" above. Scaled from D31 (about 500 bytes of growth per static load site in richards, because sites are compiled several times), richards could grow by 20–25%, deltablue by 10–15%, the others by 3–6%. Compile time perhaps +8–12% on richards (D31 added 12%). [inferred]

### Miss paths

Each failed condition is a separate respawned branch that compiles a call and the end of the block.

| condition failed | the branch calls |
|---|---|
| 1. no dict yet | `cimpl_store_attr_miss` (creates the dict, refreshes the cache) |
| 2. combined dict | `_PyDict_SetItem_KnownHash(D, name, value, hash)` (option 2). Sound: a combined dict's keys are never the class's, so `_PyObjectDict_SetItem` does exactly `PyDict_SetItem`. No cache refresh. |
| 3, 4. other keys, or the name not at `ix` | `cimpl_store_attr_miss` |
| 5. untracked dict, GC value | `cimpl_store_attr_miss` (CPython tracks the dict) |
| 7. empty slot out of order | `cimpl_store_attr_miss` (CPython converts the dict) |

All of them return `int` (`CfCommonIntZeroOk`).

### Loads (D31): move to the same check?

A recommendation only; the owner decides.

- **Names in the keys:** sound, by the argument above, with the value read instead of written (an empty slot means "not in the instance"). Two more loads and a compare than D31's pointer check, about one cycle. It shares the (class, name) cache with stores. It also makes inline the loads compiled before the class's keys held the name; today those call `cimpl_dict_get_known` for good. [read] (`instance_dict_get`, the `n >= 0` case), [inferred]
- **Names absent from the keys** (every method call on `self`, every class attribute read through an instance): no cheap check without a reference exists. Proving absence needs the whole key set, and a keys address with an entry count is not an identity. A class's keys can be replaced by re-shared keys with *fewer* entries, so a later keys object at a freed address can have the same `dk_nentries` and different keys. [run]: class keys with 5 entries; the instance that filled them freed; a 2-attribute instance stores a third name; the class gets new keys with 3 entries. A probe of the index table with the compile-time hash is sound (check `me_hash`, then `me_key`, at each slot until an empty one), but costs 10–20 instructions per load against D31's 6. [read] (`_PyObjectDict_SetItem`), [inferred]
- **Effect on F36.** F36 comes from any held reference. Almost every class has absent-name loads (its methods), so moving only the present-name loads leaves F36 as it is in practice, go's `Board` included. Removing F36 needs the probe for absent names as well.
- **Recommendation:** move present-name loads to the shared cache when option 4 lands. It is cheap, sound and simpler (no reference, no `dk_refcnt >= 2` rule for them). Keep D31's held keys for absent names unless F36's cost (go: about 3%, plus memory) is worth the probe. Measure before deciding.

### Interactions

- **Type guard (option 1, F37).** Inline code runs only after the guard passes, and the compile-time checks behind the guard decide everything about the class: `tp_setattro == PyObject_GenericSetAttr`, `tp_dictoffset > 0`, no data descriptor, and any class attribute of that name is an instance of a static type without `tp_descr_set` (rule 1). A class that gains `__setattr__`, a property or a slot changes its version tag, so the guard fails and the store goes through `PyObject_SetAttr`. [read] (`type_setattro` calls `PyType_Modified`)
- **Data descriptors and `__slots__`** take the generic path, decided behind the guard. A class with only `__slots__` has `tp_dictoffset == 0`: option 1's generic call.
- **Dict subclasses.** `obj.__dict__` may be set to a dict subclass (`subtype_setdict` only checks `PyDict_Check`). A subclass's dict is combined (`dict_new`), or split on `Py_EMPTY_KEYS` after `clear()`; `make_keys_shared` and the split copy make only exact dicts. So the check never passes for a subclass. If it did, it would still match CPython, which calls `PyDict_SetItem` and ignores an overriding `__setitem__`. [read]
- **`obj.__dict__ = {...}`.** The new dict is combined (a literal or `dict()`: [run]), so condition 2 sends it to option 2. After `obj.__dict__.clear()` it is split on `Py_EMPTY_KEYS`, and condition 3 fails. `obj.__dict__ = other.__dict__` can share one split dict between instances of two classes; the check judges that dict as it is, which is right.
- **Combined dicts:** condition 2, option 2's known-hash call.
- **Deletion** stays on the generic path (rule 8). It converts that dict to combined (condition 2 afterwards) and drops the class's keys. Instances still on the old keys stay split and keep hitting, which is right: CPython overwrites their slots too. [run] (`e1.a = 5` after `del e3.a`: `e1` stays split.) New instances get combined dicts, and `keys_inline` stops new compiles from emitting the inline path.
- **Out-of-order stores** fail condition 7 (or 4), and the helper converts the dict as CPython does.
- **Version tags (D35).** One counter, Ni's, from 2^63. CPython's counter was at 25,489 in the experiment. Tags from the two counters never meet, so no dict sees a tag twice. D25's cached `LOAD_GLOBAL` compares tags of globals dicts, so an instance `__dict__` used as a function's globals sees its tag change on an inline store. [run], [read]
- **Threads.** The GIL is held through the sequence, and nothing in it releases the GIL, so the counter needs no atomic increment. The helper writes the cache after the store and runs no Python code while writing, so neither a thread switch nor a `__del__` sees a half-written cache. [inferred]
- **Respawn.** Every compile-time decision depends on inputs a replay sees the same: the promoted type (behind the guard), `keys_inline` (block serial, D31), the cache pointer (from the table, which never shrinks) and the value's known type (`vinfo` state). The cache's contents are read only at run time. [inferred]
- **D31's loads.** Nothing to do: stores never hold keys. Loads that still hold keys keep F36; stores add no new F36 case.
- **3.12 port.** 3.11 and later keep instance values inline in the object, and `ma_version_tag` is deprecated (PEP 699). This design is 3.7-only, like D31. [read] (PORT312.md)

### Tests

A new file `tests/cases/stores.py`, compared with plain CPython as `tests/run.py` does, and run under an `ALL_CHECKS` build so that the replay check (`respawn_check`) covers every condition:

1. `overwrite_loop`: one instance; ints and objects stored to existing attributes many times; final `vars(o)`.
2. `init_fills`: `__init__` compiled on the first instance; 50 instances; `list(vars(o))` order and values. Under `NI_TRACE`, helper calls should be about one per instance after the first.
3. `out_of_order`: instances setting the same attributes in different orders (the dict converts); `list(vars(o))`.
4. `same_object`: `o.x = o.x`; `sys.getrefcount(v)` unchanged.
5. `old_value_freed`: the replaced value has a `__del__` that records `o.x`; it must see the new value.
6. `del_changes_class`: that `__del__` adds `__setattr__` (and, in a second case, a property `x`) to the class; the next store must use it.
7. `del_replaces_dict`: that `__del__` sets `o.__dict__ = {}`, or deletes another attribute; later stores and loads are right.
8. `gc_tracking`: a dict of floats, untracked after `gc.collect()`; store a list (tracked), a float (stays untracked), a tuple, the dict itself; `gc.is_tracked(o.__dict__)` after each must match CPython.
9. `globals_is_instance_dict`: `types.FunctionType(code, o.__dict__)` reading a global through the cached `LOAD_GLOBAL`; store to `o.x` inline, then call; it must see the new value.
10. `dict_replaced`: `o.__dict__ = {...}`, `o.__dict__.clear()`, `o.__dict__ = other.__dict__` (an instance of another class), a dict subclass with `__setitem__`; then stores.
11. `after_delete`: `del a.x` on one instance; stores on instances on the old keys and on instances created afterwards.
12. `reshare`: the first instance grows the keys from 8 to 16 slots; stores on old-keys and new-keys instances in one loop (size misses).
13. `name_joins_keys`: a store site compiled before any instance had the name.
14. `equal_not_identical_key`: `o.__dict__[''.join(['x', 'y'])] = 1` before the first `o.xy = 2`; one key `xy` afterwards.
15. `type_guard`: `__setattr__` added later (F37), a property added later, a class attribute whose heap class gains `__set__`, a `__slots__` member, an object without `__dict__` (error text).
16. `two_classes_one_site` and `class_address_reuse`: classes made and dropped in a loop, stored to through one compiled function.
17. `wide_keys`: a class with 200 attributes (2-byte indices, so `koff` is past them).
18. `self_cycle`: `o.x = o` on a dict holding only ints (tracking, cycle collected).

`tests/compile_bounds.py` must still pass (no compile loops from repeated misses).

### How to measure

- `tools/store_micro.py ni`: ns per store for split, combined and constructor, before and after. Expect about 2–3 ns for split (10.5 today). [inferred]
- `perf stat -e instructions:u` and time on `tools/profile_bench.py <bench> N` for richards, deltablue, raytrace, go and chaos; instructions per iteration from the difference between two values of N (which removes compiling).
- `tools/compilebench.py`: compile ms and code size per benchmark (D29). `tools/codesize.py` for where the new bytes go (stubs or code).
- A counter in `cimpl_store_attr_miss` under `NI_TRACE` (reported by `ni._stats()`), compared with `tools/store_cache_sim.py`'s prediction.
- The cache simulated on the five benchmarks (`tools/store_cache_sim.py <bench>`: plain CPython, one traced iteration after one untraced, each cache made when first used): [run]

| % of stores | richards | deltablue | raytrace | go | chaos |
|---|---|---|---|---|---|
| hit, overwrite (A) | 99.97 | 84.0 | 1.9 | 29.8 | 4.6 |
| hit, fill in order (B) | 0.02 | 11.3 | 63.4 | 0.2 | 63.6 |
| miss: no dict yet | 0.01 | 2.9 | 34.7 | 0.1 | 31.8 |
| miss: combined dict | – | – | – | 69.8 | – |
| miss: untracked dict, GC value | 0.00 | 1.9 | 0.00 | 0.03 | – |
| miss: size or entry | 0 | 0 | 0 | 0 | 0 |
| (of the hits: untracked dict, non-GC value) | 8.6 | 2.9 | 57.2 | 0.1 | 68.2 |

The last row is why the GC test looks at the value's type. Under Ni, go's `Board` loses its keys (F36), which moves about 2.8% of go's stores from A to combined.

### Gain and cost

- **Per store:** about 10.5 ns today, about 7.5 ns with option 1, about 2–3 ns inline. [run], [inferred]
- **Per benchmark,** over option 1 alone (from "Gains side by side"): richards about 13%, deltablue 5.5%, raytrace 2.4%, chaos 2.4%, go 1.4% (plus about 1% from option 2 on the combined branch). Against today: richards 21%, deltablue 9%, raytrace and chaos 4.7%, go 5.3%. [inferred]
- **Per site:** about 150–190 bytes more than option 1, 28–37 instructions on a hit, 6–7 conditions, and up to 7 respawns over its life (one for each condition that ever fails). Memory: one 40-byte cache per (class, name) stored to, never freed.

### Risks

- **Code size.** Richards +20–25% is about what D31 cost. Six stubs per site are much of it (see question 5).
- **A wrong GC test.** A missed track only leaks a cycle, but the `gc_refs` encoding and offset are 3.7-specific. An `ALL_CHECKS` assertion after the store (the dict is tracked if the value may be) would catch a mistake.
- **A decref placed before a condition** breaks replay determinism. The replay check catches it if a test reaches that condition.
- **Respawns on first misses** in constructors: each failing condition compiles once per site copy.
- **Layout constants** (`koff`, field offsets) duplicate 3.7's private structs, as D31 does (`ni_dictkeys_t`).

### Open questions for the owner

1. A cache per (class, name), as proposed, or per site with a key a replay sees the same (block serial and bytecode position)? Per site would also give per-site miss counts.
2. Include B? The simulation says yes (constructors); it costs one more branch at sites that reach it.
3. Option 2's known-hash call on the combined branch: take it with option 4 (go about +1%)?
4. Loads: move present-name loads to the shared cache now? Is F36 worth a probe for absent names?
5. Code size: is +20–25% on richards acceptable for about 13%? If not, a backend feature that lets several conditions share one respawn stub (they all lead to the same helper call) would remove most of the stub bytes. The replay would then have to stop at the first of them.
6. Should the miss helper stop refreshing a cache after many misses, before any thrash has been seen?

## Results: options 1 and 2

Done in `e86f8ae` (D39). [read] `pobject.c:heap_setattr`, `store_kind`, `store_guard`.

- A store of a constant name on a heap type is behind A9's guard (`tp_flags`, `tp_version_tag`). Once it passes, the store calls what `store_kind` chose at compile time: `_PyObjectDict_SetItem` (through `cimpl_instance_dict_set`, which turns a `KeyError` into `AttributeError` as CPython does) when `tp_setattro` is `PyObject_GenericSetAttr`, `tp_dictoffset > 0`, and the name's class attribute is absent or a non-data descriptor of a static type; otherwise `tp_setattro`. A failed guard calls `PyObject_SetAttr` (F37 fixed). Deletes take the same path with `NULL`, so `__delattr__` added later is seen too.
- Option 2: for a class without shared keys (`keys_inline` false), the store loads the dict at `tp_dictoffset`; if it exists and is combined, it calls `_PyDict_SetItem_KnownHash` with the hash computed at compile time. Split dicts and a missing dict go to option 1's helper.
- Static types keep the old path (their `tp_setattro` can't change). A megamorphic site (more than `MEGAMORPHIC_MAX` types) calls `PyObject_SetAttr`, as loads do.
- **Not in the plan: the guard repairs itself.** With one guard per site, raytrace ran 2% *more* instructions than before [run]. `Vector.__init__` is compiled by the first `Vector(...)`, at import, and `Vector.ZERO = ...` then changes the class, so every guard in `__init__` failed for good and every store took `PyObject_SetAttr`. Now the guard is shared by all sites of (type, name, kind) and holds only states of the type in which `store_kind` gives that kind. On a failed guard, `cimpl_store_type_changed` recomputes the kind and, if a guard exists for it, sets that guard to the type as it is now. A replay of a point after a passed guard sees a type of the compiled kind, and the failure branch needs no guard pointer, so the D21 rules hold without a static `respawn_guard`. [read], [run]

Instructions per warm iteration, millions (`perf stat -e instructions:u`, ten iterations minus none, after three to compile), release builds of `f27cfcb` and `e86f8ae` in the same session. [run]

| | richards | deltablue | go | raytrace | chaos |
|---|---|---|---|---|---|
| before | 223 | 33 | 1,251 | 3,347 | 511 |
| option 1 only (2 disabled) | 194 | – | 1,176–1,192 | – | – |
| options 1 and 2 | 194 | 31 | 1,150 | 3,264 | 485 |
| change | −13% | −6% | −8% | −2.5% | −5% |

- Larger gains than estimated for richards (8%) and go (3–4%), in instructions; the warm times below fall by about 10% and 7%. Option 2 is worth about 3% of go and nothing elsewhere, as expected. [run]
- raytrace's count moves by about 1% between runs; the others by under 1%. [run]

`tools/compilebench.py`, release build (code KB, compile ms; timings indicative, others were benchmarking). [run]

| | richards | deltablue | go | raytrace | chaos | all 29 |
|---|---|---|---|---|---|---|
| code before | 117.2 | 439.2 | 440.8 | 270.8 | 65.6 | 2,960 |
| code after | 133.3 | 448.2 | 474.4 | 273.3 | 66.1 | 3,037 |
| compile ms before | 0.60 | 2.45 | 3.14 | 1.71 | 0.46 | 21.1 |
| compile ms after | 0.67 | 2.89 | 3.28 | 1.77 | 0.46 | 21.9 |
| warm ms before | 10.81 | 1.55 | 68.72 | 190.87 | 26.99 | |
| warm ms after | 9.69 | 1.53 | 64.06 | 183.02 | 26.24 | |

- Code grew more than estimated: richards +14%, go +7.6%, all benchmarks +2.6%. Each compiled store site gains the guard and its respawn stub, about 50 bytes, and richards compiles each store site once per class it sees. Ending the block after the site (`split_after_global`) costs nothing measurable (richards and go unchanged without it). Option 2 adds about 10 KB to go. [run], [inferred]
- Compile time: within the run-to-run noise (deltablue's 2.45 → 2.89 ms came back as 2.52 ms in the option-1-only run). [run]

Tests: `tests/cases/attr_stores.py`, 16 cases: `__setattr__` and `__delattr__` added after compiling (both failed before), a property added later, a class attribute whose heap type gains `__set__`, a guard repaired after an unrelated class change and failing again after a relevant one, shared-key and combined dicts, `__dict__` replaced (dict subclass, non-`str` key), a key whose `__eq__` raises `KeyError`, no dict yet, `__slots__` (with and without `__dict__`), subclasses of `list`, `int` and `str`, a read-only property, deleting a missing attribute, an old value freed by the store. Checked build (`NI_TRACE=1 NI_ALL_CHECKS=1`): `tests/run.py` 385 passed; richards, deltablue, raytrace, go, hexiom, chaos and pyflate ran under `tools/profile_bench.py` without a replay divergence; `tools/check.sh`: all checks passed. [run]

## Option 4: results

Done in `80a60ce` (D38, D47), on `baseline-3.7` at `59b334b`. [read] `pobject.c:inline_store`, `store_cache`, `cimpl_store_attr_miss`, `store_needs_track`, `heap_setattr`.

### What was built

- **As designed.** After option 1's type guard, when the kind is the instance dict store and `keys_inline` is true: dict present (1), split (2), keys of the cache's `dk_size` (3), the cache's entry holds the name (4), no GC tracking to do (5), then a set value (A) or the empty slot at `ix == ma_used` (B, conditions 6 and 7). The store follows `insertdict`: the value (increfed, or its reference handed over), `ma_used + 1` for B, a tag from Ni's counter, the old value's decref last. A failed condition 1, 3, 4, 5 or 7 calls `cimpl_store_attr_miss` (option 1's store, then the cache follows the dict the store used); a combined dict (2) keeps option 2's `_PyDict_SetItem_KnownHash`. Deletes and classes without shared keys are unchanged. [read]
- **The cache** is a 56-byte struct per (class, name), found through a dict keyed by `(type address, name)`, never freed, holding no keys. `kword` is the entry's key offset in words (always a multiple of 8, so the emitted read is `[keys + 8*kword]`). The cache's address is loaded once into a register per site (`make_runtime_copy`). [read]
- **Version tag (D35):** `static uint64_t ni_dict_version = 1 << 63` in `pobject.c`, reached through the cache's `version` field, incremented and stored by the emitted code. D25's cached `LOAD_GLOBAL` compares tags for equality only (`pycompiler.c:load_global_cached`, `do_changed_global`), so it needs nothing; `stores.globals_is_instance_dict` checks that an inline store into a dict used as globals is seen. [read], [run]
- **GC test:** as designed (`(tp_flags >> 14 & 1) > (gc_refs >> 1) + 2`, unsigned, in one compare), dropped for a value of a known non-GC type, reduced to "dict untracked" for a known GC type. `_Static_assert`s pin 3.7's `PyGC_Head` (32 bytes, `gc_refs` at 16, shift 1, untracked -2, `HAVE_GC` bit 14) and the keys layout. [read]
- **Miss cap:** a cache stops being refreshed after 64 refreshes that *changed* it. Misses that leave it as it is (no dict yet, GC tracking, out of order) don't count: every constructor's first store is a miss, so counting them would freeze a class's caches after 64 instances, and a cache frozen before a later keys change would miss for good. (D47) [read]
- **Soundness, re-derived from 3.7's `dictobject.c`.** `insertdict` on a split dict with an exact `str` key converts only when `ix >= 0 && old_value == NULL && ma_used != ix` (or the key is new), so A (old value set) and B (`ix == ma_used`) are exactly the cases it leaves split; `MAINTAIN_TRACKING` comes before, and `_PyObjectDict_SetItem`'s re-share step does nothing when the keys are unchanged. Every `me_key =` in `dictobject.c` is an append at `dk_nentries` (`insertdict`, `PyDict_SetDefault`), `dictresize`'s copy into new keys, or a delete (`delitem_common`, `_PyDict_Pop_KnownHash`, `dict_popitem`), and every delete path converts a split dict first. `make_keys_shared` converts a combined dict in place only after `dictresize` has removed dummies, so its values fill `[0, ma_used)`. No hole found. [read]
- **The size check is load-bearing, not only a bounds check.** With 1-byte indices, entry `ix`'s key in 32-slot keys is at the same word as entry `ix + 1`'s in 8-slot keys (word 10 + 3·ix). An instance sharing an 8-slot dict where the name is at `ix + 1` would pass the entry check with a 32-slot cache. `stores.size_mismatch` builds that case; without the size check it stores into the wrong slot. [read], [run]

### Not in the design

- **A virtual value is computed before the type guard** (`heap_setattr`). Building it (a tuple, say) can collect garbage and run a `__del__` that changes the class, which would make the guard's compile-time decisions stale. This applied to option 1 too (KNOWN_BUGS F54). [inferred]
- **Blocks compiled before a cache is first filled call the helper.** `__init__` is compiled by the first instance, while the class's keys don't yet hold the names, so every inline check missed on that instance, and each miss is a respawn that compiles its own copy of the rest of `__init__`. Now the cache records the first block serial that may see it filled (`ready`, one-way: 0 if filled from the class's keys when made, else `psyco_next_serial` at the first refresh). A block older than that calls `cimpl_store_attr_miss` behind a test of the cache's `dk_size`, which respawns into the inline store once the cache is filled. A replay shares its block's serial, so it decides the same, as `keys_inline` does (D31). (D47) [read] Removed in `506a916`: misses no longer respawn, so such a block just calls the helper until the cache is filled ("Shrinking the store sites").
- **Stores to `self` in `__init__` predict B** (`init_store`: the first argument, in a code object named `__init__`). Otherwise the second instance took a B respawn at every site in turn, again one copy of the rest each. It depends only on the code, so a replay predicts the same. (D47) [read]

Together these two took richards' code from 320 KB to 246 KB (`Task.__init__` 103 → 54 KB) without changing its instruction count (134 → 133 M). [run]

### Measurements

Release builds of `59b334b` (options 1 and 2) and `80a60ce`, in the same session. Instructions per warm iteration, millions (`perf stat -e instructions:u`, ten iterations minus none, after three to compile; the method of "Results: options 1 and 2"). [run]

| | richards | deltablue | raytrace | go | chaos | hexiom | pyflate |
|---|---|---|---|---|---|---|---|
| options 1 and 2 | 193 | 31 | 3,245 | 1,185 | 500 | 40 | 2,442 |
| + option 4 | 133 | 28 | 3,113 | 1,126 | 472 | 40 | 2,357 |
| change | **−31%** | **−10%** | **−4.1%** | **−5.0%** | **−5.6%** | 0 | **−3.5%** |
| estimate (Gain and cost) | −13% | −5.5% | −2.4% | −1.4% | −2.4% | – | – |

`tools/compilebench.py` (code KB is exact; compile and warm ms are the mean of two runs each, alternated, on a machine others were using). [run]

| | richards | deltablue | raytrace | go | chaos | hexiom | pyflate | all 29 |
|---|---|---|---|---|---|---|---|---|
| code before | 133.3 | 449.9 | 273.0 | 447.0 | 66.1 | 348.8 | 201.4 | 2,887 |
| code after | 246.3 | 501.4 | 306.7 | 504.2 | 67.5 | 360.8 | 228.4 | 3,355 |
| change | **+85%** | +11% | +12% | +13% | +2% | +3% | +13% | +16% |
| estimate | +20–25% | +10–15% | +3–6% | +3–6% | +3–6% | – | – | |
| compile ms before | 0.70 | 2.52 | 1.91 | 3.09 | 0.50 | 2.75 | 1.66 | 21.2 |
| compile ms after | 1.02 | 2.76 | 1.99 | 3.31 | 0.47 | 2.80 | 1.70 | 22.2 |
| warm ms before | 9.75 | 1.50 | 181.4 | 65.0 | 27.1 | 2.13 | 136.9 | |
| warm ms after | 7.55 | 1.37 | 178.3 | 63.5 | 25.5 | 2.13 | 128.9 | |

`tools/store_micro.py ni` (ns per store, same session): split dict, existing key 8.7 → 3.1; constructor, three stores 38.0 → 27.6 (dict creation included); combined dict 7.2 → 7.7 (unchanged path; noise). [run]

- **Speed is well above the estimate**: richards −31% instructions (−23% warm time) against −13%; deltablue, raytrace, chaos and go about twice the estimate. pyflate, not in the estimate, gains 3.5%. [run]
- **Code size is well above the estimate for richards** (+85% against +20–25%) and above it for raytrace, go and pyflate (+12–13% against +3–6%). Compile time: richards +46% against +8–12%; the others +2–10% or within noise (chaos, hexiom). [run]

### Where the code goes

`tools/codesize.py richards` (`NI_CODESTATS=1`): [run]

| | before | after |
|---|---|---|
| total | 132 KB | 241 KB |
| STORE_ATTR compiles × bytes each | 178 × 117 B | 292 × 277 B |
| respawned buffers | 15, 6.3 KB | 54, 41.5 KB |
| stub chunks | 7 (28 KB) | 13 (52 KB) |
| `Task.__init__` | 10 KB, 12 copies of its last instruction | 54 KB, 39 copies |

- **Per compiled site,** the inline store adds about 160 bytes (117 → 277), as the design estimated (150–190). [run]
- **The rest is copies.** Each check that fails is a respawn, and a respawned branch compiles the rest of the code after the site as a new block, up to the next merge point, because the block ends at the site (`split_after_global`) and nothing joins the branch's continuation to the hit path's. Straight-line code with several store sites (a constructor) that misses at successive sites gets one copy per miss. Richards' `Task.__init__` is specialised for four classes, each with a different `link` and `input` per instance (a GC value into an untracked dict is a miss), so it takes several such chains. The design counted stubs and sites, not these copies. [read] (`pycompiler.c`, the `split_after_global` branch), [run]
- **Stubs per site:** 6 on the A path (type guard, dict, split, size, entry, old value), 7 with the GC test, one more for B (order) and one more in a block compiled before its cache was filled. About 15 bytes each (D32): 90–135 bytes per compiled copy. Size, entry, GC and order all lead to the same helper call, so one shared stub would serve them. It needs a dispatcher feature: several jumps into one respawn point whose replay stops at the first of them, with the same registers live at each. Not trivial; a follow-up (BACKLOG). [read], [inferred]
- **Joining the copies** would remove most of the growth: at the end of a respawned branch at a split site, look for the hit path's continuation at the same instruction and unify with its snapshot, as a merge point does. That is a `pycompiler.c` change, out of this change's scope (QUESTIONS 42). [inferred] Done since in `f229d14` (D44); and since `506a916` a failed check calls a helper instead of respawning.

### Tests

`tests/cases/stores.py`, 25 cases: the design's 18 (19 functions: `overwrite_loop`, `init_fills` with the helper-call count under Ni, `out_of_order`, `same_object`, `old_value_freed`, `del_changes_class`, `del_replaces_dict`, `gc_tracking`, `globals_is_instance_dict`, `dict_replaced`, `after_delete`, `reshare`, `name_joins_keys`, `equal_not_identical_key`, `type_guard`, `two_classes_one_site`, `class_address_reuse`, `wide_keys`, `self_cycle`) plus `many_instances`, `skip_a_slot` (a split dict missing a middle value), `size_mismatch`, `miss_cap` (one class's stores alternate between two key sets; refreshes stay within the cap), `module_instance` and `store_then_load`. Each check was removed in turn and the suite rerun: every removal failed at least one case (no version tag: `globals_is_instance_dict`; no GC test: `gc_tracking`, `self_cycle`; no order check: `skip_a_slot`; no `ma_used + 1`: 11 cases, two crashes; no entry check: `dict_replaced`, `miss_cap`; no size check: `size_mismatch`; no split check: two crashes). `out_of_order` alone didn't catch the order check: the first conversion drops the class's keys, so later instances are combined. [run]

Three cases make a class per call, which the compiled code keeps (C14 (2)), so they skip the leak check, as other such cases do. Functions calling `vars()` aren't compiled (C15), so the cases read `__dict__`. [run]

Checked build (`NI_TRACE=1 NI_ALL_CHECKS=1`): `tests/run.py` 478 passed; richards, deltablue, raytrace, go, hexiom, chaos and pyflate ran under `tools/profile_bench.py` without a replay divergence; `tools/check.sh`: all checks passed. [run]

## Shrinking the store sites

Branch `store-shrink`: `baseline-3.7` at `e4b3a5f` (with D44's joins), option 4 cherry-picked (`c6f447f`), then `506a916` (step 1), `0590a3c` (step 3a), `bd5361a` (step 3b), `5d1feae` (tests). The owner's budget is +20–25% code on richards over the baseline's 116 KB (D38). With joins, option 4 alone was +38% there, mostly the store's own size: 208 bytes per compiled site against option 1's 105. [run]

### What a site was

One richards site, disassembled from `ni._codestats()` (store branch): 275–286 bytes. About 69 bytes of type guard built from vinfo primitives, with copies between registers. Then the checks, each a respawn point with its own 15-byte stub (six or seven per site, D32), and each taken respawn a replayed buffer. Then an inline call to the old value's `tp_dealloc` of about 26 bytes. Spills where the allocator ran short of callee-saved registers, and a constant value loaded twice with a 10-byte `movabs`. [run]

### Step 1: one emitter, rare paths out of line (`506a916`)

- **The store is one hand-allocated emitter** (`x64/ipyencoding.c:psyco_inline_store_begin`, `_end`). It works in caller-saved registers only (RAX, RCX, RDX, RSI, RDI, R8–R11), which never hold a tracked value, so the compiler's state is the same on every path through the site. The only respawn point left is the miss call's error check. [read]
- **Every failed check jumps to one miss call**: `call` of `pobject.c:cimpl_store_miss` through a glue that aligns the stack (`x64/iprocessor.c:psyco_aligned_call_glue`), then `test eax, eax` and the error exit. The helper stores as CPython would: a combined dict takes the known-hash store (option 2), anything else `cimpl_store_attr_miss`, which also refreshes the cache. The jumps are short where they reach. The body is emitted twice, once with near jumps to find the distances and once for real, and the second pass can only be shorter. [read]
- **The dealloc is shared**: an old value whose count reaches zero calls `psyco_dealloc_rax_glue`, one glue for all sites (`mov rdi, rax; call [rax->ob_type->tp_dealloc]`, aligned). The miss call and the dealloc call are matched calls, so D32 (no unmatched call into shared code) holds. [read]
- **The next empty slot (B) is inline only for stores to `self` in `__init__`** (`init_store`, which already predicted B). Elsewhere an empty slot is a miss, and the helper fills it. [read]
- **The cache's `ready` serial is gone.** A miss no longer respawns, so a block compiled before its cache was filled just misses until the helper fills it, and then hits. The pre-fill test and its respawn went with it. [read]
- **New emitters** in `iencoding.h`: `INC_A`, `DEC_A`, `INC_O8`, `TEST_I8_BO`, `MOV_X8_R` (checked against GNU `as`). [run]

The site now: load the object, the value and the cache; dict present, split, `dk_size` equal, the name at `kword`, GC (one or two tests); the old value present; store, incref, version tag, old value's decref; FILL (B only); MISS. 191 bytes per compiled site on richards with the old type guard; respawned buffers 90 → 15, stub chunks 10 → 6, as on base. [run]

### Step 3: shorter encodings (`0590a3c`, `bd5361a`)

- **3a, the type guard in 47 bytes instead of 75** (`x64/ipyencoding.c:psyco_type_guard`, used by `pobject.c:type_changed`): load `tp_flags` and `tp_version_tag` into R10/R11, XOR each with the guard's word (RIP-relative when in reach, else through RAX, freed either way so that the compiler's state doesn't depend on where the code is), OR the two. Loads (`generic_getattr`, A9, D30) use the same guard, so 3a shrinks code without stores too: on richards about 3.6 of its 8.1 KB comes from the store sites. [read], [run], [inferred]
- **3b, operands**: the cache, a constant object or value, and the two glues are reached with a 7-byte RIP-relative `lea` and rel32 calls when in reach. Each choice is made once, before the two passes, from where the site starts, with a margin. The cache is on the C heap, usually more than 2 GB from the code, so the `lea` rarely applies. 170 → 166 bytes per site. [read], [run]
- **A bug found on the way**: the new `XOR_R_RIP` and `LEA_R_RIP` evaluated their offset after writing the first three bytes, so the guard read the tag three bytes off and always failed. Results stayed right (every store took `cimpl_store_type_changed`), and richards ran at 1,167 M instructions per iteration. The macros now evaluate the offset first. `stores.guard_holds` checks that an unchanged class's guards pass (`ni._stats()['store_type_changes']`), and fails with the old macros. `tests/encoding/check.py` can't catch this kind of bug, because it passes constant offsets. [run]

### Step 2: checks shared between stores to one object (not done)

Sharing the guard and the dict checks between consecutive stores to one object would save about 70 bytes at each store after the first. Statically, 25 of richards' 70 STORE_ATTR sites follow a store to the same local with only a load between (deltablue 10 of 65, go 20 of 60, raytrace 8 of 36, chaos 5 of 22, hexiom 4 of 15). On richards that is roughly 4 KB of 128 (3%) [inferred]. Not built, because:
- each guarded store ends its block (`split_after_global`), so the next store starts a new block, which a branch or a join can enter; a shared check needs the next block to know that it was entered only from the previous store's hit path;
- the guard is repaired per (type, name, kind) (D39), so a guard that passed for one name says nothing about another name's compile-time decisions after a repair; sharing needs one guard per run of names;
- the old value's decref and the miss call can run `__del__`, so they must re-enter the run at full checks, and a replay must take the same decisions (D21).
The budget is met without it. If it is wanted later, the tests are there: `stores.run_del_mid_sequence` (an overwritten value's `__del__` adds `__setattr__` or a property, replaces or shrinks the dict, assigns `__class__`, or grows the keys between stores), `run_interleaved_calls`, `run_two_objects` (two objects, the same object twice, a combined dict beside a split one). [run], [inferred]

### Measurements

Release builds of each commit, one session, `tools/compilebench.py` (one run each; compile ms are indicative, other agents shared the machine). Code KB: [run]

| | base `e4b3a5f` | store `c6f447f` | step 1 | + 3a | + 3b |
|---|---|---|---|---|---|
| richards | 121.7 | 162.0 | 136.4 | 128.3 | **127.6** |
| deltablue | 391.0 | 419.3 | 402.3 | 383.5 | 383.0 |
| go | 279.0 | 305.6 | 287.5 | 270.3 | 270.0 |
| raytrace | 199.6 | 213.8 | 203.5 | 195.8 | 195.7 |
| chaos | 56.5 | 57.8 | 57.2 | 55.1 | 55.1 |
| hexiom | 264.2 | 268.5 | 265.8 | 258.5 | 258.5 |
| all 29 | 2,363 | 2,548 | 2,431 | 2,347 | 2,344 |
| compile ms, all 29 | 19.8 | 21.7 | 20.2 | 20.0 | 19.2 |
| compile ms, richards | 0.74 | 0.99 | 0.79 | 0.70 | 0.72 |

Instructions per warm iteration, millions (`perf stat -e instructions:u`, ten iterations minus none after three; go and raytrace two runs): [run]

| | base | store | step 1 | + 3a | + 3b |
|---|---|---|---|---|---|
| richards | 227 | 177 | 173 | 159 | **161** |
| deltablue | 31 | 28 | 28 | 26 | 27 |
| go | 1,162–1,194 | 1,104–1,142 | 1,134–1,140 | 1,102–1,111 | 1,062–1,083 |
| raytrace | 3,277–3,296 | 3,152–3,165 | 3,154–3,158 | 3,050–3,106 | 3,043–3,067 |
| chaos | 494 | 474 | 475 | 460 | 459 |
| hexiom | 41 | 41 | 41 | 40 | 40 |

`tools/codesize.py richards -s` (`NI_CODESTATS=1`): [run]

| | base | store | step 1 | + 3a | + 3b |
|---|---|---|---|---|---|
| total | 118 KB | 167 KB | 133 KB | 125 KB | 124 KB |
| STORE_ATTR bytes per compile (172 compiles) | 105 | 208 | 191 | 170 | 166 |
| stub chunks | 6 | 10 | 6 | 6 | 6 |
| respawned buffers | 15, 3.0 KB | 90, 18.1 KB | 15, 3.4 KB | 15, 3.2 KB | 15, 3.1 KB |

- **Richards against the budget: +10% on the baseline's 116 KB** (+5% on this session's base build, 121.7 KB), inside +20–25%. Step 1 did most of it (162 → 136 KB). Its smaller sites matter less than the respawns it removed: the copies they compiled are gone, so buffers and stub chunks are back to base's. [run]
- **The speed gain is kept and grows**: richards 177 → 161 M (−29% against base). Step 3a speeds up loads as well as stores (richards 173 → 159). [run]
- **Over all 29 benchmarks the branch is now 0.8% smaller than base** (2,344 against 2,363 KB), the inline stores included, because the shorter guard also serves loads. Compile time is within noise of base. [run]
- Part of 3a's saving would apply to base without inline stores [inferred]. Measured against base plus 3a, the inline stores' own cost on richards is probably about +10 KB, not +6 KB. Not measured.

### Tests

Checked build (`NI_TRACE=1 NI_ALL_CHECKS=1`) at `5d1feae`: `tests/run.py` 597 passed, 4 xfail (C23's three, F55); `tests/cases/stores.py` 45 cases and the F55 xfail; `tests/compile_bounds.py` 7 checks passed; `tests/encoding/check.py` 69 macros, 275,191 cases, no mismatch; `run_v_mem_mov.sh` 90,144 cases; the smoke test gives the same output with and without Ni; the metac outputs are up to date; richards, deltablue, raytrace, go, hexiom, chaos and pyflate ran under `tools/profile_bench.py` without a replay divergence. CPython's tests (`tests/cpython_tests.py -j 2`): 281 modules, none unexpected. [run]

Found on the way: Ni keeps a promoted type after a call assigns `__class__`, with or without stores (KNOWN_BUGS F55). [run]

### Risks

- **The emitter is hand-written machine code**, outside the vinfo layer: register choice, the two-pass jump sizing and the glue's stack alignment are checked by the tests and by `psyco_assert`s, not by the allocator. A future change that keeps a tracked value in a caller-saved register would break it silently. [inferred]
- **The miss helper runs arbitrary code** (a combined dict's `__eq__` on a key, `__del__` of an old value), as the respawned C call did before. Its error check is the site's only respawn point. [read]
- **The dealloc glue is called with the stack misaligned by the site** and aligns it itself. Under `Py_TRACE_REFS` it calls `_Py_Dealloc`, which hasn't been built here. [read]

## Reproduce

- `perf record -m 16 -F 4999 -g -o X.data python3.7 tools/profile_bench.py richards 4`, then `perf report -i X.data --no-children --sort sym -g none`. (`-m 16` because the locked-memory limit is shared with other perf users.)
- `python3.7 tools/storeclasses.py richards` (plain CPython; tracing makes it about 50 times slower).
- `python3.7 tools/store_micro.py ni`, then `plain`.
- `python3.7 tools/store_cache_sim.py raytrace` (plain CPython; option 4's cache, simulated).
- Option 4's instruction counts: `perf stat -x, -e instructions:u` on a script that loads the benchmark with `pyperf37.load`, runs `timed(3)`, then `timed(10)` or nothing; the difference over ten. Helper calls and refreshes: `ni._stats()['store_misses']`, `['store_refreshes']`; failed store type guards: `['store_type_changes']`.
- "Shrinking the store sites": for each commit, `git checkout <commit> -- ni`, a `NI_CODESTATS=1` build for `tools/codesize.py <bench> -s`, then a release build for `tools/compilebench.py` and the instruction counts above. One site's code: `ni._codestats()` gives each copy's address and length, disassembled with capstone.
- The other [run] facts in "Option 4: design" (unused entries zeroed, keys address reuse, re-share with fewer entries, `gc_refs` values, conversions) come from short ctypes scripts under plain CPython 3.7.17, not kept.
