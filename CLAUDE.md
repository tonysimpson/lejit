# Ni — orientation for AI assistants

Ni is a fork of Armin Rigo's **Psyco** JIT for CPython, ported to x86-64 and CPython 3.7. It runs pyperformance about 2.2× faster than plain 3.7 and passes CPython's own test suite (`tools/check.sh`).

**The project's goal is research.** We keep Psyco's design and generate its "compile-time CPython" layer at build time from CPython's own C source, with one Ni source for every CPython from 3.7 on. Read `doc/GOAL.md` before proposing anything: it lists approaches that are deliberately out of scope (PyPy-style, tracing JITs, copy-and-patch, modifying CPython).

## Where to look

| You want… | Read |
|---|---|
| Why this exists, what's in and out of scope, the done test | `doc/GOAL.md` |
| Where things stand, and what's waiting on the owner | `doc/STATUS.md` |
| The generator's new design and its migration (current work) | `doc/GENERATOR_DESIGN.md` |
| The current generator's rules, and the failures log | `doc/METACOMPILER.md` |
| The plan of work for the phase, step by step | `doc/GENERATION.md` |
| What every file is (layer, CPython counterpart, reviewed commit) | `doc/MAP.md` |
| How compilation works end to end; terms | `doc/ARCHITECTURE.md`, `doc/GLOSSARY.md` |
| How a CPython C function becomes its compile-time form | `doc/PATTERNS.md` |
| What mirrors CPython and why it's still hand-written | `tools/metac/donecheck.py` (`doc/INVENTORY.md`) |
| Decisions, open questions, open bugs, open ideas | `doc/DECISIONS.md`, `doc/QUESTIONS.md`, `doc/KNOWN_BUGS.md`, `doc/BACKLOG.md` |
| Findings of finished studies (stores, code size, same code, closures, generators, compile cost) and their tools | `doc/FINDINGS.md` |
| Choosing the real-world applications suite | `doc/REALWORLD.md` |
| Profiling, and telling heap noise from code changes | `doc/PROFILE.md` |
| Specialisation policy: promotion sites and merge points | `doc/SPECIALISATION.md` |
| Porting to 3.8 and 3.12 | `doc/PORT38.md`, `doc/PORT312.md` |
| Building and debugging | `doc/BUILD.md` |
| CPython's own JIT, for comparison | `doc/CPYTHON_JIT.md` |
| What was removed from Psyco during the port; upstream refs and papers | `doc/history/`, `doc/REFERENCES.md` |

The code is the source of truth. The complex files (core, virtual objects, backend) start with a comment explaining what is non-obvious and any known bugs; simple files have no header. Original Psyco is in this repo's git history: `git show 9bd5834:c/<path>`.

## Conventions

- British English.
- Psyco's authors went on to build PyPy; write about them and their work with respect. Credit ideas to them, describe Psyco's costs as engineering facts, and never characterise why anyone moved on or judge their code ("hack", "died", "failed").
- **Docs hold current truth only.** Rewrite or delete rather than append. Measurements, step reports and history go in commit messages, not docs; git keeps everything removed. No per-step entries in `STATUS.md`. A fixed bug leaves `KNOWN_BUGS.md` unless something still cites it.
- Docs are local Markdown in `doc/`. Mark claims **[read]** (checked in code), **[run]** (observed) or **[inferred]**. Anchor to `file:function` and commit hashes, not line numbers.
- Layers: `core` | `translated` | `backend` | `glue` | `tooling` | `obsolete` (definitions in `doc/GOAL.md`).
- Comment only where the code is complex, written for that file, never a template, and never restating the code (`doc/DECISIONS.md` D9). Source comments say what is true now and why, in place: no decision numbers, no history (what it used to do, which commit changed it); that belongs in commit messages. When you change a file, update its row in `doc/MAP.md`. Record new decisions in `doc/DECISIONS.md` and unknowns in `doc/QUESTIONS.md`, each in a sentence or two.
- Agents running commands: never `rm` an unguarded variable path (write literal paths or `"${S:?}"/x`), no `git stash`, run tools from your own worktree, cap Python at `prlimit --as=4000000000`.
- Build and test with `tools/check.sh` (`doc/BUILD.md`). It needs CPython 3.7.
