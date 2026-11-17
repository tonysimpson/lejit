---
written: 2026-11-17
head_ref: 3cd02f7
status: research; options for the owner, nothing implemented
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

## Reproduce

- `perf record -m 16 -F 4999 -g -o X.data python3.7 tools/profile_bench.py richards 4`, then `perf report -i X.data --no-children --sort sym -g none`. (`-m 16` because the locked-memory limit is shared with other perf users.)
- `python3.7 tools/storeclasses.py richards` (plain CPython; tracing makes it about 50 times slower).
- `python3.7 tools/store_micro.py ni`, then `plain`.
