# Inventory: what in Ni mirrors CPython

GOAL's done test, point 1, is computed from the code by `tools/metac/donecheck.py` (D90). The hand-written inventory that used to be here is in git history (`git show e404100:doc/INVENTORY.md`).

## What it reads

`report.txt`, which `generate.py` writes beside the headers: one line per function a generator attempts, sorted, with tab-separated columns key, status (`generated`, `resumed` in part, `restarted` at once, `refused`), reason (for a resume, each place) and CPython location. The key names no file, so that versions compare: an exported function's name (`PyNumber_Add`), a static slot's type and slot (`PyFloat_Type.nb_add`), an opcode's name, a builtin's `module.name`, `virtual.NAME` for a virtual type's constructor, and other statics by name (by location where the name is ambiguous). The first line, `# VERSION SOURCE-SHA256 GENERATOR-COMMIT`, names the source's sha256 (`local` for a tree `cpython_src.py` didn't unpack) and the generator's commit (HEAD, with a hash of uncommitted changes under `tools/metac` and `ni`); donecheck refuses a report whose sha or commit isn't the current one.

The key, status and reason of each line must match the checked-in baseline, `tools/metac/expected/<version>.txt` (the header line and the location are ignored); `generate.py` fails on any difference and prints a diff. To accept it, copy the report over the baseline, `cp DIR/report.txt tools/metac/expected/<version>.txt`, and commit it. 3.8.20's baseline comes from the `--past-policy` dry run. `python3 tools/metac/status_diff.py 3.7.17 3.8.20 [-r]` compares two versions key by key (3.7.17 to 3.8.20: 14 status changes, 11 keys only in 3.7.17, 8 only in 3.8.20).

## What it reports

Every definition in Ni that mirrors a CPython one, in one of these classes:
- **(a) generated**: the report's functions that aren't refused, by status, and the definitions and lines of the headers.
- **(b) hand-written, final reason**: a `hand_table.py` reason of kind `core`, `mechanism` or `judgement`.
- **(t) hand-written, transitional reason**: open work for the phase: a `limit` or `helper` reason, the policy items of the policy-table entry that names it, or the generator's refusal of its CPython counterpart.
- **(c) hand-written, no reason**.

A definition mirrors CPython when the policy table names it, when its name is a CPython function's once Ni's prefixes are taken off, when it is a run-time copy (`cimpl_*`), when `Psyco_DefineMeta` registers it, or when it is an opcode `case` of `pycompiler.c`. The docstring of `donecheck.py` has the exact rules. Name matches that mirror nothing are `not_mirror` in `hand_table.py`.

## Running it

    prlimit --as=4000000000 python3 tools/metac/donecheck.py --gen DIR [--src DIR] [-v] [--list a|b|t|c|x]

DIR is `generate.py`'s output directory (`tools/check.sh` uses `build/gen-check/1`). It takes about a second. Exit 0 when the done test passes (no (t), no (c)); 3 when only transitional reasons remain, which `check.sh` accepts; 1 when (c) is not empty; 2 for a stale report or when `hand_table.py` names something not in Ni or a reference not in the docs.

## Where reasons live

- `tools/metac/hand_table.py`: `REASONS`, one `(kind, refs, why)` per definition, and `KINDS`. Refs are `Dn` (DECISIONS), `Fn`/`Cn` (KNOWN_BUGS), `Pn` (PATTERNS) or `failures:NAME` (a row of METACOMPILER.md's failures log); donecheck checks each exists.
- `tools/metac/policy_table.py`: `ENTRIES`; a definition an entry names (`ni`) has that entry's policy items as a transitional reason.
- `report.txt`: a CPython counterpart the report refuses gives its hand-written copy the refusal as a transitional reason.

## Current counts

CPython 3.7.17 at `b8093ba`: (a) 201 of 280 attempted functions (generated 142, resumed 53, restarted 6; refused 79), the headers 311 definitions, 6,601 lines; (b) 60 definitions, 1,362 lines (core 49, judgement 1, mechanism 10); (t) 168 definitions, 3,547 lines (helper 21, limit 13, policy 108, refused 26); (c) 0. 3.8.20 (dry run): (a) 189 of 277, (b) 60, (t) 164 (refused 22), (c) 0. Rerun for the current figures.
