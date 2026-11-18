---
written: 2026-11-16
head_ref: 9eae61d
verified: true    # built and tested on 2026-11-18 (Debian 13, gcc 14, CPython 3.7.17); see STATUS.md
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
- `NI_ALL_STATIC=1` puts every `.c` file into one unit (`ni/ni.c`, `ni/initialize.h`).
- `setup.py:autodetect` accepts only `x86_64`, and adds `ni/x64` to the include path, so backend headers are included as `<iencoding.h>`.

**Generated code (D67, D68).** Before compiling, `build_ext` runs `tools/metac/generate.py`, which writes `plongobject_gen.h`, `pfloatobject_gen.h`, `pycompiler_gen.h` and the three `mechanism_*_gen.h` (the snippets the source's answers choose, D75) into `build/temp.<platform>-<X.Y>/gen/` (on the include path). Nothing generated is in git. What it needs:
- **A second Python, 3.10 or later, for the generators** (the vendored `cases_generator` needs it): `NI_GEN_PYTHON`, default `python3` on `PATH`. In a 3.7 virtualenv `python3` is 3.7, so set it (`NI_GEN_PYTHON=/usr/bin/python3`).
- **CPython's source for exactly the version being built for** (`sys.version`, e.g. `3.7.17`):
  - `NI_CPYTHON_SRC=DIR`: a source tree (unpacked tarball or checkout; a pyenv build keeps one in `~/.pyenv/sources/3.7.17/Python-3.7.17`). Its `Include/patchlevel.h` must name the same version.
  - Otherwise the release tarball: the first build fetches `https://www.python.org/ftp/python/X.Y.Z/Python-X.Y.Z.tgz` (about 24 MB), checks its sha256 against `tools/metac/cpython_sha256.txt`, and unpacks it into `~/.cache/ni/cpython/Python-X.Y.Z/` (`NI_CACHE_DIR`, else `$XDG_CACHE_HOME/ni`). Later builds use the cache. A version without a recorded sha256 fails: add the line, with the hash from a source other than the download (pyenv's `python-build` definitions have them), or use `NI_CPYTHON_SRC`.
- A wrong version, a missing tree, a failed download, a bad checksum, a policy table that doesn't match the source (`tools/metac/policy.py`: a key not found, a changed shape, a value outside the vocabulary; D71) or a generator error (for example `gen_ceval`'s checks against `ceval.c` and `dis.stack_effect`), or mechanism answers with no snippet (`tools/metac/mechanisms.py:VARIANTS`) fails the build with the reason, and removes the old generated headers. An old `ni/**/*_gen.h` left in a checkout also fails the build: `"x.h"` would find it first.
- To see which mechanisms a source tree selects: `python3 tools/metac/mechanisms.py SRC...`; `python3 tests/mechanisms.py` checks the answers on 3.7, 3.8, 3.11, 3.12 and 3.13 (fetching any tarball not cached) and that unmapped answers fail generation. A variant not written yet stops the compiler with `#error "... not written yet"`.
- To run the generators alone: `python3 tools/metac/generate.py --python ~/.pyenv/versions/3.7.17/bin/python3.7 --out DIR [--src DIR] [-v]` (`-v` lists what each generated and skipped, with the reason).

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
- **Crashes in emitted code: `crash_report.py`**, not gdb (D28). `venv/bin/python tools/nianalyser/crash_report.py python3.7 script.py` runs to the fatal signal and reports the fault, the emitted chunk holding it (line, bytecode offset, compiler stack), the stack words that point into emitted code, and the registers. Its first line says how many times compiled code was entered (`psyco_processor_run`). Rerun with `--step-from K` (K = that count, or a little less) to single-step from that entry and print the chunks and instructions that ran just before the crash: that is what shows a bad jump or return. Needs an `NI_TRACE=1` build; an `NI_ALL_CHECKS=1` build can hide a crash (F31 did). Pointbreak pain points go in `doc/pointbreak-journal.md`.
- Without pointbreak: `ni._entry_point(f.__code__)[0]` is a CodeBuffer (`[1]` is the address of its globals) whose `codestart` pointer is at offset 16; read it with ctypes and disassemble with `objdump -b binary -m i386:x86-64`.
- `tools/test_harness`: counts `NiEval_EvalFrame` against `NiCode_Run` hits, to see how many frames actually ran compiled code.
- `tools/test_runner`: unittest runner that imports `ni` when `NI_ENABLED` is set.
- `tools/asm`, `tools/test_asm`, `tools/test_c_encoding`, `tools/asm_test_cases.txt`, `tools/test_mcg_template.c`: check that the C encoding macros in `ni/x64/iencoding.h` emit the same bytes as gcc's assembler.

## Tests
Upstream Psyco's test suite (`test/`) and the Ni regression pack were removed (see `doc/history/removed-files.md`). New suites were added on 2026-11-16 (below).

### Running
- Build Python 3.7.17 once: `pyenv install 3.7.17` (worked as-is on Debian 13 with gcc 14).
- Build Ni in place: `NI_TRACE=1 NI_ALL_CHECKS=1 ~/.pyenv/versions/3.7.17/bin/python3.7 setup.py build_ext --inplace`.
- Behaviour: `tests/run.py [-k name] [-v]`. Each case in `tests/cases/*.py` runs with and without Ni in fresh processes, and must compile (or declare `interpreted`). Expected failures name a KNOWN_BUGS id.
- Encoding: `tests/encoding/check.py [-k MACRO] [-v]`. Checks every `/* @asm: ... */`-annotated emitter in `ni/x64/iencoding.h` against GNU `as`. It takes about a minute.

### Design notes (some still planned)
- **Encoder tests from intent annotations.** Each emitter (the `iencoding.h` macros, each emission branch in `v_mem_mov.h`) gets a one-line annotation giving the intended instruction in explicit-size syntax, for example `/* @asm: cmp qword [%base + %off], %imm8 */`. A script collects the annotations and instantiates each emitter over its operand domains: every allocatable and transient register, every size and signedness, every addressing form. It then compares the bytes with GNU `as` assembling the same intent. The annotation states what the code is *meant* to emit, so a mismatch is a bug. The current `tools/asm_test_cases.txt` only records what the code does: its `cmp $0,0x20(%rax)` has no size suffix, so `as` picks 32 bits and the test enshrines bug B1.
- **Lint for unannotated emission.** A grep-level check that every site writing bytes (`WRITE_*`, `*code++`) is inside an annotated emitter, so nothing escapes the encoder tests.
- **Run-time check through pointbreak.** Optionally, emitters record (address, intent) as code is generated, and pointbreak or nianalyser disassembles the buffer and compares each instruction with its intent. This covers the operand combinations that only occur in real compilations.
- **Differential tests.** Each test runs under CPython and under Ni in a subprocess with a timeout. Results, exceptions and output must match, and the test must assert that the function really was compiled. Crashes are analysed with pointbreak (`~/work/pointbreak`, maintained separately).

## Notes
- CPython's sysconfig CFLAGS include `-DNDEBUG`, so plain `assert()` in the backend is compiled out. Some of those asserts are wrong (`doc/KNOWN_BUGS.md` B9). Psyco's own checks are `extra_assert`, enabled by `NI_ALL_CHECKS=1`.
- To prove an edit is comment-only without building, strip comments and compare against git:
  `gcc -fpreprocessed -E -P -x c FILE | tr -d ' \t\n' | md5sum`, compared with the same for `git show HEAD:FILE`.

**Check everything with one command:** `tools/check.sh`. It runs the generators twice into `build/gen-check/` and requires the same output, then builds with `NI_TRACE=1 NI_ALL_CHECKS=1`, stops if the build fails or `ni.so` is older than any source, checks the build compiled the same generated headers, then runs `tests/run.py`, `tests/encoding/check.py`, the `v_mem_mov` execution tests, `tests/compile_bounds.py` (fails if warm code keeps compiling, via `ni._stats()`), `tests/cpython_tests.py` (285 of CPython's own test modules, counts and exit status must match; below) and `tests/smoke.py` (stdlib output with and without Ni). About two minutes. [run]

**CPython's own tests (`tests/cpython_tests.py`).** All 416 modules in `lib/python3.7/test/` are in one of three lists. `MODULES` (285, the default, run by `tools/check.sh`): fast modules, mostly pure Python. `EXTENDED` (86, added by `--all`): slower ones, or ones using subprocesses, sockets, threads or timing, which a loaded machine can upset. `EXCLUDED` (45), with a reason each: GUI or terminal, network or downloads, other platforms, very slow or resource-hungry, test infrastructure, and one Ni class: `test_sys` (a C11 failure in `test_current_frames` leaves its thread waiting, so the run never exits). The C24 modules came back when compiled code began testing `eval_breaker` (D45): `test_unittest` to the default set, `test_signal`, `test_threading`, `test_io` and `test_asyncio` to `--all` (`test_unittest` and `test_asyncio` with accepted C11 differences; F50 and F52 are fixed). The C23 modules came back when compiled calls began testing for tracing (D46): `test_sys_settrace`, `test_sys_setprofile`, `test_bdb` and `test_profile` to the default set, `test_pdb`, `test_cprofile` and `test_trace` to `--all` (`test_sys_settrace` and `test_pdb` with accepted C29 differences: a frame already running compiled gets no events). Two crashes `--all` found then, `test_timeit` (C30) and `test_shutil` (C31), are fixed (`63b4f95`, `5a39986`) and both modules match plain. `test_dynamic` (F49) and `test_subclassinit` (F51) match since `5f8b506` and `125cf3d`; `test_socket`, excluded while F51 aborted it, is in `--all` since `125cf3d`, accepted as differing (C19 in `testRefCountGetNameInfo`) with no expected counts, since its skips and errors depend on the network. Each module runs plain and with Ni in fresh interpreters, three at a time (`-j N`), each in a temporary directory with a 600 s timeout and a 4 GB address-space limit (`MEMORY_LIMIT`), so a runaway gets MemoryError instead of the machine. `KNOWN` accepts a module's exact result under Ni with the KNOWN_BUGS id that explains it. Timings on 2026-11-17, checked build, with other agents' jobs on the machine: the default set about 50 s (the old 55-module set took about 20 s of process time, run one after another), `--all` about 3 minutes more. Usage: `python3.7 tests/cpython_tests.py [-j N] [--all | module ...]`. [run]. `test_capi` joined `--all` when C25 was fixed; `test_compare` and `test_iter` match plain since F10, F48 and F50.

**Surveying modules** (how the lists were made, 2026-11-17): run every module plain and with Ni under the same limits, first with the release build, then the checked one; compare the result lines; for a difference, list the failing tests (a runner that prints `unittest` failures with tracebacks), then run each test of the module alone to find a crash, a hang or memory growth (`resource.getrusage(...).ru_maxrss` per test). Crashes in Ni's own C code show in `tools/nianalyser/crash_report.py` as a fault outside emitted code (F45 was in `PsycoTuple_Load`). [run]

**Showing a new test fails without the fix (D21):** `tools/without_fix.sh <changed files> -- <command>`, e.g. `tools/without_fix.sh ni/mergepoints.c -- python3.7 tests/run.py -k framelocals`. It stashes those files' uncommitted changes, removes `ni.so` and rebuilds, runs the command, then restores everything. Stashing by hand just after a build can leave the fixed `ni.so` in place (same-second timestamps). [run]

**Rebuilds.** `setup.py` lists every `.c` and `.h` as a dependency, and removes `ni.so` when the build options change (`build/ni_config.stamp`), so header edits and switching between debug and optimised builds rebuild. distutils compares whole seconds, so an edit in the same second as the last build can still be missed; `tools/check.sh`'s staleness check catches that. [run]

### Code size counters

`NI_CODESTATS=1 python3.7 setup.py build_ext --inplace` builds in per-instruction and per-buffer code size counters (`ni._codestats()`); `tools/codesize.py <benchmark> [-d]` reports them (`doc/CODE_SIZE.md`). `-d` needs capstone: `python3.7 -m pip install --target <dir> capstone`, then `PYTHONPATH=<dir>`.

**Same code:** `tools/samecode.py A B` builds two revisions (or takes two `NI_CODESTATS` builds) and says per code object and instruction whether they emit the same machine code, with addresses named (CODE_SIZE.md "Same code"). About 2 minutes for the tests and the seven benchmarks. [run]

