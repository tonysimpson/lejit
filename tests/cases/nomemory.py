# Compile-time MemoryError. A pure call folded during compilation
# (compute_long's PyLong_FromLong for a constant virtual int) that fails to
# allocate raises with PyErr_SetNone, so PyErr_Fetch gives val == NULL, which
# psyco_virtualize_exception must not make a NULL constant of. Each child
# fails exactly one allocation (n) during g's first call, via
# _testcapi.set_nomemory, with a fixed hash seed so the count is
# reproducible. Children import Ni only when the parent has it,
# so the plain run is the oracle. The windows are fragile: if this build's
# counts move, re-find them with the same child and a wider range.
#
# Early allocations. With Ni, 0-12 are the merge-point analysis
# (mergepoints.c: the code's extra record, the report, the liveness
# arrays, the fatlists, the capsule); a failure there leaves the code
# uncompiled for this call. 13-23 abort on purpose ("psyco: out of memory"
# or "cannot recover from the error above"; KNOWN_BUGS F27), so no window
# covers them. 24-26 register the frame at run time (frames.c), and must
# not return NULL without an exception.
import os
import subprocess
import sys

ROOT = sys.path[1]   # the repo root, inserted by tests/child.py
CHILD = '''
import sys, _testcapi
if sys.argv[2] == 'ni':
    sys.path.insert(0, %r); import ni
SRC = """
def g():
    a = 500
    b = a + 600
    return b
"""
ns = {}
exec(SRC, ns)
_testcapi.set_nomemory(int(sys.argv[1]), int(sys.argv[1]) + 1)
try:
    r = ns['g']()
except MemoryError:
    r = 'MemoryError'
finally:
    _testcapi.remove_mem_hooks()
print(r)
''' % ROOT


def _children(lo, hi):
    env = dict(os.environ)
    env['PYTHONHASHSEED'] = '0'
    results = []
    for n in range(lo, hi):
        p = subprocess.run([sys.executable, '-c', CHILD, str(n),
                            'ni' if 'ni' in sys.modules else 'plain'], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        results.append((p.returncode, p.stdout.strip()))
    return results


def compile_time_memory_error(lo, hi):
    # only deaths by a signal: an exit status of 1 (a MemoryError outside
    # the try) happens with and without Ni
    return sorted(set(c for c, out in _children(lo, hi) if c < 0))


def early_allocation(lo, hi):
    # plain and Ni runs fail different allocations here, so compare only
    # that every child exited normally with the result or a MemoryError
    return [(n, c, out) for n, (c, out) in enumerate(_children(lo, hi), lo)
            if c != 0 or out not in (b'1100', b'MemoryError')]


CASES = [
    ('compile_time_memory_error', [(24, 40)], {'leaks': 'runs child processes'}),
    ('early_allocation', [(0, 13), (24, 27)], {'leaks': 'F27: runs child processes'}),
]
