# Calls of METH_FASTCALL C functions and methods, which compiled code makes
# with the arguments in one array and, for a METH_FASTCALL | METH_KEYWORDS
# one, the call's names tuple (pmethodobject.c:call_fast, call_keywords):
# no arguments, a few, and more than fit in registers; constants mixed with
# values known only at run time and values made in the caller; keywords;
# and CPython's own errors from the function and from the residual call.
import math, struct, sys

try:
    math.hypot(1, 2, 3)
    WIDE_HYPOT = True  # METH_FASTCALL, any number of arguments
except TypeError:
    WIDE_HYPOT = False


def positional(n):
    out = []
    for i in range(n):
        f = i + 0.5
        x = [i, 2, 1]
        out.append((divmod(i + 7, 3), x.pop(), x.pop(0), x,
                    'abcabc'.count('b', i % 3, 5),
                    'a-b-c'.replace('-', str(i), 1),
                    struct.pack('<2b', i, 2),
                    struct.pack('<7b', i, 1, 2, 3, 4, 5, i + 1),
                    struct.pack('<5bdq', i, -1, 2, 3, 4, f * 2, i << 40),
                    struct.pack('<16B', *[i] * 4, 1, 2, 3, 4, 5, 6, 7, 8,
                                i, i, i, i),
                    WIDE_HYPOT and math.hypot(i, 1.5, 2, 3, 4, 5, f),
                    isinstance(i, (str, int)), getattr(x, 'nope', i)))
    return out


def keywords(n):
    out = []
    for i in range(n):
        f = i * 1.5
        out.append(('a b c'.split(), 'a,b,c'.split(',', maxsplit=i % 2),
                    'a b'.rsplit(maxsplit=1, sep=' '),
                    int.from_bytes(bytes([i, 1]), 'little', signed=True),
                    int.from_bytes(b'\xff', byteorder='big', signed=True),
                    sorted([i, 3, f, -2, 1, 0.5], key=abs, reverse=True),
                    (i + 2).to_bytes(length=2, byteorder='big')))
    return out


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: [].pop(),
                  lambda: [i].pop(5),
                  lambda: divmod(i),
                  lambda: divmod(i, 0),
                  lambda: divmod(i, b=2),
                  lambda: struct.pack('<2b', i, 2, 3, 4, 5, 6, 7),
                  lambda: struct.pack('<7b', i, 1, 2, 3, 4, 5, 999),
                  lambda: WIDE_HYPOT and math.hypot(i, 1, 2, 3, 4, 5, 'x'),
                  lambda: 'ab'.split(',', 1, 2),
                  lambda: 'ab'.split(sepp=','),
                  lambda: 'ab'.split(',', sep=','),
                  lambda: int.from_bytes(b'\x01', 'middle'),
                  lambda: int.from_bytes(b'\x01', 'big', i, True),
                  lambda: sorted([i, 'a'], key=None, reverse=False)):
            try:
                out.append(f())
            except (TypeError, ValueError, IndexError, ZeroDivisionError,
                    struct.error) as e:
                out.append((type(e).__name__, str(e)))
    return out


def _stats():
    lejit = sys.modules.get('lejit')
    if lejit is None:
        return 0, 0
    s = lejit._core._stats()
    return s['cfunc_fast'], s['cfunc_proxy']


SOURCE = """
def g(i, f):
    return (struct.pack('<7b', i, 1, 2, 3, 4, 5, i),
            WIDE_HYPOT and math.hypot(i, 1, 2, 3, 4, 5, f),
            'a b'.split(maxsplit=i))
"""


COMPILED = []


def _make_g():
    ns = {'struct': struct, 'math': math, 'WIDE_HYPOT': WIDE_HYPOT}
    exec(compile(SOURCE, 'fastcall', 'exec'), ns)
    return ns['g']


def direct(n):
    # the wide calls compile to direct calls: counted as such, none residual.
    # g is made once, so its first call compiles it and the later ones run
    # what was compiled
    if not COMPILED:
        g = _make_g()
        fast, proxies = _stats()
        g(0, 0.25)
        fast_now, proxies_now = _stats()
        COMPILED.append((g, fast_now > fast or 'lejit' not in sys.modules,
                         proxies_now - proxies))
    g, counted, proxies = COMPILED[0]
    return [(g(i, i + 0.25), counted, proxies) for i in range(n)]


CASES = [
    ('positional', "[(3,), (4,)]", {}),
    ('keywords', "[(3,), (4,)]", {}),
    ('errors', "[(2,), (3,)]", {}),
    ('direct', "[(3,)]", {'checked': 'reads lejit._core._stats'}),
]
