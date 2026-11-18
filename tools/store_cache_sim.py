"""Simulate option 4's store cache (doc/FINDINGS.md "Attribute stores")
over one iteration of a vendored pyperformance benchmark. Plain CPython 3.7
only:

    python3.7 tools/store_cache_sim.py richards [iterations]

Traces every STORE_ATTR as tools/storeclasses.py does and replays the
inline check against a cache keyed by (type, name), filled from the class's
keys when the cache is made (on the site's first store, as Ni compiles on
first call) and refreshed after every miss from the dict the store used.
Reports hits, misses by the check that failed, and GC-tracking cases.
Layouts are 3.7 x86-64 (Objects/dict-common.h, Include/objimpl.h)."""
import collections
import ctypes
import gc
import opcode
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = sys.argv[1]
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 1
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pyperf37  # noqa

rd = lambda addr: ctypes.c_void_p.from_address(addr).value or 0
rds = lambda addr: ctypes.c_ssize_t.from_address(addr).value
HT_CACHED_KEYS = type.__basicsize__ - 8
GetDictPtr = ctypes.pythonapi._PyObject_GetDictPtr
GetDictPtr.restype = ctypes.c_void_p
GetDictPtr.argtypes = [ctypes.py_object]
STORE_ATTR = opcode.opmap['STORE_ATTR']
EXT = opcode.opmap['EXTENDED_ARG']
OBJ_SETATTR = object.__setattr__
HAVE_GC = 1 << 14


def entries(k):
    size = rds(k + 8)
    ixsize = 1 if size <= 0xff else 2 if size <= 0xffff else 4
    return k + 40 + size * ixsize


def keys_index(k, name_id):
    e = entries(k)
    for i in range(rds(k + 32)):
        if rd(e + 24 * i + 8) == name_id:
            return i
    return -1


caches = {}          # (type, name) -> [dk_size, ix] or None
pending = []         # (cache key, dict address holder object, name)
counts = collections.Counter()


def fill(ck, k, name):
    ix = keys_index(k, id(name)) if k else -1
    caches[ck] = [rds(k + 8), ix] if ix >= 0 else [0, -1]


def check(o, name, value):
    tp = type(o)
    if tp.__setattr__ is not OBJ_SETATTR or not tp.__flags__ & (1 << 9):
        return 'not inline (type)'
    ck = (tp, name)
    if ck not in caches:
        fill(ck, rd(id(tp) + HT_CACHED_KEYS), name)
    dp = GetDictPtr(o)
    if not dp:
        return 'not inline (no __dict__)'
    d = rd(dp)
    pending.append((ck, o, name))
    if not d:
        return 'miss: no dict yet'
    keys, values = rd(d + 32), rd(d + 40)
    if not values:
        return 'miss: combined'
    size, ix = caches[ck]
    if rds(keys + 8) != size:
        return 'miss: dk_size'
    if rd(entries(keys) + 24 * ix + 8) != id(name):
        return 'miss: key at ix'
    pending.pop()        # a hit leaves the cache as it is
    dd = ctypes.cast(d, ctypes.py_object).value
    untracked = not gc.is_tracked(dd)
    vgc = type(value).__flags__ & HAVE_GC
    if untracked:
        counts['  (untracked dict, value type %s)' % ('GC: helper' if vgc else 'not GC: inline')] += 1
    if untracked and vgc:
        return 'miss: untracked dict, GC value'
    if rd(values + 8 * ix):
        return 'hit: overwrite (A)'
    if ix == rds(d + 16):
        return 'hit: fill in order (B)'
    return 'miss: empty slot out of order'


def refresh():
    for ck, o, name in pending:
        dp = GetDictPtr(o)
        d = rd(dp) if dp else 0
        if d and rd(d + 40):
            fill(ck, rd(d + 32), name)
    del pending[:]


def tracer(frame, event, arg):
    if event == 'call':
        frame.f_trace_opcodes = True
        frame.f_trace_lines = False
        return tracer
    if event != 'opcode':
        return tracer
    if pending:
        refresh()
    code = frame.f_code
    i = frame.f_lasti
    if code.co_code[i] != STORE_ATTR:
        return tracer
    oparg = code.co_code[i + 1]
    if i >= 2 and code.co_code[i - 2] == EXT:
        oparg |= code.co_code[i - 1] << 8
    name = code.co_names[oparg]
    top = rd(id(frame) + 72)            # f_stacktop
    o = ctypes.cast(rd(top - 8), ctypes.py_object).value
    v = ctypes.cast(rd(top - 16), ctypes.py_object).value
    counts[check(o, name, v)] += 1
    return tracer


benches = pyperf37.load(os.path.join(ROOT, 'tools', 'pyperformance', 'bm_%s.py' % BENCH))
_, timed = benches[0]
timed(1)                                   # warm (first-time setup stores)
sys.settrace(tracer)
timed(ITERS)
sys.settrace(None)

tot = sum(n for c, n in counts.items() if not c.startswith(' '))
print('== %s: %d stores, %d caches' % (BENCH, tot, len(caches)))
for c, n in sorted(counts.items()):
    print('  %6.2f%%  %-45s %d' % (100. * n / tot, c, n))
