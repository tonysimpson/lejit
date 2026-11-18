# History

The full history of Psyco (2001–2010, Armin Rigo et al.) and Ni (2016–2021, Tony Simpson) is in this repo's git.

| Ref | Meaning |
|---|---|
| `ee2bc8a` | Psyco initial check-in, 2001-12-04 |
| `ccce919` | Psyco 1.6 release, 2007-12-03 |
| `9bd5834` | Last upstream commit (py27 branch), 2010-02-22: **the upstream baseline** |
| `54a16e9` | First Ni commit, 2016-01-20 (64-bit via ivm) |
| `4b45ec8` | 2018-01-18: x64 backend forked from i386 |
| `58aff23` | 2019-02-14 "Cleanup for Py3": profiler, debugging and the psyco Python package removed |
| `a880444` | 2020-04-12 "Start of Python3": most optimisations stripped, merge points simplified |
| `e1fd808` | HEAD, 2021-05-07 |

- `removed-files.md`: upstream files and subsystems removed in the port (compact objects, virtual ints, range, profiler, ivm/i386, test suite…), and unmerged remote branches (`origin/gdb_support` has GDB JIT debug-info work).
- `compact-objects.md`: how compact objects worked and what a restore would need.
- `removed-mechanisms.md`: optimisations and mechanisms removed, disabled or weakened inside surviving files (LOAD_GLOBAL folding, merge-point placement, dead-local deletion, inlining, virtual int/bool/range/strings, opcode coverage, frame emulation, backend losses). Each file says what has come back since.

The two commits that matter most are `a880444` ("Many optimization have been removed, some would be good to put back with Python3 versions, but requires analysis") and `58aff23`.
