---
written: 2026-11-17
status: measurements; rerun with tools/profile_bench.py
---
# Where compiled code spends its time

Method: `perf record -F 4999 -g` on `tools/profile_bench.py <benchmark> 4` (warm, after three compiling iterations). Release build at `4fbe1ef`. Self time, `perf report --no-children`. Emitted code shows as `[JIT]`. [run]

## 2026-11-17: richards, deltablue, raytrace

| | richards | deltablue | raytrace |
|---|---|---|---|
| Ni's emitted code (`[JIT]`) | 29% | 21% | 15% |
| libpython | 58% | 68% | 70% |
| ni.so (helpers, metas) | 13% | 10% | 13% |

The C functions with the most self time:

| function | richards | deltablue | raytrace | what calls it |
|---|---|---|---|---|
| `PyDict_GetItem` | 20.5% | 17.1% | 13.1% | instance attribute load (`pobject.c:cimpl_instance_dict_get`, A9) |
| `lookdict_split` | 15.0% | 12.1% | 6.8% | the same, on a shared-key (PEP 412) instance dict |
| `insertdict` | 5.9% | 3.3% | 4.9% | instance attribute store |
| `cimpl_instance_dict_get` | 3.8% | 3.1% | 2.1% | A9's helper itself |
| `cimpl_type_unchanged` | 3.5% | 2.2% | 1.9% | A9's `tp_version_tag` guard, a C call |
| `_PyObject_GenericSetAttrWithDict` | 3.2% | 1.6% | 1.2% | STORE_ATTR, residual |
| `_PyType_Lookup` | 2.4% | 4.3% | 1.6% | residual lookups, e.g. attributes of classes (`type_getattro`) |
| `_PyObjectDict_SetItem`, `PyDict_SetItem`, `_PyObject_GetDictPtr` | 5.2% | 3.6% | 2.6% | stores and the dict pointer |
| `PyErr_Restore` | 1.9% | 1.9% | – | `PyDict_GetItem` saving and restoring the error state |
| `cimpl_enter/leave_recursive_call` | 2.6% | 1.0% | – | the recursion depth count on compiled calls (C12) |
| allocation and dealloc (`pymalloc_*`, `subtype_dealloc`, `dict_dealloc`) | – | – | about 8% | creating instances with dicts |

Instance attributes and dicts take about 60% of richards, 50% of deltablue and 36% of raytrace. Call overhead is small, which agrees with the inlining measurement (STATUS). [run], [inferred]

## What follows

1. **Inline A9's guard.** Done: `pobject.c:type_changed` compares `tp_flags` and `tp_version_tag` in emitted code instead of calling `cimpl_type_unchanged`. Same session, before and after: richards 19.4–20.2 → 18.5–18.9 ms (about 5%), go 82.1 → 79.1 ms, others within noise; code 2–4% larger, compile time unchanged. [run]
2. **Shared-key instance attributes.** Done (D31): `pobject.c:instance_dict_get` loads an instance attribute inline when the class's instances share keys (PEP 412): the dict at `tp_dictoffset`, one check that it uses the keys compiled in, then the value at the name's index. A name not in the keys reads the empty slot after the last entry, guarded by the keys' entry count. Stores still go through C. Same session, release build, before (`614ea60`) and after: [run]

| benchmark | Ni warm (ms) | code KB | compile ms |
|---|---|---|---|
| richards | 19.0 → 12.0 | 150 → 200 | 0.57 → 0.64 |
| richards_super | 32.2 → 25.5 | 152 → 198 | 0.57 → 0.72 |
| deltablue | 2.20 → 1.64 | 529 → 700 | 2.11 → 2.56 |
| raytrace | 222 → 194 | 359 → 438 | 1.97 → 1.78 |
| go | 80.3 → 75.0 | 1509 → 1650 | 13.0 → 13.7 |
| hexiom | 2.71 → 2.27 | 870 → 1007 | 6.04 → 6.29 |
| pyflate | 173 → 144 | 466 → 516 | 2.56 → 2.75 |
| chaos | 32.3 → 28.1 | 90 → 98 | 0.44 → 0.46 |

