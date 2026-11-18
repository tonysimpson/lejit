# Status

## Handover (2026-11-18)

Where the phase stands (GENERATION.md):
- `baseline-3.7` holds everything, not pushed. `tools/check.sh` passes (939 tests, 285 CPython modules, donecheck enforcing).
- Generated on 3.7: 75 of 119 opcodes; donecheck 212 generated, 228 hand-written with a reason, 0 with none. Steps 0–4, 6, 7, 9 done; 5 and 8 mostly done (what's left is reasoned in `tools/metac/hand_table.py` and the METACOMPILER failures log); 10 is `donecheck.py`.
- 3.8.20 generates to the end with `--past-policy` (PORT38.md has the table and causes); not built.

Waiting on the owner:
1. QUESTIONS 50: does a policy-only reason (108 definitions) satisfy the done test, or is that still this phase's work? Count definitions or lines?
2. D92: ~1% pyflate/hexiom moves must pass PROFILE.md's heap-state controls before they count as regressions. Accept, or keep D29's plain reading?
3. QUESTIONS 49 (`_PyArg_CheckPositional` policy entry: the largest 3.8 unlock) and QUESTIONS 48 (block stack without `#if`, needed to build 3.8).
4. D81: one call per part for `[*a, *b]`, `{**a, **b}`, and now `f(*a, *b)` with unknown parts (D93).

Done: the docs were cut from 1.25 MB towards 150–200K as agreed (2026-11-18); docs hold current truth only, and history and measurements are in commit messages.

Open bugs from the last run: `test_queue` under `--all` fails on timing now and then, with and without Ni.
