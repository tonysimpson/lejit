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

Done (`3aadf1e`, `296bf84`): `len()` and `s[i]` with an int index for str (`punicodeobject.c`) and bytes (`pbytesobject.c`), and `in` between two constant strs. A bytes item is a bounds check and a byte load giving a virtual int; a str item is inline for compact ASCII strings and gives CPython's cached one-character string. Before and after, release build, same session (instructions per warm iteration with `perf stat -e instructions:u`, `PYTHONHASHSEED=0`; code and compile time from `tools/compilebench.py`; timings indicative, other work shared the machine):

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
