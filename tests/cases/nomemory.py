# Compile-time MemoryError (KNOWN_BUGS F26). A pure call folded during
# compilation (compute_long's PyLong_FromLong for a constant virtual int)
# that fails to allocate raises with PyErr_SetNone, so PyErr_Fetch gives
# val == NULL; psyco_virtualize_exception made a NULL constant of it and the
# re-raise crashed. Each child fails exactly one allocation (n) during g's
# first call, via _testcapi.set_nomemory, with a fixed hash seed so the
# count is reproducible. Children import Ni only when the parent has it,
# so the plain run is the oracle. The window is fragile: allocations below it hit
# Ni's deliberate fatal "out of memory" or F27; if this build's counts
# move, re-find it with the same child and a wider range.
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


def compile_time_memory_error(lo, hi):
    env = dict(os.environ)
    env['PYTHONHASHSEED'] = '0'
    codes = []
    for n in range(lo, hi):
        p = subprocess.run([sys.executable, '-c', CHILD, str(n),
                            'ni' if 'ni' in sys.modules else 'plain'], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        codes.append(p.returncode)
    # only deaths by a signal: an exit status of 1 (a MemoryError outside
    # the try) happens with and without Ni
    return sorted(c for c in set(codes) if c < 0)


CASES = [
    ('compile_time_memory_error', [(24, 40)], {'leaks': 'F26: runs child processes'}),
]
