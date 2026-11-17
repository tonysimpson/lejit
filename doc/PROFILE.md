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
4. **Recursion depth inline** (`5409970`, C12): `cimpl_enter/leave_recursive_call` (2.6% of richards, 1.0% of deltablue) are gone. The enter is 7 instructions (thread state, depth load, add, store, limit load, compare, branch); the leave is 7 too, testing `overflowed`. Only an overflow, or a leave while `overflowed` is set, calls C. Release build, instructions per warm iteration (`perf stat -e instructions:u`, ten iterations less none, two runs each) and `tools/compilebench.py`, before (`3cd02f7`) and after: [run]

| benchmark | instructions/iter (M) | code KB | compile ms |
|---|---|---|---|
| richards | 233–235 → 220–221 (−6%) | 115 → 113 | 0.59 → 0.58 |
| deltablue | 34.4–34.8 → 33.1–33.2 (−4%) | 417 → 432 | 2.36 → 2.46 |
| go | 1,253–1,266 → 1,225–1,232 (−2%) | 1,071 → 1,083 | 13.6 → 13.3 |
| all of compilebench | | 3,787 → 3,880 (+2.5%) | 35.4 → 34.6 |

Each call site grows by about 40 bytes (two inline sequences of about 34 bytes instead of two C calls). Timings, indicative only (other jobs were running): richards warm 12.3 → 10.9 ms, deltablue 1.64 → 1.53 ms, go 70.7 → 67.2 ms.