A first version that sent classes without usable keys to a check that always fails respawned once per site: go then made 1,896 respawns (491 before) and 2.4 MB of code for no gain. `keys_inline` now sends a class whose keys CPython has dropped straight to the C helper.
3. **Unshared instance dicts with a compile-time hash.** Done (`0965d06`): when a class has no usable shared keys, or the keys check fails, `pobject.c:instance_dict_get` loads the dict at `tp_dictoffset` and calls `_PyDict_GetItem_KnownHash` with the name's hash computed at compile time, instead of `_PyObject_GetDictPtr` plus `PyDict_GetItem`. No dict is "not found" without a call. go: 1,535 → 1,364 million instructions per iteration (11% fewer); richards unchanged (its classes share keys). Timings to follow on a quiet machine. [run] (`perf stat -e instructions`)
4. **Recursion depth inline** (`1de8b28`, C12): `cimpl_enter/leave_recursive_call` (2.6% of richards, 1.0% of deltablue) are gone. The enter is 7 instructions (thread state, depth load, add, store, limit load, compare, branch); the leave is 7 too, testing `overflowed`. Only an overflow, or a leave while `overflowed` is set, calls C. Release build, instructions per warm iteration (`perf stat -e instructions:u`, ten iterations less none, two runs each) and `tools/compilebench.py`, before (`3cd02f7`) and after: [run]

| benchmark | instructions/iter (M) | code KB | compile ms |
|---|---|---|---|
| richards | 233–235 → 220–221 (−6%) | 115 → 113 | 0.59 → 0.58 |
| deltablue | 34.4–34.8 → 33.1–33.2 (−4%) | 417 → 432 | 2.36 → 2.46 |
| go | 1,253–1,266 → 1,225–1,232 (−2%) | 1,071 → 1,083 | 13.6 → 13.3 |
| all of compilebench | | 3,787 → 3,880 (+2.5%) | 35.4 → 34.6 |

Each call site grows by about 40 bytes (two inline sequences of about 34 bytes instead of two C calls). Timings, indicative only (other jobs were running): richards warm 12.3 → 10.9 ms, deltablue 1.64 → 1.53 ms, go 70.7 → 67.2 ms.

## 2026-11-17: str and bytes (D14)

Which string operations the pyperformance set (`tools/pyperformance/`) spends time in under Ni, before any str or bytes support beyond `==`. Release build at `3cd02f7`. [run]

Method: `perf record -F 2999 -g` on `tools/profile_bench.py <bench> 3` for all 23 benchmark files, self time summed over every `unicode_*`, `bytes_*`, `PyUnicode_*`, `PyBytes_*`, `stringlib_*`, `striter_*`, `memchr` and `builtin_ord` symbol (`_PyUnicode_FromId` and `lookdict_unicode*` left out: they are attribute and dict machinery, not string operations). And which string functions compiled code calls: a temporary log in `iencoding.c:run_time_call` (not committed) of every emitted C call target, counted per benchmark minus what all of them share (start-up code). The call counts are call *sites* emitted, not calls made.

| benchmark | string self time | what |
|---|---|---|
| pyflate | 3.5% | `bytes_subscript` 1.2%, `stringlib_bytes_join` 0.9%, `_PyBytes_FromList` 0.4%, `PyBytes_FromStringAndSize` 0.2%, `striter_next` 0.2%, `memchr` 0.2% (`bytes.find`), `bytes_length` 0.1%, `bytes_repeat` 0.1%, `builtin_ord` 0.07% |
| deltablue | 0.7% | `str % args` (`PyUnicode_Format` and the writer), in its planner's error messages |
| mdp | 0.3% | `unicode_hash` (dict keys) |
| all others | ≤ 0.07% | |

