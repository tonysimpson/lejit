# Running out of memory while compiling. Each child fails allocations and
# must exit normally, with the result or a MemoryError; a compile that runs
# out is abandoned, and the code runs interpreted or is compiled again
# (codemanager.h:psyco_guard_abandon).
#
# memory_runs_out: each child fails exactly one allocation (n) during g's
# first call, via _testcapi.set_nomemory, with a fixed hash seed. With
# LeJIT, the first dozen are the merge-point analysis; the compile follows,
# then frame registration at run time. The plain run fails other
# allocations, and an exit status of 1 (a MemoryError outside the try)
# happens with and without LeJIT, so only deaths by a signal and wrong
# results count.
#
# injected: a checked build fails the n-th allocation at each site
# (lejit._core._fail_after, psyco.h:LE_FAIL), in the first compile, in
# coding pauses, respawns and promotions; the child's results must be the
# plain run's. Children run LeJIT only when the parent does, so the plain
# run is the oracle. Results are cached: the leak check calls each case
# 109 more times.
import os
import subprocess
import sys

ROOT = sys.path[1]   # the repo root, inserted by tests/jit/child.py
CHILD = '''
import sys, _testcapi
if sys.argv[2] == 'lejit':
    sys.path.insert(0, %r)
    import lejit
    lejit.start()
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

INJECT = '''
import sys
site, n = sys.argv[1], int(sys.argv[2])
if site != 'plain':
    sys.path.insert(0, %r)
    import lejit
    lejit.start()

def h(x):
    return x * 2

class A:
    def m(self):
        return 1

class B(A):
    def m(self):
        return 2

def g(n):
    t = 0
    for i in range(n):
        if i %% 3:
            t += h(i)
        else:
            t -= len(str(i))
    for v in [1, 'a', 2.0, (1,), A(), B(), None, [3]]:
        t += 1 if isinstance(v, A) and v.m() == 2 else 0
        t += len(type(v).__name__)
    try:
        t += [][n]
    except IndexError as e:
        t += len(e.args)
    return t

if site != 'plain':
    lejit._core._fail_after(site, n)
r = g(50), g(70)
fired = site == 'plain' or lejit._core._fail_after(site, 0) == 0
print(r, fired)
''' % ROOT

SITES = ['psycoobject', 'array', 'codebuf', 'lookup', 'owner']
_cache = {}


def _run(src, *args):
    env = dict(os.environ)
    env['PYTHONHASHSEED'] = '0'
    p = subprocess.run([sys.executable, '-c', src] + [str(a) for a in args],
                       env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.strip()


def memory_runs_out(lo, hi):
    if (lo, hi) not in _cache:
        mode = 'lejit' if 'lejit' in sys.modules else 'plain'
        bad = []
        for n in range(lo, hi):
            c, out = _run(CHILD, n, mode)
            if c < 0 or (c == 0 and out not in (b'1100', b'MemoryError')):
                bad.append((n, c, out))
        _cache[lo, hi] = bad
    return _cache[lo, hi]


def injected(counts):
    # the plain run has nothing to inject: the oracle is []
    if 'lejit' not in sys.modules:
        return []
    if counts not in _cache:
        expected = _run(INJECT, 'plain', 0)
        bad = []
        for site in SITES:
            fired = False
            for n in counts:
                c, out = _run(INJECT, site, n)
                if out.endswith(b' True'):
                    fired = True
                    out = out[:-5] + b' False'
                if c != 0 or out != expected[1][:-5] + b' False':
                    bad.append((site, n, c, out))
            if not fired:
                bad.append((site, 'never fired'))
        _cache[counts] = bad
    return _cache[counts]


# every count early on, then spread out: what a failure leaves behind can
# show only in a later compile (a respawn of code compiled after a failed
# coding pause fails for most counts between 59 and 289)
INJECT_COUNTS = tuple(range(1, 10)) + tuple(range(15, 300, 30))

CASES = [
    ('memory_runs_out', [(0, 50)], {'leaks': 'runs child processes'}),
    ('injected', [(INJECT_COUNTS,)], {'leaks': 'runs child processes'}),
]
