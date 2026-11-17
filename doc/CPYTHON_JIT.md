---
written: 2026-11-17
status: web research (agent), sources linked; not re-checked by hand
---
# CPython JIT: speed-ups, 3.15 status, benchmarks (researched 2026-11-17)

## 1. Reported speed-ups over time

| When | Claim | Source |
|---|---|---|
| 3.13 (2024) | "about as fast as the existing specializing interpreter" (no geomean given); 10–20% more memory | [PEP 744](https://peps.python.org/pep-0744/) (Draft, Informational, created 2024-04-11) |
| 3.13 / 3.14 (Jul 2025) | Ken Jin: the 3.13 JIT is often *slower* than the interpreter; with Clang 20 the 3.14 interpreter usually wins; parity only with an old compiler such as GCC 11. He disputes the earlier "2–9% faster" figure (origin of that figure not confirmed) | [jit-reflections](https://fidget-spinner.github.io/posts/jit-reflections.html), [DevClass 2025-07-09](https://www.devclass.com/development/2025/07/09/despite-30-months-work-core-developer-says-pythons-jit-compiler-is-often-slower-than-the-interpreter/1629293) |
| 3.14 (Oct 2025) | "range from 10% slower to 20% faster, depending on workload". JIT included but off in Windows/macOS binaries (`PYTHON_JIT=1`) | [What's New 3.14](https://docs.python.org/3.14/whatsnew/3.14.html) |
| main, late 2025 | Same-commit JIT vs default geomeans: Linux x86-64 1.060 vs 1.063 (JIT slightly slower); Windows 1.120 vs 1.059 (JIT faster); macOS 1.064 vs 1.054 (ratios vs 3.13.0) | [faster-cpython/benchmarking-public](https://github.com/faster-cpython/benchmarking-public) |
| 3.15a (Jan 2026) | What's New draft said 3–4% geomean; individual benchmarks ranged from about 20% slower to more than 100% faster | [discuss thread 2026-01-12](https://discuss.python.org/t/python-3-15-jit-benchmarks/105678) |
| 3.15a (Mar 2026) | 5–6% on x86-64 Linux and 11–12% on macOS AArch64 against the tail-call interpreter. Gains came from trace recording and refcount elimination | [Ken Jin, Python blog 2026-03-23](https://blog.python.org/2026/03/jit-on-track/) |
| 3.15 (final 2026-10-09) | **7–8% geomean on x86-64 Linux; 11–12% on AArch64 macOS** (against the tail-call interpreter); per benchmark from about −15% to more than +100%, excluding `unpack_sequence`. Needs LLVM 21 at build time | [What's New 3.15](https://docs.python.org/3.15/whatsnew/3.15.html) |
| Jun 2026 | PEP 836: 4–12% geomean across Tier-1 platforms (ripley x86-64 1.069, sulaco aarch64 1.073, jones M3 1.126, Windows 1.047) | [PEP 836](https://peps.python.org/pep-0836/) |

- **Best benchmarks** (my calculation from doesjitgobrrr data, below): richards and richards_super (2.2–2.5×), nbody (about 1.5×), scimark_sor/lu (1.4–1.6×), spectral_norm and float (1.3–1.5×).
- **Worst benchmarks:** unpack_sequence (0.33–0.75×), then django_template, k_core, mdp and html5lib (0.7–0.9×).
- **Tier-2 without the JIT** (the uop interpreter, `PYTHON_UOPS` builds) is a debugging and validation path and is slower. The faster-cpython repo has pystats for it but publishes no headline speed-up.

## 2. Is the JIT "being pulled from 3.15"? No. It is being pulled from main (3.16).

- **2026-06-05:** The Steering Council asked that no new JIT development land on main until a standards-track PEP is accepted. If none is accepted within about six months, "the JIT code must be removed from the main branch". On the same day the SC said it was "okay with the status quo for 3.15". [SC announcement](https://discuss.python.org/t/an-announcement-from-the-steering-council-regarding-the-jit-project/107638)
- **2026-07-03:** PEP 836, "JIT Go Brrr" by Ostrowski, Ken Jin and Bucher, was posted for discussion. It targets 3.16 and proposes a method-based frontend. [PEP 836](https://peps.python.org/pep-0836/), [thread](https://discuss.python.org/t/pep-836-jit-go-brrr-the-path-to-a-supported-jit-compiler-for-cpython/108010)
- **2026-09-21:** The SC replied with ten points to address. It said it was judging whether the JIT may continue as an *experiment in main*, not whether it becomes a supported feature.
- **2026-10-19:** The authors withdrew PEP 836, citing bandwidth. [LWN 2026-10-26](https://lwn.net/SubscriberLink/1102102/489e41d80e4966aa/)
- **Outcome:** Brett Cannon said work in the main repository stops and any further work is left to the community. LWN expects removal from main "fairly soon", early in the 3.16 cycle.
- **3.15 shipped with the JIT** (experimental, off by default) and keeps getting fixes. There are 3.15 backports dated 2026-10-26.
- **[read, 2026-11-17]** `Python/jit.c` is still on main, and doesjitgobrrr still benchmarks JIT builds of `3.16.0a2+dev` (2026-11-16). I found no removal PR. The removal is announced but has not landed.

## 3. Benchmark suite and Python 3.7

- **Suite:** [pyperformance](https://github.com/python/pyperformance), run with pyperf. [doesjitgobrrr](https://doesjitgobrrr.com) and [its repo](https://github.com/savannahostrowski/doesjitgobrrr) run it nightly with extra benchmarks (about 116, including k_core and pycparser). Per-day JSON is at `https://doesjitgobrrr.com/data/runs/YYYY-MM-DD.json`.
- **Typical run:**
  ```
  pyperformance run --python=/path/python -b nbody,richards -o a.json
  pyperf compare_to a.json b.json
  ```
- **Version support on PyPI:**
  - pyperformance ≤1.11.0 (2024-03) declares `>=3.7`; 1.12.0+ needs `>=3.10`.
  - pyperf 2.8.0 is the last release for 3.7.
- **Caveat [read]:** from pyperformance 1.0.3 onwards, almost every benchmark's `pyproject.toml` says `requires-python >=3.8`. Only comprehensions and sqlglot say `>=3.7`. `cli.py` drops benchmarks that don't match the *harness's* own Python version, so a 3.7 harness runs almost nothing.
- **Options for 3.7:**
  - Use pyperformance 1.0.2 (2021-05, before the manifest).
  - Or run each `bm_X/run_benchmark.py` directly under 3.7 with pyperf 2.8.0 [inferred: works].
  - All 1.11.0 benchmark sources parse with the 3.7 grammar [read: `ast` with `feature_version=(3,7)`].
- **Worker processes:** pyperf runs each benchmark in fresh worker processes, so Ni must be enabled in each worker (for example via `sitecustomize`), not just in the parent.

Pure-Python benchmarks with no third-party packages (pyperformance 1.11.0; "C-heavy" means time is mostly spent in C stdlib):

| Benchmark | Extra packages | Notes |
|---|---|---|
| nbody, richards, richards_super, raytrace, chaos, fannkuch, float, spectral_norm, go, hexiom, deltablue, nqueens, meteor_contest, pyflate, scimark (sor/lu/fft/sparse/monte_carlo), generators, coroutines, comprehensions, deepcopy, mdp, unpack_sequence, gc_collect, gc_traversal, pickle_pure_python, unpickle_pure_python | none | Best JIT candidates (pure Python compute) |
| logging, pathlib, pprint, concurrent_imap, async_generators, async_tree (base and eager variants) | none | Mixed. `*_eager` needs 3.12 and `*_tg` needs 3.11 asyncio, so not usable on 3.7 |
| json_dumps, json_loads, pickle, pickle_dict, pickle_list, unpickle, unpickle_list, regex_compile, regex_dna, regex_effbot, regex_v8, telco, pidigits, sqlite_synth, xml_etree, asyncio_tcp(_ssl) | none | C-heavy |
| typing_runtime_protocols | none | Needs 3.8 (`typing.Protocol`) |
| python_startup(_no_site) | none | Startup only, not useful for a JIT |
| 2to3 | none (data bundled) | Uses lib2to3 |
| chameleon, crypto_pyaes (pyaes), coverage, dask, django_template (3.10+), docutils, dulwich_log, genshi, html5lib, mako, sqlalchemy_*, sqlglot*, sympy, tomli_loads, tornado_http, asyncio_websockets, hg_startup | yes | Pinned in each `requirements.txt` |

## 4. Same-build JIT/no-JIT numbers for pure-Python benchmarks

Speed-up ratios (no-JIT median ÷ JIT median, same commit, higher is better), from doesjitgobrrr, run 2026-09-25, commit 80f9aa0 (3.16.0a0). Machines:

- ripley: i5-8400, Ubuntu, x86-64
- jones: M3 Pro, macOS, against the tail-call interpreter
- sulaco: AmpereOne, aarch64

My geomean for ripley (1.065) matches the site's 1.07.

| bench | ripley | jones | sulaco |
|---|---|---|---|
| richards | 2.42 | 2.23 | 2.32 |
| nbody | 1.54 | 1.55 | 1.42 |
| spectral_norm | 1.39 | 1.30 | 1.48 |
| float | 1.33 | 1.30 | 1.34 |
| deltablue | 1.21 | 1.30 | 1.21 |
| pyflate | 1.19 | 1.23 | 1.20 |
| fannkuch | 1.17 | 1.21 | 1.05 |
| nqueens | 1.16 | 1.20 | 1.12 |
| chaos | 1.14 | 1.21 | 1.10 |
| go | 1.04 | 1.09 | 1.13 |
| raytrace | 1.02 | 1.18 | 1.11 |
| hexiom | 1.01 | 1.14 | 1.06 |
| generators | 0.96 | 0.98 | 0.93 |
| mdp | 0.93 | 0.91 | 0.94 |
| unpack_sequence | 0.57 | 0.70 | 0.75 |
| **geomean (all)** | **1.065** | **1.101** | **1.073** |

Other machines: a Raspberry Pi 5 geomean is about 0.99; Windows (JIT+tail-call) is 1.03 on 2026-09-25 and 1.06 on 2026-11-16. ripley on 2026-11-16 is 1.08.

## 5. Type information for a JIT: what shipped

**Shipped**

- **3.12: watchers and version tags.** `PyType_AddWatcher`, `PyDict_AddWatcher`, `PyFunction_AddWatcher`, `PyCode_AddWatcher` and `PyUnstable_Type_AssignVersionTag` are all `versionadded:: 3.12` [read: CPython `Doc/c-api/*.rst` on main]. They are invalidation hooks: they tell a JIT when a type, dict or function it depends on has changed. They do not describe types. They were largely driven by Cinder's needs. A type's `tp_version_tag` is what the specialiser and JIT guard on.
- **3.12: `CALL_INTRINSIC_1/2`.** This is unrelated to type information. It is an internal opcode that dispatches to a fixed table of helpers (`INTRINSIC_PRINT`, `IMPORT_STAR`, `LIST_TO_TUPLE`, `TYPEVAR`, and so on) to slim the bytecode [read: `Doc/library/dis.rst`]. It is not an API for type intrinsics.
- **3.13+: guards and type propagation in the tier-2 optimiser.**
  - The uop optimiser does abstract interpretation over symbols (known type, constant, null or not-null) in `Python/optimizer_bytecodes.c` and removes redundant guards such as `_GUARD_TYPE_VERSION`. This grew out of [faster-cpython/ideas#564](https://github.com/faster-cpython/ideas/issues/564) (Ken Jin and Jules Poon, 2023).
  - Size [read]: the 3.13 file is 812 lines with 34 type-set or type-new calls; on main it is 2,777 lines with 96.
  - The result types of builtin calls are known **only for hand-coded cases**: `_CALL_LEN`, `_CALL_ISINSTANCE`, `_CALL_TYPE_1`, `_CALL_STR_1`, `_CALL_TUPLE_1` and `_CALL_LIST_APPEND`. Generic `_CALL_BUILTIN_O/FAST` and method-descriptor calls only yield "not null" [read: main].
  - Main also has `_CALL_METHOD_DESCRIPTOR_NOARGS_INLINE`, which bakes in the C function pointer. The call is direct but still boxed.

**Stayed proposals**

- **Mining Argument Clinic types for JIT/boxing removal:** [faster-cpython/ideas#546](https://github.com/faster-cpython/ideas/issues/546), opened by Max Bernstein on 2023-01-23. Still open; it was prototyped in Cinder (`TypedMethodDef`) and PyPy (see 6).
- **Mark Shannon's "hasty call":** [faster-cpython/ideas discussion #374](https://github.com/faster-cpython/ideas/discussions/374), 2022-05-10. Callables would export a signature plus a "hasty" entry point. The caller does argument reordering and defaults, and the end goal is JIT code calling the low-level typed `foo_impl` directly. Not implemented.
- **Uncertain:** I found no Mark Shannon talk or post specifically about "type intrinsics" for builtins. The closest matches are #374 and #546. No PEP lets builtins or C functions declare argument or result types.

## 6. Unboxed or typed C extension calling interfaces

| Proposal | Who, when | Status |
|---|---|---|
| Vectorcall (PEP 590) | Shannon and Demeyer, 2019 | **Shipped 3.8** (public 3.9). Arguments are still `PyObject*` |
| "Hasty call" (typed signature + low-level impl) | Shannon, [ideas#374](https://github.com/faster-cpython/ideas/discussions/374), 2022-05 | Idea only |
| New C API "Native Interface" | Shannon, [markshannon/New-C-API-for-Python](https://github.com/markshannon/New-C-API-for-Python) (2022-08, last push 2025-08); [api-revolution#1](https://github.com/capi-workgroup/api-revolution/issues/1) (Viktorin, 2023-10, open) | Design only. Its generated `PyAPI.h` has a `FunctionDefinition { signature, impl (raw C pointer), call (vectorcall fallback) }` [read]; the intent to allow unboxed calls is [inferred]. Its schedule (spec 3.13, implementation 3.14) did not happen |
| "C API is weakly typed" | Shannon, [problems#31](https://github.com/capi-workgroup/problems/issues/31), 2023-05 | Problem statement (typed C handles, not unboxing) |
| HPy Argument-Clinic-style C signatures | Cuni, [hpy#129](https://github.com/hpyproject/hpy/issues/129), 2020-12 | Open. HPy's last release is **0.9.0 (2023-09-22)** and CPython has not adopted HPy |
| Typed methods: `METH_TYPED` with arg/ret type codes and a raw C function pointer | Bernstein and Bolz-Tereick, ["Dr Wenowdis"](https://arxiv.org/abs/2403.02420) (arXiv 2024-03, PLDI SOAP 2024) | **PyPy prototype only**. Up to about 60× on a call-heavy microbenchmark. Not proposed as a CPython PEP |
| PEP 793 `PyModExport`, PEP 803 abi3t | 2025 | **Accepted for 3.15** (module entry and ABI). No typed calls |
| PEP 809 "Stable ABI for the Future" | Dower, 2025 | Status not confirmed; it does not address unboxing |

**Bottom line:** no typed or unboxed C calling convention has landed in 3.12–3.15. What exists is invalidation (watchers, version tags) plus hand-written per-builtin knowledge in the tier-2 optimiser. For Ni, the Argument Clinic files (`*.c.h` and `clinic` input) remain the best machine-readable source of C-level signatures [inferred].
