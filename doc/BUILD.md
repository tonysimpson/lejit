---
written: 2026-11-16
head_ref: e1fd808
verified: true    # built and tested on 2026-11-16 (Debian 13, gcc 14, CPython 3.7.17); see STATUS.md
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
```
- `NI_ALL_STATIC=1` puts every `.c` file into one unit (`ni/ni.c`, `ni/initialize.h`).
- `setup.py:autodetect` accepts only `x86_64`, and adds `ni/x64` to the include path, so backend headers are included as `<iencoding.h>`.

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

**Check everything with one command:** `tools/check.sh`. It builds with `NI_TRACE=1 NI_ALL_CHECKS=1`, stops if the build fails or `ni.so` is older than any source, then runs `tests/run.py`, `tests/encoding/check.py`, the `v_mem_mov` execution tests, `tests/compile_bounds.py` (fails if warm code keeps compiling, via `ni._stats()`), `tests/cpython_tests.py` (50 of CPython's own test modules, counts and exit status must match) and `tests/smoke.py` (stdlib output with and without Ni). About two minutes. [run]

**Showing a new test fails without the fix (D21):** `tools/without_fix.sh <changed files> -- <command>`, e.g. `tools/without_fix.sh ni/mergepoints.c -- python3.7 tests/run.py -k framelocals`. It stashes those files' uncommitted changes, removes `ni.so` and rebuilds, runs the command, then restores everything. Stashing by hand just after a build can leave the fixed `ni.so` in place (same-second timestamps). [run]

**Rebuilds.** `setup.py` lists every `.c` and `.h` as a dependency, and removes `ni.so` when the build options change (`build/ni_config.stamp`), so header edits and switching between debug and optimised builds rebuild. distutils compares whole seconds, so an edit in the same second as the last build can still be missed; `tools/check.sh`'s staleness check catches that. [run]
