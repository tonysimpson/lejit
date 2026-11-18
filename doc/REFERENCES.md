---
written: 2026-11-16
---
# References

## Pinned versions

| Thing | Ref | Notes |
|---|---|---|
| Original Psyco, final state | git `9bd5834` (Armin Rigo, 2010-02-22) | Head of the upstream py27 branch. Sources under `c/`. Read with `git show 9bd5834:c/<path>` or `git worktree add ../psyco-upstream 9bd5834`. |
| Psyco 1.6 release | git `ccce919` (2007-12-03) | Last packaged release. |
| First Ni commit | git `54a16e9` (2016-01-20) | "Now supports 64bit machines using the ivm". Everything from here on is Ni. |
| Psyco history | git `ee2bc8a` (2001-12-04) onwards | The **whole** Psyco history is in this repo, so there's no need to fetch upstream. |
| Ni HEAD at time of indexing | git `e1fd808` (2021-05-07, "WIP") | |
| Target CPython | 3.7 | Uses 3.7-specific internals: `interp->eval_frame` (PEP 523 field), `_Py_CODEUNIT` wordcode, `SETUP_LOOP`/`SETUP_EXCEPT`/`SETUP_FINALLY` block opcodes (removed in 3.8/3.9/3.11). See `ni/eval_hook.c`, `ni/mergepoints.c`. |
| CPython source to read alongside | v3.7.x tag of github.com/python/cpython | Not vendored. The translated files mirror `Objects/*.c` and `Python/ceval.c` from roughly this version, with Py2-era leftovers. |

## Papers and talks (by Armin Rigo)

- *Representation-Based Just-In-Time Specialization and the Psyco Prototype for Python*, PEPM 2004. The theory paper: values, representations, promotion. The original URL (`codespeak.net/svn/user/arigo/papers/psyco/...`) is dead. Try the Wayback Machine, or the PyPy `extradoc` repository, which inherited codespeak's svn. [unverified location]
- Talks: `accu2004-psyco` and `pepm04-psyco` (same dead codespeak svn paths, listed in `doc/psyco_legacy/README.txt`).
- *The Ultimate Psyco Guide*: `doc/psyco_legacy/psycoguide.tex` (in repo). It's user-facing (profilers, `psyco.compact`, `psyco.classes`, known bugs) and useful for understanding features that were removed.
- Project page: psyco.sourceforge.net (historic).

## Psyco history outside this repo

`~/work/psyco-history` (read-only; start with `HANDOVER-ni.md`): the psyco-devel mailing list (`archive/psyco-devel/`, 379 messages), an hg mirror with upstream branches Ni never got (`git/psyco-hg-mirror.git`: `nfs` 2006 builtin metas, `devel` Tismer 2007–08, 2011–16 master), CVS 2001–05, and what survives of Psyco V2 (`reports/tismer-work.md`). Used for KNOWN_BUGS F26 and the builtin metas.

## Related local repositories

- `~/work/pointbreak`: Tony's Python debugger library, used by `tools/nianalyser` and `tools/test_harness`.

## Other in-repo history docs

- `doc/history/removed-files.md`: upstream files and subsystems that no longer exist, and unmerged remote branches.
- `doc/history/removed-mechanisms.md`: optimisations and mechanisms removed or disabled inside surviving files.
