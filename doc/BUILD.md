---
written: 2026-11-16
verified: true    # built and tested on 2026-11-18 (Debian 13, gcc 14, CPython 3.7.17)
---
# Building, running, debugging

## Requirements
- **CPython 3.7, x86-64 Linux.** The code depends on 3.7 internals (see `REFERENCES.md`), so it won't compile against 3.8+ without porting.
- `libffi` (headers and library). `setup.py` links `-lffi`, and the backend uses it for C calls.
- `setup.py` uses `distutils`, which was removed in Python 3.12. That's fine under 3.7.
- CPython 3.7.17 via `pyenv install 3.7.17` (`uv` offers 3.8 at the earliest). libffi headers are present (`/usr/include/x86_64-linux-gnu/ffi.h`).
- Builds with gcc 10+ defaults (`-fno-common`): headers declare globals with `EXTERNVAR`, which is `extern` (KNOWN_BUGS C5).

## Build
```
NI_TRACE=1 NI_ALL_CHECKS=1 pip install -e .     # debug: -O0 -g3, extra asserts
pip install .                                   # release: -O3
python3.7 setup.py build_ext --inplace          # what tools/check.sh does
```
- `NI_ALL_STATIC=1` puts every `.c` file into one unit (`ni/ni.c`, `ni/initialize.h`, `ni/x64/iinitialize.h`), except `platform.c` and `Python/pyruntime.c`, which stay separate. With everything static, gcc's unused-function warnings list the dead code (about 30 functions, 2026-11-18). Checked build passes `tests/run.py` (C55, `c33b13f`). [run]
- `setup.py:autodetect` accepts only `x86_64`, and adds `ni/x64` to the include path, so backend headers are included as `<iencoding.h>`.

