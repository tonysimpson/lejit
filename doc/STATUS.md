# Status

## 2026-11-16: first indexing pass (at `e1fd808`)

Done:
- Every `.c`/`.h` in `ni/` and `tools/` has been read in full. The 29 complex files (core, virtual objects, backend, buggy translations) got a comment written for that file at the top, and two single-site bugs got comments at the site. Simple files are untouched (D9). Comment-only change: verified by comparing each file with comments stripped against HEAD (all 31 identical).
- `doc/MAP.md`: coverage ledger, one hand-maintained row per file.
- `doc/KNOWN_BUGS.md`: every suspected bug in one place, including a direct review of the memory-access code (`ni/x64/v_mem_mov.h`, `vcompiler.c` field functions).
- `doc/GOAL.md`, `GLOSSARY.md`, `ARCHITECTURE.md` (lifecycle, mixed-file breakdowns, backend boundary), `PATTERNS.md` (P01–P26 + generator rules), `REFERENCES.md`, `BUILD.md`, `DECISIONS.md`, `QUESTIONS.md`.
- `doc/history/removed-files.md` and `doc/history/removed-mechanisms.md`: what was taken out of Psyco during the port, with commits and reasons.
- `CLAUDE.md` at the repo root as the entry point; a status note added to `README.md`.

How it was done: the core (`vcompiler`, `dispatcher`, `codegen`, `psyfunc`, `mergepoints`, `eval_hook`, `Python/pycompiler`, `frames`) was reviewed directly. `Objects/` (in two groups), `x64/` and the two history passes were done by parallel sub-reviews, then merged and spot-checked. Bugs listed in the docs come from reading only: **nothing was built or run** (no Python 3.7 available).

Not done / next candidates:
- Build under CPython 3.7 (pyenv) to confirm the suspected bugs and the "works for simple functions" baseline.
- The sub-review reports (per-function tables, virtual-object layouts, bug lists with evidence) are kept as point-in-time records in `doc/reviews/2026-11-16/` (including the brief the reviewers worked from). They aren't maintained; re-check against the code before relying on them.
- Diff each translated file against the CPython 3.7 source it mirrors, to measure drift precisely.
- Owner answered Q1–6 (DECISIONS D10–D15).
- Start the meta-compiler design: the per-slot signature table (Q7) and the `Cf*` derivation (Q8).
