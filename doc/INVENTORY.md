# Inventory: what in Ni mirrors CPython

GOAL's done test, point 1, is computed from the code by `tools/metac/donecheck.py` (D90). The hand-written inventory that used to be here is in git history (`git show e404100:doc/INVENTORY.md`).

## What it reports

Every definition in Ni that mirrors a CPython one, in one of three classes:
- **(a) generated**: written at build time by a generator; counted in the generated headers' lines.
- **(b) hand-written, reasoned**: with a named reason, by kind (`core`, `mechanism`, `limit`, `judgement`, `helper`, `policy`, `refused`).
- **(c) hand-written, no reason**: the done test fails on these.

A definition mirrors CPython when the policy table names it, when its name is a CPython function's once Ni's prefixes are taken off, when it is a run-time copy (`cimpl_*`), when `Psyco_DefineMeta` registers it, or when it is an opcode `case` of `pycompiler.c`. The docstring of `donecheck.py` has the exact rules.

## Running it

    prlimit --as=4000000000 python3 tools/metac/donecheck.py --python ~/.pyenv/versions/3.7.17/bin/python3.7 [-v] [--list a|b|c|x]

It runs the generators (no build needed) and takes about 90 seconds. `--past-policy` runs past the policy table's problems, for a dry run on a version Ni isn't built for (PORT38.md). Exit 0 when (c) is empty, 1 when it is not, 2 when `hand_table.py` names something not in Ni or a reference not in the docs. `tools/check.sh` runs it.

## Where reasons live

- `tools/metac/hand_table.py`: `REASONS`, one `(kind, refs, why)` per definition, and `KINDS`. Refs are `Dn` (DECISIONS), `Fn`/`Cn` (KNOWN_BUGS), `Pn` (PATTERNS) or `failures:NAME` (a row of METACOMPILER.md's failures log); donecheck checks each exists.
- `tools/metac/policy_table.py`: `ENTRIES`; a definition an entry names (`ni`) is reasoned by that entry's policy items.
- The generators' refusals: a CPython counterpart a generator refuses gives its hand-written copy the refusal as its reason.

## Current counts

CPython 3.7.17 at `e404100`: (a) 212 definitions, 6,228 lines; (b) 228 definitions, 4,909 lines (core 49, helper 21, judgement 1, limit 13, mechanism 10, policy 108, refused 26); (c) 0. Rerun for the current figures.
