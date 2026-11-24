# Keyword calls of C functions and methods (pmethodobject.c:call_keywords):
# a METH_FASTCALL | METH_KEYWORDS one gets its keywords as vectorcall
# passes them, a METH_VARARGS | METH_KEYWORDS one a tuple and a dict; the
# others, and calls with more values than the direct path takes, raise
# CPython's own TypeError through the residual call.
import io
import sys


def key(v):
    return -v


class Boom(Exception):
    pass


def boom(v):
    raise Boom(v)


def fastcall(n):
    out = []
    for i in range(n):
        x = [i, 3, 1, 2]
        x.sort(key=key)
        y = [i, 5, 4]
        list.sort(y, key=key, reverse=True)
        out.append((sorted([i, 2, 9], key=key), sorted([i, 1], reverse=True),
                    sorted([3, i, 1], key=key, reverse=False), x, y,
                    max([i, 4, 1], key=key), min([], default=i),
                    'a,b,c'.split(',', maxsplit=i % 3),
                    'a b'.split(sep=' '), int.from_bytes(b'\x01\x02',
                                                         byteorder='big')))
    return out


def many_values(n):
    # past the direct path's count, the values go in one tuple
    out = []
    for i in range(n):
        buf = io.StringIO()
        print(i, 'b', 3.5, sep='-', end='|', file=buf, flush=True)
        print(i, i + 1, sep=',', end='', file=buf)
        out.append(buf.getvalue())
        try:
            print(i, 2, 3, sep=1, end='', file=buf)
        except TypeError as e:
            out.append(str(e))
    return out


def varargs(n):
    out = []
    for i in range(n):
        out.append(('{a}-{b}'.format(a=i, b='x'), '{0}{k}'.format(i, k=1),
                    dict.fromkeys('ab', i), {}.fromkeys(['q'], i)))
    return out


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: sorted([i], kee=key),
                  lambda: sorted([i], key=key, key2=1),
                  lambda: len(x=i),
                  lambda: [].append(x=i),
                  lambda: 'a'.upper(k=i),
                  lambda: 'ab'.split(foo=i),
                  lambda: max(1, 2, key=key, default=i),
                  lambda: [3, i].sort(key=boom),
                  lambda: sorted([1, i], key=boom),
                  lambda: [1, 2].sort(key=key, reverse=True, x=i)):
            try:
                out.append(f())
            except (TypeError, ValueError, Boom) as e:
                out.append((type(e).__name__, str(e)))
    return out


def _proxies():
    lejit = sys.modules.get('lejit')
    return lejit._core._stats()['cfunc_proxy'] if lejit is not None else 0


SOURCE = """
def g(x, buf):
    return (sorted(x, key=abs), x.sort(key=abs, reverse=True),
            '{a}'.format(a=1), max(x, key=abs, default=0),
            print(1, 2, 3, sep='', end='', file=buf, flush=False), x)
"""


def _fresh_g():
    ns = {}
    exec(compile(SOURCE, 'kw', 'exec'), ns)
    return ns['g']


def no_residual_calls(n):
    # the calls a fresh code object compiles are all direct: the residual
    # call would make the bound method, an argument tuple and a dict
    out = []
    for i in range(n):
        g = _fresh_g()
        before = _proxies()
        buf = io.StringIO()
        out.append((g([i, -3, 2], buf), _proxies() - before, buf.getvalue()))
    return out


CASES = [
    ('fastcall', "[(3,), (4,)]", {}),
    ('many_values', "[(3,), (4,)]", {}),
    ('varargs', "[(3,), (4,)]", {}),
    ('errors', "[(2,), (3,)]", {}),
    ('no_residual_calls', "[(3,)]", {'checked': 'reads lejit._core._stats'}),
]
