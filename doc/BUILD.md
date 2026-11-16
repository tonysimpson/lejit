---
written: 2026-11-16
head_ref: e1fd808
verified: false   # not built during indexing; facts below are from setup.py, README and the code
---
# Building, running, debugging

## Requirements
- **CPython 3.7, x86-64 Linux.** The code depends on 3.7 internals (see `REFERENCES.md`), so it won't compile against 3.8+ without porting.
- `libffi` (headers and library). `setup.py` links `-lffi`, and the backend uses it for C calls.
- `setup.py` uses `distutils`, which was removed in Python 3.12. That's fine under 3.7.
- No Python 3.7 was installed on the owner's machine at indexing time (system Python is 3.13). `uv` offers 3.8 at the earliest, not 3.7. `pyenv` is installed and can build 3.7 from source. libffi headers are present (`/usr/include/x86_64-linux-gnu/ffi.h`).

## Build
```
NI_TRACE=1 NI_ALL_CHECKS=1 pip install -e .     # debug: -O0 -g3, extra asserts
pip install .                                   # release: -O3
```
- `NI_ALL_STATIC=1` puts every `.c` file into one unit (`ni/ni.c`, `ni/initialize.h`).
- `setup.py:autodetect` accepts only `x86_64`, and adds `ni/x64` to the include path, so backend headers are included as `<iencoding.h>`.

## Run
`import ni` installs the frame-eval hook immediately (`ni/eval_hook.c`). From then on, every non-module function whose opcodes are all in `ni/mergepoints.c` is compiled on its first call. There's no Python-level API (`module_methods` is empty). The README says only very simple functions work, for example integer loops and `a + b`.

## Debug
- **nianalyser** (`tools/nianalyser`): runs the target under `pointbreak`, sets breakpoints on the empty `ni_trace_*` functions in `ni/ni.c`, and records emitted code, jumps, calls and unifications, disassembled with `distorm3`. Needs `pointbreak` (`~/work/pointbreak`), `distorm3` and `intervaltree`.
- `tools/test_harness`: counts `NiEval_EvalFrame` against `NiCode_Run` hits, to see how many frames actually ran compiled code.
- `tools/test_runner`: unittest runner that imports `ni` when `NI_ENABLED` is set.
- `tools/asm`, `tools/test_asm`, `tools/test_c_encoding`, `tools/asm_test_cases.txt`, `tools/test_mcg_template.c`: check that the C encoding macros in `ni/x64/iencoding.h` emit the same bytes as gcc's assembler.
- gdb: build with `NI_TRACE=1`. Breakpoints on `ni_trace_*` are the natural hooks.

## Tests
Upstream Psyco's test suite (`test/`) and the Ni regression pack were removed (see `doc/history/removed-files.md`). There is currently no automated test suite.

### Planned for the 3.7 baseline (D18); not built yet
- **Encoder tests from intent annotations.** Each emitter (the `iencoding.h` macros, each emission branch in `v_mem_mov.h`) gets a one-line annotation giving the intended instruction in explicit-size syntax, for example `/* @asm: cmp qword [%base + %off], %imm8 */`. A script collects the annotations and instantiates each emitter over its operand domains: every allocatable and transient register, every size and signedness, every addressing form. It then compares the bytes with GNU `as` assembling the same intent. The annotation states what the code is *meant* to emit, so a mismatch is a bug. The current `tools/asm_test_cases.txt` only records what the code does: its `cmp $0,0x20(%rax)` has no size suffix, so `as` picks 32 bits and the test enshrines bug B1.
- **Lint for unannotated emission.** A grep-level check that every site writing bytes (`WRITE_*`, `*code++`) is inside an annotated emitter, so nothing escapes the encoder tests.
- **Run-time check through pointbreak.** Optionally, emitters record (address, intent) as code is generated, and pointbreak or nianalyser disassembles the buffer and compares each instruction with its intent. This covers the operand combinations that only occur in real compilations.
- **Differential tests.** Each test runs under CPython and under Ni in a subprocess with a timeout. Results, exceptions and output must match, and the test must assert that the function really was compiled. Crashes are analysed with pointbreak (`~/work/pointbreak`, maintained separately).

## Notes
- CPython's sysconfig CFLAGS include `-DNDEBUG`, so plain `assert()` in the backend is compiled out. Some of those asserts are wrong (`doc/KNOWN_BUGS.md` B9). Psyco's own checks are `extra_assert`, enabled by `NI_ALL_CHECKS=1`.
- To prove an edit is comment-only without building, strip comments and compare against git:
  `gcc -fpreprocessed -E -P -x c FILE | tr -d ' \t\n' | md5sum`, compared with the same for `git show HEAD:FILE`.
