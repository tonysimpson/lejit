# Ni — orientation for AI assistants

Ni is a fork of Armin Rigo's **Psyco** JIT for CPython, ported partway to x86-64 and CPython 3.7. It is dormant: the code compiles only very simple functions.

**The project's current goal is research, not features.** We keep Psyco's design and plan to replace its hand-written "compile-time CPython" layer with output from a **meta-compiler that reads CPython's C source**. Read `doc/GOAL.md` before proposing anything. It lists approaches that are deliberately out of scope (PyPy-style, tracing JITs, copy-and-patch, modifying CPython).

## Where to look

| You want… | Read |
|---|---|
| Why this exists, what's in and out of scope | `doc/GOAL.md` |
| What every file is (layer, CPython counterpart, review status) | `doc/MAP.md` |
| How compilation works end to end | `doc/ARCHITECTURE.md` |
| Terms: vinfo_t, Source, promotion, merge point, respawn… | `doc/GLOSSARY.md` |
| How a CPython C function becomes its compile-time form (the meta-compiler spec) | `doc/PATTERNS.md` |
| What was removed from Psyco during the port, and why | `doc/history/` |
| Upstream refs, CPython version, papers | `doc/REFERENCES.md` |
| Settled decisions / open questions / progress | `doc/DECISIONS.md`, `doc/QUESTIONS.md`, `doc/STATUS.md` |
| Meta-compiler plan, tooling options, failures log | `doc/METACOMPILER.md` |
| CPython's own JIT: speed-ups, status, pyperformance on 3.7 | `doc/CPYTHON_JIT.md` |
| Compile cost: compile time and code size per benchmark (`tools/compilebench.py`) | `doc/COMPILE_COST.md` |
| Code size: where emitted code goes, identical copies (`tools/codesize.py`) | `doc/CODE_SIZE.md` |
| Profiles: where compiled code spends its time (`tools/profile_bench.py`) | `doc/PROFILE.md` |
| Attribute stores: what they cost, what they do to the dict, options (`tools/storeclasses.py`) | `doc/STORES.md` |
| Generators: where time goes, options (resumable bodies, genexpr inlining) | `doc/GENERATORS.md` |
| Porting the core to 3.12: what changes, the entry design, risks | `doc/PORT312.md` |
| Known and suspected bugs (one list) | `doc/KNOWN_BUGS.md` |
| Everything still open, bugs and ideas, with pointers | `doc/BACKLOG.md` |
| One policy for specialisation: promotion sites and merge points | `doc/SPECIALISATION.md` |
| Building and debugging (3.7 only, nianalyser) | `doc/BUILD.md` |
| Per-function review evidence (point-in-time, not maintained) | `doc/reviews/` |

The code is the source of truth. The complex files (core, virtual objects, backend) start with a comment explaining what is non-obvious and any known bugs; simple files have no header. Original Psyco is in this repo's git history: `git show 9bd5834:c/<path>`.

## Conventions

- British English.
- Docs are local Markdown in `doc/`. Mark claims **[read]** (checked in code) or **[inferred]**. Anchor to `file:function` and commit hashes, not line numbers.
- Layers: `core` | `translated` | `backend` | `glue` | `tooling` | `obsolete` (definitions in `doc/GOAL.md`).
- Comment only where the code is complex, written for that file, never a template (`doc/DECISIONS.md` D9). When you change or re-review a file, update its row in `doc/MAP.md` (including the `reviewed` commit). Record new decisions in `doc/DECISIONS.md` and unknowns in `doc/QUESTIONS.md`.
- Agents running commands: never `rm` an unguarded variable path (write literal paths or `"${S:?}"/x`), no `git stash`, run tools from your own worktree, cap Python at `prlimit --as=4000000000`.
- Don't assume the code builds or runs. It needs CPython 3.7 and hasn't been built recently (see `doc/BUILD.md`).