The emitted call sites agree: pyflate has 35 `bytes_subscript`, 20 `bytes_repeat`, 16 `bytes_join` and 12 `bytes_length` sites; hexiom has 57 `unicode_subscript` sites but they run in set-up, not in the timed loop (0.01%). No benchmark here is string-heavy: the set has none of pyperformance's regex, json, logging or template benchmarks. So the measurement supports only the basics: bytes indexing and `len()` for pyflate, and the same for str for correctness and constant folding. Iteration over bytes (pyflate's `enumerate(L)`, `striter_next` 0.2%) goes through `enum_next` anyway, and concatenation is `b"".join` of a list (0.9% + 0.4%) rather than repeated `+`, so **concatenation buffers would not help here**; not implemented. [run]

Done (`9abd6a3`, `f1612c5`): `len()` and `s[i]` with an int index for str (`punicodeobject.c`) and bytes (`pbytesobject.c`), and `in` between two constant strs. A bytes item is a bounds check and a byte load giving a virtual int; a str item is inline for compact ASCII strings and gives CPython's cached one-character string. Before and after, release build, same session (instructions per warm iteration with `perf stat -e instructions:u`, `PYTHONHASHSEED=0`; code and compile time from `tools/compilebench.py`; timings indicative, other work shared the machine):

| benchmark | instructions (M) | code KB | compile ms | warm ms |
|---|---|---|---|---|
| pyflate | 2,681 → 2,528 (−5.7%) | 281.0 → 273.2 | 2.72 → 2.62 | 148.5 → 140.2 |
| deltablue | 34 → 34 | 416.8 → 420.8 | 2.39 → 2.39 | 1.61 → 1.60 |
| hexiom | 41 → 42 | 603.5 → 601.2 | 5.98 → 6.26 | 2.31 → 2.22 |
| chaos | 509 → 506 | 64.9 → 64.9 | 0.45 → 0.44 | 27.4 → 27.4 |
| richards | 235 → 235 | 115.4 → 115.4 | 0.58 → 0.59 | 12.1 → 12.3 |
| raytrace | 3,414 → 3,391 | 262.9 → 262.9 | 1.69 → 1.65 | 195 → 194 |
| go | 1,226 → 1,268 | 1070.7 → 1074.8 | 13.5 → 13.2 | 70.9 → 70.2 |

go's instruction count varies by ±3% between runs of the same build (A/B: 1,214 and 1,256 before, 1,264 and 1,247 after). Over all 29 benchmarks compile time is 35.0 → 34.4 ms and code 3,787 → 3,793 KB. In pyflate `bytes_length` is gone and `bytes_subscript` falls from 1.2% to 0.5% (what is left is slicing, `nt[i:i + 1]`). [run]

Left as C calls, with what the measurement says about them: run-time `in` for str (no hot site; a constant needle could use a one-character search helper), iteration over str and bytes, slices, `bytes.join`, `%` formatting (deltablue, 0.7%), and `x[i] == 'c'`, which is still a `cimpl_unicode_eq` call although both sides are usually CPython's cached one-character strings (Py2's virtual characters, `pstringobject.c`, would make it a byte compare; QUESTIONS).

## 2026-11-17: slices, join and % (D56)

What is left of the strings item after `len`, indexing and `in`: slices, `str.join` and `%` formatting. Measured first, release build at `59b72aa`. [run]

**Where the benchmarks spend it.** `perf record -F 2999 -g` on `tools/profile_bench.py <bench> 3` for pyflate, deltablue, chaos, go, hexiom, unpack_sequence, richards and raytrace (the set has no json, regex, genshi or mako benchmark). Self time:

| benchmark | slices | join | % |
|---|---|---|---|
| pyflate | the slice object: `slice_dealloc` 1.1%, `_PyEval_SliceIndex` 0.7%, `PySlice_Unpack` 0.7%, `PySlice_New` 0.7%, `PySlice_AdjustIndices` 0.4% (3.7% together); the slicing itself: `list_ass_slice` 6.5%, `list_slice` 3.2%, `bytes_subscript` 0.6% | `stringlib_bytes_join` 1.1%, `_PyBytes_FromList` 0.4% | — |
| hexiom | `list_slice` 0.3%, `PySlice_Unpack` 0.2%, `PySlice_New` 0.2%, `slice_dealloc` 0.1%, `list_ass_slice` 0.1% | — | 0.03% |
| deltablue | `list_ass_slice` 0.3% | — | `PyUnicode_Format` 0.4% and the writer, about 0.8% in all (`"v%s" % i` in set-up, `'%s' % (a, b)` in `__repr__`) |
| the others | ≤ 0.01% | — | ≤ 0.1% |

