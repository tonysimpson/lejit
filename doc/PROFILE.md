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

1. **Inline A9's guard.** Compare the type's `tp_version_tag` and flags in emitted code instead of calling `cimpl_type_unchanged`. Worth 2–3.5%.
2. **Shared-key instance attributes.** CPython 3.7 instances of a class share one keys table (PEP 412), and in a shared table a name's index never changes. So a load can be: the dict pointer at `tp_dictoffset`, a check that its `ma_keys` is the expected table, then `ma_values[index]`, with a fallback when any of these fails. That replaces `PyDict_GetItem` and `lookdict_split`, worth up to about 35%. Stores are harder: a store must give the dict a new `ma_version_tag`, whose counter is static in `dictobject.c`. [read] (`Objects/dictobject.c`, `Objects/dict-common.h`), [inferred]
