---
upstream_ref: 9bd5834
head_ref: e1fd808
written: 2026-11-16
---

# Files and subsystems removed since upstream Psyco

Scope: whole files/subsystems present in upstream Psyco (`9bd5834`, Armin Rigo,
2010-02-22, py27 head) that are absent or gutted at Ni HEAD (`e1fd808`). Mechanisms
removed *inside* surviving files are documented separately; they are only
mentioned here where a feature spanned a removed file and surviving hooks.

Path mapping: upstream `c/` became `ni/` in `58aff23`; `c/i386` was copied
verbatim to `c/x64` in `4b45ec8` (diff of all four i386/x64 files at that commit:
zero lines differ) and then edited; `py-support/` → `psyco/` (`52eff9f`) →
`ni/psyco/` (`56d4fa0`) → deleted (`58aff23`).

Evidence tags: **[read]** = seen in code/diff/commit message; **[inferred]** = my
deduction. Categories: (a) core specialiser design worth keeping, (b) CPython
translation a meta-compiler would regenerate, (c) backend, (d) tooling/packaging,
(e) obsolete (Py2-only, platform-specific, etc.).

Key removal commits [read]:

| commit | date | message |
|---|---|---|
| `52eff9f` | 2016-01-20 | Move to Ni |
| `012b48f` | 2018-07-22 | WIP on pointbreak based analyser |
| `892d5a6` | 2019-02-09 | Fixes and cleanup |
| `05f85f4` | 2019-02-09 | Removed old Makefile |
| `58aff23` | 2019-02-14 | Cleanup for Py3 |
| `a880444` | 2020-04-12 | Start of Python3 |
| `ee1161b` | 2020-04-24 | prangeobject needs fixing |

`58aff23` body [read]: "Removes a lot of the unneeded/unused functionality from
Psyco... Most of the code removed was related to debugging which can now be done
with nianalyser or dynamic profiling for code compilation which is not an
immediate goal for Ni. Compact object is broken! But it needed rethinking anyway."
`a880444` body [read]: mergepoints stripped to a minimal version; "Many
optimization have been removed"; "Compact object has been removed mainly because I
don't understand it so wasn't sure how to port it. It is very fast when used so
analysis should be done around if or how to reintroduce it."

## Summary table

| item | upstream path | removed in | category | restore difficulty |
|---|---|---|---|---|
| Compact objects (`psyco.compact`, `compacttype`) | `c/Objects/compactobject.[ch]`, `c/Objects/pcompactobject.[ch]`, `py-support/kdictproxy.py`, `py-support/classes.py` | `a880444` (C), `58aff23` (Py) | a | hard |
| Virtual Py2 int | `c/Objects/pintobject.[ch]` | `a880444` | a (pattern) + b (int slots) | medium |
| Virtual range/xrange | `c/Objects/prangeobject.[ch]` | ported in `a880444`, deleted `ee1161b` | b (+a pattern) | easy-medium |
| Type call / object instantiation | `c/Objects/ptypeobject.[ch]` | `a880444` | b | medium |
| `array` module meta-impl | `c/Modules/parray.c` | `a880444` | b | medium |
| Frame-introspecting builtins (`_psyco.globals/eval/locals/...`) | `c/Modules/ppsyco.c` | `58aff23` | b/e | low value |
| Bool / str meta-impls (gutted, files remain) | `c/Objects/pboolobject.c`, `c/Objects/pstringobject.c` | `a880444`, `7441415`/`58aff23` | b | medium |
| C-struct-as-PyObject helper | `c/cstruct.[ch]` | `a880444` | d (infrastructure) | trivial (superseded by PyCapsule) |
| Profiler / turbo (OSR entry) | `c/profile.[ch]`, `c/stats.[ch]`, `c/timing.h`, `c/alarm.[ch]`, `py-support/profiler.py`, `py-support/logger.py` | `58aff23` | a (selection policy, OSR) | hard |
| `_psyco` extension module entry | `c/psyco.c` | `58aff23` (replaced by `ni/ni.c`) | d | n/a (replaced) |
| Python package `psyco` (`full/bind/proxy/...`) | `py-support/*.py` | `58aff23` | d | medium |
| ivm backend + Prolog VM generator | `c/ivm/**` | `892d5a6` | c | hard, low value |
| i386 backend | `c/i386/*` | `892d5a6` (after copy to x64 in `4b45ec8`) | c | n/a (lives on as x64) |
| Build/source-list tooling | `c/files.py`, `c/depend.py`, `c/mkincl.py`, `c/Makefile`, `c/versionchecker.py`, `c/win32/psyco.vcproj` | `58aff23`, `05f85f4`, `892d5a6` | d | easy |
| Opcode coverage checker | `c/mychecker.py` | `58aff23` | d (relevant to meta-compiler) | easy |
| Memory debugging | `c/linuxmemchk.[ch]` | `58aff23` | d | easy |
| Code-buffer inspection tools | `py-utils/*` (`xam`, `httpxam`, `vinfo_dump`, `symbols`, `ivm*`, ...) | `012b48f` | d | medium (superseded by nianalyser) |
| Upstream test suite | `test/*.py` etc. | `52eff9f` | d | easy (needs Py3 port) |
| Docs | `doc/*`, `README.txt` | moved to `doc/psyco_legacy/` in `6a376a6`; `README.txt` deleted `52eff9f` | d | n/a |

## Since the port

The table is the state at the port (`e1fd808`). Since then a virtual range is back (`ni/Objects/prangeobject.c`), ints are virtual again (`plongobject.c`), and class instantiation has a meta (`pbltinmodule.c:ptype_call`). The per-item detail (what each did, the stated and inferred reasons, leftovers) is `git show e404100:doc/history/removed-files.md`; compact objects are in `compact-objects.md`.

## Remote branches

`origin/gdb_support` (`92a5985`, 2017-01-10) is the only one with unmerged content: an incomplete GDB JIT debug-info recorder (`c/debug_info/`, a GDB JIT reader plugin needing a patched GDB), for i386 and Py2; its design is relevant to symbolising JIT code for perf or GDB. `origin/POC_knee_deep_in_Py3` duplicates master commits; the others are fully merged. [read]