pyflate's hot slices are lists (`move_to_front`: `l[:] = l[c:c + 1] + l[0:c] + l[c + 1:]`) and one-byte bytes slices (`nt[i:i + 1]`); str slices run in no timed loop. So the slice object, which every slice pays for whatever its type, is the cost worth removing, and list must be covered with str and bytes.

**Per call** (a loop of 10⁶, `perf stat -e instructions:u`, minus the empty loop; a scratch micro-benchmark, not committed): `s[i:i + 3]` on a str 679 instructions, `b[i:i + 1]` 437, `l[i:i + 3]` 717, `','.join(three strs)` 634, `b''.join(three bytes)` 528, `"v%s" % k` 1,397, `"<%s - %d>" % (a, k)` 2,201. For join, compiled code already calls `PyUnicode_Join` directly (the method lookup folds): its profile is `_PyUnicode_JoinArray`, allocation and memcpy, with 1% in `PySequence_Fast`. For `%`, `unicode_mod` and `PyMapping_Check` are 3% of the call; the rest is `PyUnicode_Format` itself, the writer's over-allocation and resize (about 20%) and the int's decimal conversion. Splitting a constant format into concatenation could save perhaps a third of a `%` call, about 0.3% of deltablue, for a translation of `PyUnicode_Format`'s parser that every format directive would need; a direct call would save 3% of the call. **Neither join nor `%` is worth doing on this set.** [run]

**Done** (`b251646`, D56): virtual slices (`psliceobject.c`). BUILD_SLICE makes one; the str, bytes and list subscript metas and list's `mp_ass_subscript` unpack a step-None slice of None or int bounds into words and call one helper, the type's step-1 case after `PySlice_Unpack`. Per call: str 679 → 320 instructions, bytes 437 → 88, list 717 → 373. Release build, before (`59b72aa`) and after, same session; instructions per warm iteration with `perf stat -e instructions:u`, `PYTHONHASHSEED=0`, ((3 + 12) − (3 + 2) iterations) / 10, two runs each, identical to the M; code, compile time and warm time from `tools/compilebench.py` (warm times indicative, other agents shared the machine):

| benchmark | instructions (M) | code KB | compile ms | warm ms |
|---|---|---|---|---|
| pyflate | 2,309 → 2,032 (−12.0%) | 180.6 → 178.1 | 1.69 → 1.54 | 134.0 → 112.3 |
| hexiom | 41 → 40 | 260.8 → 249.9 | 2.76 → 2.67 | 2.16 → 2.13 |
| chaos | 459 → 460 | 55.8 → 59.8 | 0.48 → 0.46 | 25.7 → 26.0 |
| richards | 163 → 163 | 128.8 → 128.8 | 0.76 → 0.76 | 8.46 → 8.37 |
| deltablue | 27 → 27 | 392.4 → 392.4 | 2.86 → 2.65 | 1.35 → 1.37 |
| raytrace | 3,071, 3,080 → 3,061, 3,058 | 197.7 → 197.7 | 1.46 → 1.44 | 175 → 183 |
| go | 1,058 → 1,058 | 276.6 → 276.6 | 2.37 → 2.42 | 60.9 → 60.6 |

