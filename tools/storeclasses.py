"""Classify every STORE_ATTR of a vendored pyperformance benchmark by what
the store does to the instance dict (doc/FINDINGS.md). Plain CPython 3.7 only:

    python3.7 tools/storeclasses.py richards [iterations]

Opcode tracing (f_trace_opcodes): during the trace call 3.7 sets f_stacktop,
so the owner (TOS) is read with ctypes before the store runs. Struct layouts
are 3.7 x86-64 (Include/frameobject.h, Objects/dict-common.h). One untraced
iteration runs first, so one-time setup stores are left out."""
import collections
import ctypes
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


def keys_index(k, name_id):
    size = rds(k + 8)
    nent = rds(k + 32)
    ixsize = 1 if size <= 0xff else 2 if size <= 0xffff else 4 if size <= 0xffffffff else 8
    entries = k + 40 + size * ixsize
    for i in range(nent):
        if rd(entries + 24 * i + 8) == name_id:
            return i
    return -1


def classify(o, name):
    tp = type(o)
    if tp.__setattr__ is not OBJ_SETATTR:
        if isinstance(o, type):
            return 'other: class attribute'
        if not isinstance(tp.__setattr__, type(OBJ_SETATTR)):
            return 'other: Python __setattr__'
        return 'other: C setattro (%s)' % tp.__name__
    for k in tp.__mro__:
        if name in k.__dict__:
            d = k.__dict__[name]
            dt = type(d)
            if hasattr(dt, '__set__') or hasattr(dt, '__delete__'):
                return 'descriptor: ' + ('slot' if dt.__name__ == 'member_descriptor' else dt.__name__)
            break
    dp = GetDictPtr(o)
    if not dp:
        return 'other: no __dict__'
    d = rd(dp)
    shared = rd(id(tp) + HT_CACHED_KEYS) if tp.__flags__ & (1 << 9) else 0
    if not d:
        return 'creates dict (%s)' % ('shared keys' if shared else 'combined')
    keys, values = rd(d + 32), rd(d + 40)
    used = rds(d + 16)
    if values:   # split
        ix = keys_index(keys, id(name))
        same = 'class keys' if keys == shared else 'old keys'
        if ix >= 0:
            if rd(values + 8 * ix):
                return 'split: existing key, value set (%s)' % same
            if ix == used:
                return 'split: key in keys, next in order (%s)' % same
            return 'split: key in keys, out of order -> combined'
        if used == rds(keys + 32) and rds(keys + 24) > 0:
            return 'split: new key appended to shared keys (%s)' % same
        return 'split: new key past keys / no room -> combined'
    dd = o.__dict__
    return 'combined: existing key' if name in dd else 'combined: new key'


counts = collections.Counter()
sites = collections.defaultdict(collections.Counter)
site_types = collections.defaultdict(set)


def tracer(frame, event, arg):
    if event == 'call':
        frame.f_trace_opcodes = True
        frame.f_trace_lines = False
        return tracer
    if event != 'opcode':
        return tracer
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
    c = classify(o, name)
    counts[c] += 1
    key = (code.co_filename.rsplit('/', 1)[-1], code.co_name, code.co_firstlineno, i, name)
    sites[key][c] += 1
    site_types[key].add(type(o))
    return tracer


benches = pyperf37.load(os.path.join(ROOT, 'tools', 'pyperformance', 'bm_%s.py' % BENCH))
_, timed = benches[0]
timed(1)                                   # warm (first-time setup stores)
sys.settrace(tracer)
timed(ITERS)
sys.settrace(None)

tot = sum(counts.values())
print('== %s: %d stores over %d iteration(s), %d sites' % (BENCH, tot, ITERS, len(sites)))
for c, n in counts.most_common():
    print('  %6.2f%%  %-55s %d' % (100. * n / tot, c, n))
mono = sum(sum(v.values()) for v in sites.values() if max(v.values()) >= 0.99 * sum(v.values()))
monot = sum(sum(sites[k].values()) for k in sites if len(site_types[k]) == 1)
print('  stores at sites with one class >=99%%: %.1f%%; at sites seeing one type: %.1f%%'
      % (100. * mono / tot, 100. * monot / tot))
print('  top sites:')
for k, v in sorted(sites.items(), key=lambda kv: -sum(kv[1].values()))[:12]:
    n = sum(v.values())
    print('   %5.1f%% %s:%s:%d @%d .%s  %s  types=%s' % (100. * n / tot, *k,
          ', '.join('%s %d' % kv for kv in v.most_common(3)), ','.join(t.__name__ for t in site_types[k])))
