# Status

## Handover (2026-11-18)

Where the phase stands (GENERATION.md):
- `baseline-3.7` holds everything; pushed to origin when the owner asks. `tools/check.sh` passes (939 tests, 285 CPython modules, the status baselines and donecheck enforcing).
- Generated on 3.7: 75 of 119 opcodes; donecheck 201 of 280 attempted functions generated, 60 hand-written with a final reason, 168 with a transitional one (D97), 0 with none. Steps 0–4, 6, 7, 9 done; 5 and 8 mostly done (what's left is reasoned in `tools/metac/hand_table.py` and the METACOMPILER failures log); 10 is `donecheck.py`.
- 3.8.20 generates to the end with `--past-policy` (PORT38.md has the table and causes); not built.

Waiting on the owner: nothing. Delegated questions are decided (D99–D101); the owner is told only of visible behaviour changes, slowdowns over 1% that survive the heap controls, or changes of direction.

Done: the docs were cut from 1.25 MB towards 150–200K as agreed (2026-11-18); docs hold current truth only, and history and measurements are in commit messages.

Open bugs from the last run: `test_queue` under `--all` fails on timing now and then, with and without Ni.
