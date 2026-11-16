# Status

## 2026-11-16: 3.7 baseline (branch `baseline-3.7`, D18)

Ni builds and runs on CPython 3.7.17 (pyenv, gcc 14, `-fcommon`). Three test suites, all passing:
- `tests/run.py`: 58 behaviour cases, each run with and without Ni, required to compile, and leak-checked. There are 4 expected failures, all bugs in the translated layer: F1 ×2 (mixed-type binary operations raise TypeError), F4 (catching an exception segfaults) and F5 (float division by zero). Two cases correctly fall back to the interpreter because of unsupported opcodes.
- `tests/encoding/check.py`: 56 `@asm`-annotated emitters, about 192k operand combinations, compared with GNU `as`.
- `tests/encoding/run_v_mem_mov.sh`: about 90k executed loads and stores through `v_mem_mov`.

Fixed (one commit each; details in KNOWN_BUGS):
- core: C4 (merge-point lookup ran past its sentinel), C6 (every compiled call leaked its frame), M6 (4-byte compile-time field reads);
- backend: B1, B11–B13 (emitter encodings), M1–M5 and M9 (`v_mem_mov`), B2 (uncounted push), B3 (double/int argument registers);
- translated, under the D18 exception: F16 (a leak in float comparison that crashed every float compare).

Inspected the generated code for a simple loop with objdump: type promotions with inline caches, then residual calls to `long_add` and the comparison functions (integers aren't virtual yet, D13). nianalyser wasn't run: pointbreak needs `libunwind-dev`, which isn't installed.

Left alone per D18 (translated layer, to be regenerated): F1–F15 except F16, notably F4 (exceptions) and the opcodes that keep Python 2 meanings. Open backend and core items: B5–B10, M8, C2, C3, C5 (use `extern` properly instead of `-fcommon`).

## 2026-11-16: first indexing pass (at `e1fd808`)

Done:
- Every `.c`/`.h` in `ni/` and `tools/` has been read in full. The 29 complex files (core, virtual objects, backend, buggy translations) got a comment written for that file at the top, and two single-site bugs got comments at the site. Simple files are untouched (D9). Comment-only change: verified by comparing each file with comments stripped against HEAD (all 31 identical).
- `doc/MAP.md`: coverage ledger, one hand-maintained row per file.
- `doc/KNOWN_BUGS.md`: every suspected bug in one place, including a direct review of the memory-access code (`ni/x64/v_mem_mov.h`, `vcompiler.c` field functions).
- `doc/GOAL.md`, `GLOSSARY.md`, `ARCHITECTURE.md` (lifecycle, mixed-file breakdowns, backend boundary), `PATTERNS.md` (P01–P26 + generator rules), `REFERENCES.md`, `BUILD.md`, `DECISIONS.md`, `QUESTIONS.md`.
- `doc/history/removed-files.md` and `doc/history/removed-mechanisms.md`: what was taken out of Psyco during the port, with commits and reasons.
- `CLAUDE.md` at the repo root as the entry point; a status note added to `README.md`.

How it was done: the core (`vcompiler`, `dispatcher`, `codegen`, `psyfunc`, `mergepoints`, `eval_hook`, `Python/pycompiler`, `frames`) was reviewed directly. `Objects/` (in two groups), `x64/` and the two history passes were done by parallel sub-reviews, then merged and spot-checked. At the time, bugs came from reading only; the baseline section above records what has since been confirmed by running.

Not done / next candidates:
- The sub-review reports (per-function tables, virtual-object layouts, bug lists with evidence) are kept as point-in-time records in `doc/reviews/2026-11-16/` (including the brief the reviewers worked from). They aren't maintained; re-check against the code before relying on them.
- Diff each translated file against the CPython 3.7 source it mirrors, to measure drift precisely.
- Owner answered Q1–6 (DECISIONS D10–D15).
- Start the meta-compiler design: the per-slot signature table (Q7) and the `Cf*` derivation (Q8).