All 29 benchmarks: code 2,381 → 2,369 KB, compile 20.1 → 19.4 ms. compilebench's code column moves in whole 4 KB respawn-stub chunks (`dispatcher.c:psyco_new_stub`, 15 bytes a stub): an int bound's one-digit test adds a respawn point, and a benchmark whose last chunk fills takes a new one (chaos, nbody and scimark_monte_carlo +4.0 KB; scimark_sor and meteor_contest −4 to −5 KB the other way). The code itself: `tools/codesize.py` (an `NI_CODESTATS` build) gives chaos 57 → 57 KB, pyflate 182 → 180 KB (BUILD_SLICE's 1.3 KB gone, BINARY_SUBSCR 3.1 → 2.9 KB), hexiom 269 → 254 KB with 5,051 → 4,841 compiles (the helpers' results have a known type, so the code after them needs no promotion of it; [inferred]). In pyflate's profile `PySlice_New`, `slice_dealloc`, `PySlice_Unpack` and `_PyEval_SliceIndex` are gone; `list_ass_slice` and `list_slice` are what is left, the slicing itself. [run]

Left: tuple slices (the generic path; no hot site), a step (`x[::-1]`), bounds with `__index__`, `str.join`, `bytes.join` and `%` (above), and the `bytes.join` of pyflate (1.1%), which is the join's own work.

## 2026-11-17: the shapes kernel after the rejoin (C54, D66)

`perf record -e instructions:u` on `tools/bench.py --child ni shapes`, `NI_CODESTATS` builds (otherwise release) of `f229d14^` and `f229d14`: run-time `isinstance` is most of both profiles (`_PyObject_GenericGetAttrWithDict` 18.7% / 15.6%, `PyObject_IsInstance` 12.2% / 16.9%, `recursive_isinstance` 6.9% / 6.5%, `_PyObject_LookupAttr` 6.1% / 7.5%, `PyType_IsSubtype` 5.0% / 7.3%, `_PyType_Lookup` 4.4% / 7.6%): every failing `isinstance` against a type looks up `__class__`. After the rejoin the instructions in these calls nearly double (`PyObject_IsInstance` about 86 → 164 M of 708 → 970 M), because one shared promotion site now sends `None` and both ints to the generic chain as well as the two classes (CODE_SIZE.md "Promotion caches and the cut-off"). Fixed: 977 → 720 M. The classes still take the generic chain by design (`isinstance`'s site promotes static types only). [run]


## 2026-11-18: heap state (QUESTIONS 51, D92)

A change that alters only start-up code can still move a benchmark's instructions per warm iteration by 1–2%, through the allocators. pyflate allocates 2 KB lists in its hot loop (`move_to_front`), above pymalloc's 512-byte limit, so glibc's `_int_malloc` serves them, and how far it searches its bins depends on what start-up left there; hexiom's run depends on the pymalloc heap. With the generated `in` (D92, measured on `dc3c353`) pyflate ran 1,956 → 1,971 M and hexiom 39.73 → 40.10 M per warm iteration, with the hot code the same [run].

Two checks tell a heap effect from a code effect:
- **Split the count by object.** `valgrind --tool=callgrind --smc-check=all-non-file python3.7 BENCH-DRIVER`, then `callgrind_annotate --auto=no`: emitted code shows as `???:0x…`, and the self counts sum per object (emitted code, `ni.so`, libpython, libc). For pyflate the emitted code differed by 50 instructions in 3,276 M, `ni.so` by −0.19 M and libpython by +0.39 M; libc's `_int_malloc` and `unlink_chunk` carried the rest (+27 M over five iterations) [run]. callgrind uses glibc's own malloc, but its address layout is not the native one, so a heap effect's size there differs.
- **Perturb the heap.** Run both builds with one `malloc` block (ctypes) or one Python object kept alive after warm-up, or under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. pyflate: 1,977/1,992 M became 1,997/1,979 M with one 2 KB block kept; hexiom: 39.78/40.14 M became 39.76/39.70 M with one object kept [run]. A difference that changes sign is the heap's.

Use fixed iteration counts (three warm-up, then N; the difference of two runs over N) rather than time-bounded runs when comparing under perturbations. With `perf record -e instructions:u` on this AMD machine, samples skid and the period must stay above about 1 M instructions or the kernel throttles them, so per-symbol counts don't settle it; callgrind does.
