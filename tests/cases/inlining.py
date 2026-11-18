# Small functions called from compiled code are inlined (Psyco's rule;
# mergepoints.c: one merge point, no block, no LOAD_GLOBAL, weight <= 6).
# Everything observable must be as if they were called: results,
# exceptions and their tracebacks, frames seen by callees, defaults,
# identity, and recursion through an inlined function.
import sys


def add(a, b):
    return a + b


def first(t):
    return t[0]


def pick(flag, a, b):
    return a if flag else b


def with_default(a, b=10):
    return a * b


def apply(f, x):
    return f(x)


def same(x):
    return x


def drop_temp(o):
    m = o.append
    return 1


# leaving the inlined drop_temp frees the bound method in m: that decref
# calls method_dealloc, a C call aligned from the frame being restored
# (the restored frame must be there to align from)
def inlined_frees_local(n):
    out = []
    t = 0
    for i in range(n):
        t += drop_temp(out)
    return t


def hot_loop(n):
    t = 0
    for i in range(n):
        t = add(t, i)
    return t


def floats(n):
    t = 0.0
    for i in range(n):
        t = add(t, i * 0.5)
    return t


def mixed(n):
    out = []
    for i in range(n):
        out.append(pick(i % 2, add(i, 1), with_default(i)))
    return out


def defaults(a):
    return with_default(a), with_default(a, 2), with_default(b=3, a=a)


def raises_inside(a):
    try:
        return first(a)
    except IndexError as e:
        tb = sys.exc_info()[2]
        names = []
        while tb is not None:
            names.append(tb.tb_frame.f_code.co_name)
            tb = tb.tb_next
        return type(e).__name__, names


def frame_names():
    out = []
    f = sys._getframe(1)
    while f is not None and len(out) < 4:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def names_callback(x):
    return frame_names()


def frames_through_inlined():
    # apply is inlined; frame_names must still see it (a lambda here would
    # be a new function object each call, compiled for each until the call
    # site goes megamorphic: growth the leak check sees, C14)
    return apply(names_callback, 0)


def identity_kept(a):
    t = (a, a)
    return same(t) is t, add(t, t) == t + t


def recursion_through_inlined(n):
    def down(k):
        return 0 if k == 0 else 1 + apply(down, k - 1)
    return down(n)


def endless_through_inlined():
    def down(k):
        return apply(down, k + 1)
    try:
        down(0)
    except RecursionError:
        return 'RecursionError'


CASES = [
    ('inlined_frees_local', [(20,)], {}),
    ('hot_loop', [(100,), (0,)], {}),
    ('floats', [(50,)], {}),
    ('mixed', [(7,)], {}),
    ('defaults', [(3,), (2.5,)], {}),
    ('raises_inside', "[([],), ([7],)]", {}),
    ('frames_through_inlined', [()], {}),
    ('identity_kept', [(1,)], {}),
    ('recursion_through_inlined', [(50,)], {}),
    ('endless_through_inlined', [()], {}),
]
