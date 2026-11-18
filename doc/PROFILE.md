# Profiling compiled code

How to see where compiled code spends its time, and how to tell a real cost from heap state. Results go in commit messages.

## Where the time goes

Release build. `tools/profile_bench.py BENCH SECONDS` runs one vendored pyperformance benchmark warm under Ni (three compiling iterations first) for a profiler:

```
perf record -m 16 -F 4999 -g -o X.data python3.7 tools/profile_bench.py richards 4
perf report -i X.data --no-children --sort sym -g none
```

- Needs `kernel.perf_event_paranoid` ≤ 2 (Debian's default is 3: `sudo sysctl kernel.perf_event_paranoid=1`, until reboot). `-m 16` because the locked-memory limit is shared with other perf users.
- Emitted code shows as `[JIT]`; Ni's helpers and metas as `ni.so`; the rest is libpython and libc. Self time (`--no-children`) is what to read.
- To see which C functions compiled code calls, log the targets in `iencoding.c:run_time_call` in a scratch build: that counts call sites emitted, not calls made.

## Instructions per warm iteration

The figure every change is judged by (D29): `perf stat -e instructions:u` on `tools/profile_bench.py BENCH` at two lengths (say 1 s and 4 s), or on a driver that runs `timed(3)` then `timed(N)` against `timed(3)` alone, the difference divided by the difference in iterations. `PYTHONHASHSEED=0`. Prefer fixed iteration counts when comparing under the heap controls below. go varies by ±3–4% between runs of one build; most others by under 1%.

## Heap state (D92)

A change that alters only start-up code can still move a benchmark's instructions per warm iteration by 1–2%, through the allocators. pyflate allocates 2 KB lists in its hot loop (`move_to_front`), above pymalloc's 512-byte limit, so glibc's `_int_malloc` serves them, and how far it searches its bins depends on what start-up left there; hexiom's run depends on the pymalloc heap. Compiling a few unrelated functions before pyflate moves it by about ±1% too, through the code-buffer state (coding-pause placement, [inferred]).

Two checks tell a heap effect from a code effect:
- **Split the count by object.** `valgrind --tool=callgrind --smc-check=all-non-file python3.7 BENCH-DRIVER`, then `callgrind_annotate --auto=no`: emitted code shows as `???:0x…`, and the self counts sum per object (emitted code, `ni.so`, libpython, libc). If the emitted code and `ni.so` are the same and libc's `_int_malloc`/`unlink_chunk` carry the difference, it is the heap. callgrind uses glibc's own malloc, but its address layout is not the native one, so a heap effect's size there differs.
- **Perturb the heap.** Run both builds with one `malloc` block (ctypes) or one Python object kept alive after warm-up, or under `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. A difference that changes sign is the heap's.

With `perf record -e instructions:u` on this AMD machine, samples skid and the period must stay above about 1 M instructions or the kernel throttles them, so per-symbol counts don't settle it; callgrind does.

## What profiles have shown

- **Instance attributes and dicts** were about 60% of richards, 50% of deltablue and 36% of raytrace: lookups through `PyDict_GetItem` and `lookdict_split`, stores through `insertdict`. Hence attribute lookup specialised on the type behind an inline guard (D30), inline loads on shared keys (D31), known-hash loads for unshared dicts (`0965d06`), and the store work (D35, D38, D39, D47; FINDINGS.md). The recursion count on compiled calls is inline too (C12).
- **Strings:** in the vendored set only pyflate spends real time on them (bytes indexing, slices, `b"".join`); deltablue's `%` is about 0.8%, in set-up and `__repr__`. Done: `len` and indexing (D37), virtual slices of str, bytes and list (D56, pyflate −12%). Measured and not worth doing on this set: `str.join`, `bytes.join` and `%` (the join is the work itself; splitting a constant format would save about 0.3% of deltablue), and concatenation buffers. Run-time `in` on str is generated (D92). Left as C calls: iteration over str and bytes, tuple slices, steps, `__index__` bounds, and `s[i] == 'c'` (QUESTIONS 40).
- **`isinstance` on classes** takes the generic chain by design: its site promotes static types only (D66).
