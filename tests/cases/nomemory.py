# Running out of memory while compiling (KNOWN_BUGS F26, F27). Each child
# fails allocations and must exit normally, with the result or a
# MemoryError; a compile that runs out is abandoned, and the code runs
# interpreted or is compiled again (codemanager.h, compile guards).
#
# memory_runs_out: each child fails exactly one allocation (n) during g's
# first call, via _testcapi.set_nomemory, with a fixed hash seed. With Ni,
# the first dozen are the merge-point analysis, which fails cleanly; the
# compile follows, then frame registration at run time (frames.c). The
# plain run fails other allocations, and an exit status of 1 (a
# MemoryError outside the try) happens with and without Ni, so only deaths
# by a signal and wrong results count. F26 was a NULL constant made of a
# folded call's MemoryError (PyErr_SetNone gives val == NULL).
#
# injected: a debug build fails the n-th allocation at each site that ran
# out mid-compile (ni._fail_after, psyco.h:NI_FAIL), in the first compile,
# in coding pauses, respawns and promotions; the child's results must be
# the plain run's. Children import Ni only when the parent has it, so the
# plain run is the oracle. Results are cached: the leak check calls each
# case 109 more times.
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

INJECT = '''
import sys
site, n = sys.argv[1], int(sys.argv[2])
if site != 'plain':
    sys.path.insert(0, %r); import ni

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
    ni._fail_after(site, n)
r = g(50), g(70)
fired = site == 'plain' or ni._fail_after(site, 0) == 0
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
        mode = 'ni' if 'ni' in sys.modules else 'plain'
        bad = []
        for n in range(lo, hi):
            c, out = _run(CHILD, n, mode)
            if c < 0 or (c == 0 and out not in (b'1100', b'MemoryError')):
                bad.append((n, c, out))
        _cache[lo, hi] = bad
    return _cache[lo, hi]


def injected(counts):
    # plain and release builds have nothing to inject: the oracle is []
    ni = sys.modules.get('ni')
    if ni is None or not hasattr(ni, '_fail_after'):
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


CASES = [
    ('memory_runs_out', [(0, 60)], {'leaks': 'runs child processes'}),
    ('injected', [((1, 2, 3, 5, 8, 13, 30, 100, 300),)],
     {'leaks': 'runs child processes',
      'xfail': 'F116: a coding pause that runs out of memory and recompiles can diverge on respawn replay'}),
]
