---
written: 2026-11-17
status: web research (agent), not re-checked by hand; the full version with sources is `git show e404100:doc/CPYTHON_JIT.md`
---
# CPython's own JIT, for comparison

## Speed and status

- **3.15 (final 2026-10-09):** 7–8% geomean on x86-64 Linux, 11–12% on AArch64 macOS, against the tail-call interpreter; per benchmark from about −15% to more than +100%. Experimental, off by default, needs LLVM 21 to build ([What's New 3.15](https://docs.python.org/3.15/whatsnew/3.15.html)). Earlier releases were roughly at parity with the interpreter or slower (3.13, 3.14).
- **Same-build JIT against no JIT** (doesjitgobrrr, 2026-09-25, x86-64): geomean 1.065. Best: richards 2.4×, nbody 1.5×, spectral_norm 1.4×, float 1.3×, deltablue 1.2×. Worst: unpack_sequence 0.57×, mdp 0.93×, generators 0.96×.
- **Future:** the Steering Council froze new JIT work on main (2026-06-05) pending a standards-track PEP; PEP 836 was withdrawn (2026-10-19), and removal from main is expected early in the 3.16 cycle. As of 2026-11-17 `Python/jit.c` was still on main and no removal PR was found. 3.15 keeps the JIT.

## Running pyperformance on 3.7

- pyperformance ≤ 1.11.0 declares `>=3.7`, but from 1.0.3 almost every benchmark's `pyproject.toml` says `requires-python >=3.8`, and `cli.py` drops benchmarks that don't match the harness's own Python. So either use pyperformance 1.0.2, or run each `bm_X/run_benchmark.py` directly under 3.7 with pyperf 2.8.0 (the last release for 3.7). Ni does the second: `tools/pyperf37.py` over the vendored `tools/pyperformance/` (29 pure-Python benchmarks in 23 files). All 1.11.0 benchmark sources parse with the 3.7 grammar.
- pyperf runs each benchmark in fresh worker processes, so Ni must be enabled in each worker, not just the parent.
- Not usable on 3.7: `*_eager` (3.12), `*_tg` (3.11), `typing_runtime_protocols` (3.8), `django_template` (3.10).

## Type information for a JIT

- **Shipped:** 3.12's watchers (`PyType_AddWatcher`, `PyDict_AddWatcher`, `PyFunction_AddWatcher`, `PyCode_AddWatcher`) and `PyUnstable_Type_AssignVersionTag`: invalidation hooks, not type descriptions. The tier-2 optimiser (3.13+) propagates types and removes guards, but knows builtin result types only for hand-coded cases (`_CALL_LEN`, `_CALL_ISINSTANCE`, `_CALL_TYPE_1`, `_CALL_STR_1`, `_CALL_TUPLE_1`, `_CALL_LIST_APPEND`).
- **Not shipped:** mining Argument Clinic types for unboxed calls (faster-cpython/ideas#546, open), Mark Shannon's "hasty call" (ideas discussion #374, idea only), HPy's typed signatures (open; CPython hasn't adopted HPy), `METH_TYPED` (a PyPy prototype, PLDI SOAP 2024). Vectorcall (PEP 590) still passes `PyObject*`.
- **Bottom line:** no typed or unboxed C calling convention has landed in 3.12–3.15. For Ni, the Argument Clinic files (`*.c.h` and the clinic input) remain the best machine-readable source of C-level signatures [inferred].
