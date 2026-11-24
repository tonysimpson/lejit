# A small body inlined into its compiled caller, seen from Python that
# its call through C runs: the inlined frame's line, and its caller's
# locals, must be CPython's.
import sys


def where_called(x):
    # run by map, from C: the frames out of it
    f = sys._getframe(1)
    back = f.f_back
    return (x, f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno,
            back.f_code.co_name,
            sorted((k, v) for k, v in back.f_locals.items()
                   if not callable(v)))


def through_c(f, a, m):                # inlined: no loop, join or global
    b = a + 1
    return [*m(f, (a, b))]


def caller(n):
    out = {}
    for i in range(n):
        s = 's%d' % i
        t = i * 2.5
        out[i] = through_c(where_called, i, map)
    return out, s, t


class Seen:
    @property
    def place(self):
        f = sys._getframe(1)
        return (f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno,
                sorted(k for k in f.f_back.f_locals))


def read_place(o, k):                  # inlined; the property runs in C's call
    j = k * 3
    return j, o.place


def property_caller(n):
    o = Seen()
    out = {}
    for i in range(n):
        w = -i
        out[i] = read_place(o, i)
    return out, w


CASES = [
    ('caller', [(3,)], {}),
    ('property_caller', [(3,)], {}),
]