**Generated code (D67, D68).** Before compiling, `build_ext` runs `tools/metac/generate.py`, which writes one header per generator (`generate.py:GENERATORS`: `pycompiler_gen.h`, `pabstract_gen.h`, `plongobject_gen.h` and the rest), the virtual objects' headers (`gen_virtual.py`) and one header per mechanism (the snippets the source's answers choose, `mechanisms.HEADERS`, D75, D77) into `build/temp.<platform>-<X.Y>/gen/` (on the include path). Nothing generated is in git. What it needs:
- **A second Python, 3.10 or later, for the generators** (the vendored `cases_generator` needs it): `NI_GEN_PYTHON`, default `python3` on `PATH`. In a 3.7 virtualenv `python3` is 3.7, so set it (`NI_GEN_PYTHON=/usr/bin/python3`).
- **CPython's source for exactly the version being built for** (`sys.version`, e.g. `3.7.17`):
  - `NI_CPYTHON_SRC=DIR`: a source tree (unpacked tarball or checkout; a pyenv build keeps one in `~/.pyenv/sources/3.7.17/Python-3.7.17`). Its `Include/patchlevel.h` must name the same version.
  - Otherwise the release tarball: the first build fetches `https://www.python.org/ftp/python/X.Y.Z/Python-X.Y.Z.tgz` (about 24 MB), checks its sha256 against `tools/metac/cpython_sha256.txt`, and unpacks it into `~/.cache/ni/cpython/Python-X.Y.Z/` (`NI_CACHE_DIR`, else `$XDG_CACHE_HOME/ni`). Later builds use the cache. A version without a recorded sha256 fails: add the line, with the hash from a source other than the download (pyenv's `python-build` definitions have them), or use `NI_CPYTHON_SRC`.
- A wrong version, a missing tree, a `report.txt` that differs from `tools/metac/expected/<version>.txt` (accept by copying it over, INVENTORY.md), a failed download, a bad checksum, a policy table that doesn't match the source (`tools/metac/policy.py`: a key not found, a changed shape, a value outside the vocabulary; D71) or a generator error (for example `gen_ceval`'s checks against `ceval.c` and `dis.stack_effect`), or mechanism answers with no snippet (`tools/metac/mechanisms.py:VARIANTS`) fails the build with the reason, and removes the old generated headers. An old `ni/**/*_gen.h` left in a checkout also fails the build: `"x.h"` would find it first.
- To see which mechanisms a source tree selects: `python3 tools/metac/mechanisms.py SRC...`; `python3 tests/mechanisms.py` checks the answers on 3.7, 3.8, 3.11, 3.12 and 3.13 (fetching any tarball not cached) and that unmapped answers fail generation. A variant not written yet stops the compiler with `#error "... not written yet"`.
- **Generation cache (D109).** `tools/metac/cache.py` keeps pickled results in `~/.cache/ni/gen/` (beside the CPython tarballs: `NI_CACHE_DIR`, else `$XDG_CACHE_HOME/ni`), each file named by the sha256 of what it depends on: `pp/` (a file's preprocessing: its bytes, any rules' C, the gcc command and `gcc -v`, the vocabulary and its shim, `front.py`, the prelude and the vendored pycparser; checked against the hash of every header its line markers name, `pyconfig.h` included), `text/` (the preprocessed text, by its hash), `ast/` (pycparser's AST, by the text's hash and the same code) and `effects/` (`effects.analyse`: every scanned file's text hash, the policy rules' contents, `effects.py`, `gen2/vocabulary.py`, the front end). A change to any of these invalidates just what depends on it; nothing else needs clearing. Not seen: a header newly added where the include path would find it before the one used. Bypass with `generate.py --no-cache` or `NI_NO_CACHE=1` (also for `setup.py` and `check.sh`): the headers are the same either way. Writes are atomic, a damaged entry is rebuilt, and entries unused for 14 days are removed; one CPython version takes about 530 MB. Generation on 3.7.17: about 63 s with no cache, 75 s filling it, 7 s from it. [run]
- To run the generators alone: `python3 tools/metac/generate.py --python ~/.pyenv/versions/3.7.17/bin/python3.7 --out DIR [--src DIR] [-v]` (`-v` lists what each generated and skipped, with the reason; `DIR/report.txt` has each function's status).

## Run
`import ni` installs the frame-eval hook immediately (`ni/eval_hook.c`). From then on, every non-module function whose opcodes are all in `ni/mergepoints.c` is compiled on its first call. There's no Python-level API (`module_methods` is empty). The README says only very simple functions work, for example integer loops and `a + b`.

## Benchmarks
`tools/benchmarks.sh` builds for release and runs the three suites: `tools/bench.py` (small kernels), `tools/pyperf37.py` (pyperformance) and `tools/compilebench.py` (compile time, code size, break-even). Name suites to run only those (`tools/benchmarks.sh pyperf`) and use `-k name` to filter. Each run is kept in `build/bench/<date>-<commit>.txt`, marked `-dirty` with uncommitted changes. All three suites take about 15 minutes. A release build is left behind; `tools/check.sh` rebuilds for debugging.

## Debug
- **nianalyser** (`tools/nianalyser`): runs the target under `pointbreak`, sets breakpoints on the empty `ni_trace_*` functions in `ni/ni.c`, and records emitted code, jumps, calls and unifications, disassembled with `distorm3`. Needs `pointbreak` (`~/work/pointbreak`, which needs `libunwind-dev` to build), `distorm3` and `intervaltree`. Setup (2026-11-16): `sudo apt install libunwind-dev`, then make a venv outside both repos and install pointbreak from a copy (pip builds in-tree, and pointbreak is maintained separately):
  ```
  git -C ~/work/pointbreak archive HEAD | tar -x -C /some/dir/pb
  uv venv --python 3.12 venv && VIRTUAL_ENV=$PWD/venv uv pip install /some/dir/pb distorm3 intervaltree
  venv/bin/python tools/nianalyser/dump_function.py FUNC ~/.pyenv/versions/3.7.17/bin/python3.7 script.py
  ```
  `dump_function.py` prints every chunk emitted for one function, annotated with its line, bytecode offset and compiler call stack. Promotion records sit inline in the code and disassemble as junk.
- **Crashes in emitted code: `crash_report.py`**, not gdb (D28). `venv/bin/python tools/nianalyser/crash_report.py python3.7 script.py` runs to the fatal signal and reports the fault, the emitted chunk holding it (line, bytecode offset, compiler stack), the stack words that point into emitted code, and the registers. Its first line says how many times compiled code was entered (`psyco_processor_run`). Rerun with `--step-from K` (K = that count, or a little less) to single-step from that entry and print the chunks and instructions that ran just before the crash: that is what shows a bad jump or return. Needs an `NI_TRACE=1` build; an `NI_ALL_CHECKS=1` build can hide a crash (F31 did).
- Without pointbreak: `ni._entry_point(f.__code__)[0]` is a CodeBuffer (`[1]` is the address of its globals) whose `codestart` pointer is at offset 16; read it with ctypes and disassemble with `objdump -b binary -m i386:x86-64`.
- `tools/test_harness`: counts `NiEval_EvalFrame` against `NiCode_Run` hits, to see how many frames actually ran compiled code.
- `tools/test_runner`: unittest runner that imports `ni` when `NI_ENABLED` is set.
- `tools/asm`, `tools/test_asm`, `tools/test_c_encoding`, `tools/asm_test_cases.txt`, `tools/test_mcg_template.c`: check that the C encoding macros in `ni/x64/iencoding.h` emit the same bytes as gcc's assembler.

## Tests
Upstream Psyco's test suite (`test/`) and the Ni regression pack were removed (see `doc/history/removed-files.md`).

### Running
- Build Python 3.7.17 once: `pyenv install 3.7.17` (worked as-is on Debian 13 with gcc 14).
- Build Ni in place: `NI_TRACE=1 NI_ALL_CHECKS=1 ~/.pyenv/versions/3.7.17/bin/python3.7 setup.py build_ext --inplace`.
- Behaviour: `tests/run.py [-k name] [-v]`. Each case in `tests/cases/*.py` runs with and without Ni in fresh processes, and must compile (or declare `interpreted`). Expected failures name a KNOWN_BUGS id.
- Encoding: `tests/encoding/check.py [-k MACRO] [-v]`. Checks every `/* @asm: ... */`-annotated emitter in `ni/x64/iencoding.h` against GNU `as`, over its operand domains (registers, sizes, addressing forms). The annotation states what the code is *meant* to emit, so a mismatch is a bug. The cases are compiled in files of 15000 (`check.py:PART`), each gcc under 0.7 GB: as one file they needed over 16 GB. It takes about 25 s. `tests/encoding/run_v_mem_mov.sh` runs the `v_mem_mov` execution tests. It passes constant offsets, so it can't catch an emitter that computes a RIP-relative offset at the wrong moment.

### Still planned
- **Lint for unannotated emission.** A grep-level check that every site writing bytes (`WRITE_*`, `*code++`) is inside an annotated emitter, so nothing escapes the encoder tests.
- **Run-time check through pointbreak.** Optionally, emitters record (address, intent) as code is generated, and pointbreak or nianalyser disassembles the buffer and compares each instruction with its intent. This covers the operand combinations that only occur in real compilations.

## Notes
- CPython's sysconfig CFLAGS include `-DNDEBUG`, so plain `assert()` in the backend is compiled out. Some of those asserts are wrong (`doc/KNOWN_BUGS.md` B9). Psyco's own checks are `extra_assert`, enabled by `NI_ALL_CHECKS=1`.
- To prove an edit is comment-only without building, strip comments and compare against git:
  `gcc -fpreprocessed -E -P -x c FILE | tr -d ' \t\n' | md5sum`, compared with the same for `git show HEAD:FILE`.

**Check everything with one command:** `tools/check.sh`. It runs the generators twice into `build/gen-check/` and requires the same output, then builds with `NI_TRACE=1 NI_ALL_CHECKS=1`, stops if the build fails or `ni.so` is older than any source, checks the build compiled the same generated headers, then runs `tests/run.py`, `tests/encoding/check.py`, the `v_mem_mov` execution tests, `tests/compile_bounds.py` (fails if warm code keeps compiling, via `ni._stats()`), `tests/cpython_tests.py` (285 of CPython's own test modules, counts and exit status must match; below) and `tests/smoke.py` (stdlib output with and without Ni). About two minutes. [run]

**CPython's own tests (`tests/cpython_tests.py`).** All 416 modules in `lib/python3.7/test/` are in one of three lists. `MODULES` (285, the default, run by `tools/check.sh`): fast modules, mostly pure Python. `EXTENDED` (86, added by `--all`): slower ones, or ones using subprocesses, sockets, threads or timing, which a loaded machine can upset. `EXCLUDED` (45), with a reason each: GUI or terminal, network or downloads, other platforms, very slow or resource-hungry, test infrastructure, and one Ni class: `test_sys` (a C11 failure in `test_current_frames` leaves its thread waiting, so the run never exits). Each module runs plain and with Ni in fresh interpreters, three at a time (`-j N`), each in a temporary directory with a 600 s timeout and a 4 GB address-space limit (`MEMORY_LIMIT`), so a runaway gets MemoryError instead of the machine. `KNOWN` accepts a module's exact result under Ni with the KNOWN_BUGS id that explains it. The default set takes about 50 s on a checked build, `--all` about 3 minutes more. Usage: `python3.7 tests/cpython_tests.py [-j N] [--all | module ...]`. [run]

**Surveying modules** (how the lists were made, 2026-11-17): run every module plain and with Ni under the same limits, first with the release build, then the checked one; compare the result lines; for a difference, list the failing tests (a runner that prints `unittest` failures with tracebacks), then run each test of the module alone to find a crash, a hang or memory growth (`resource.getrusage(...).ru_maxrss` per test). Crashes in Ni's own C code show in `tools/nianalyser/crash_report.py` as a fault outside emitted code (F45 was in `PsycoTuple_Load`). [run]

**Other projects' test suites (`tools/thirdparty/`).** `python3.7 tools/thirdparty/thirdparty.py run [-j N] [--mode plain|ni] [project ...]` runs the suites of the projects in `projects.py` (pure Python, each at its last release for 3.7) plainly and with Ni, and reports the tests that pass plainly but fail, error, crash or hang with Ni, and the reverse, with each run's time (`build/thirdparty/report.txt`; tracebacks in `report-details.txt`). Each project has a venv under `build/thirdparty/<project>/` holding exactly the wheels `lock.txt` names, installed with `pip --require-hashes --no-deps`; its tests come from its sdist (or its release archive), checked against the sha256 in `lock.txt` and unpacked afresh per run. A `.pth` file imports Ni when `NI_THIRDPARTY=1`, so the whole test process and its subprocesses run with Ni; the report gives Ni's code-buffer count to show it ran. pytest runs with `ni_tp_pytest.py` (one JSON line per test phase, Hypothesis made deterministic with no deadline, a 300 s per-test timeout), Django with its `runtests.py` and `ni_tp_django.py`. A run that crashes or times out is restarted without the tests already done and the one it was in, which is reported as a crash or hang. Every process has a 4 GB address-space limit; `-j 2` (the default) runs the two modes at once. `thirdparty.py lock` re-resolves `projects.py` into `lock.txt` from PyPI; `report` compares the last runs again. Needs network access only for `lock` and the first `run`. [run]

**Showing a new test fails without the fix (D21):** `tools/without_fix.sh <changed files> -- <command>`, e.g. `tools/without_fix.sh ni/mergepoints.c -- python3.7 tests/run.py -k framelocals`. It stashes those files' uncommitted changes, removes `ni.so` and rebuilds, runs the command, then restores everything. Stashing by hand just after a build can leave the fixed `ni.so` in place (same-second timestamps). [run]

**Rebuilds.** `setup.py` lists every `.c` and `.h` as a dependency, and removes `ni.so` when the build options change (`build/ni_config.stamp`), so header edits and switching between debug and optimised builds rebuild. distutils compares whole seconds, so an edit in the same second as the last build can still be missed; `tools/check.sh`'s staleness check catches that. [run]

### Sanitizers

`tools/sanitize.sh [run] [bounds] [cpython | cpython-all] [smoke]` builds Ni with gcc's AddressSanitizer and UBSan (`NI_SANITIZE=address,undefined`, with `NI_TRACE=1 NI_ALL_CHECKS=1`, `-O1 -g -fno-omit-frame-pointer`) into `build/asan/`, runs the named suites (default all but `cpython-all`) under it, and lists the reports deduplicated by kind and top four frames (`tools/sanitize_report.py LOGDIR [--frames N]`; the full text of each is in `LOGDIR/report.txt`, logs in `build/asan/logs/<time>-<pid>/`). A report that a plain run also gives is listed apart: CPython's tests raise `Py_FatalError` and faulthandler's signals in subprocesses on purpose. Exit status 0 only with no report from Ni and every suite passing. About 5 minutes for the behaviour tests, 4 for CPython's default set, 15 for `cpython-all`. [run]
- `setup.py` builds with `NI_SANITIZE` only into `--build-lib` (it refuses `--inplace`) and keeps that build's config stamp in `build/asan/`, so the in-place build is left alone. The test runners find it through `NI_MODULE_DIR`, and `NI_SANITIZE_LOGS` gives each child its own log (`tests/testenv.py`); unset, they behave as before.
- CPython 3.7.17 isn't instrumented: gcc's `libasan.so` and `libubsan.so` are preloaded, and `PYTHONMALLOC=malloc` sends CPython's allocations through ASan's allocator, so a stale `PyObject *` in Ni is caught like any heap use after free.
- Ni's own allocators: under ASan each `blockalloc.h` item (vinfo, source_known, the dispatcher's vci links) is its own `PyMem_MALLOC` block, and the code manager poisons free extents of executable memory (`NI_POISON` in `psyco.h`). The machine code Ni emits is not instrumented: its stray accesses show only when they reach C code, or as a SEGV.
- UBSan's alignment check is off (`-fno-sanitize=alignment`): the encoder writes immediates at any byte address. CPython's `-fwrapv` makes signed overflow defined. gcc's `libubsan` ignores `log_path` when preloaded beside `libasan`, so the runners keep a child's stderr when it holds a UBSan report.
- Children run with no address-space limit (ASan reserves terabytes) but in a systemd user scope with `MemoryMax=6G` (`NI_SANITIZE_MEM`), CPython's tests two at a time, with timeouts four times the usual. `NI_ASAN_OPTIONS` adds ASan options (`detect_stack_use_after_return=1` found nothing more). [run]
- `test_threading` is skipped (`testenv.py:SANITIZE_SKIP`): with Ni, `test_is_alive_after_fork`'s forked child waits for good on a size-class lock of ASan's allocator (the region at `0x540000000000`), taken by another thread at fork time. It passes without ASan, and alone under ASan (QUESTIONS 52). [run]
- Under `PYTHONMALLOC=malloc`, `sys.getallocatedblocks()` stays 0, so `tests/run.py`'s leak check passes vacuously there. That check, on the normal build, is the leak test that works; LeakSanitizer (`NI_SANITIZE_LEAKS=1`, reports only, with Ni in one of the first three frames past the allocator) lists about 140 groups at exit, nearly all false: the glue code buffers and the guard caches are kept for good (C14), and records held by emitted code are found only through pointers written as `MOV r64, imm64` at unaligned addresses, which LeakSanitizer doesn't scan, though executable memory is registered as a root region. It found one real leak (`dfe1a39`), absent from the code by the test's count too. [run]

### Code size counters

`NI_CODESTATS=1 python3.7 setup.py build_ext --inplace` builds in per-instruction and per-buffer code size counters (`ni._codestats()`); `tools/codesize.py <benchmark> [-d]` reports them (`doc/FINDINGS.md`). `-d` needs capstone: `python3.7 -m pip install --target <dir> capstone`, then `PYTHONPATH=<dir>`.

**Same code:** `tools/samecode.py A B` builds two revisions (or takes two `NI_CODESTATS` builds) and says per code object and instruction whether they emit the same machine code, with addresses named (FINDINGS.md "Same code"). About 2 minutes for the tests and the seven benchmarks. [run]

