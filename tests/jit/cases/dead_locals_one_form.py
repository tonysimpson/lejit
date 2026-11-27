# At a merge point, a dead local takes one form whatever the path: a
# run-time value that may be NULL where it is unbound on one path and
# bound on another, a constant made run-time even when another local
# holds it too. At a loop head's second block counters' values become
# run-time. The values stay what CPython shows: in f_locals of a frame
# asked for later, in a traceback's frame, to a tracer, each object
# released once when rebound, and an unbound local still raises.
import sys
import weakref


class Obj:
    pass


def peek():
    return sorted(sys._getframe(1).f_locals.items(), key=repr)


def loop_variable(items):
    out = []
    for item in items:
        out.append(peek())
        out.append(item)
    return out


def bound_on_one_path(n):
    out = []
    i = 0
    while i < n:
        if i % 3 == 0:
            tmp = i * 2
            out.append(tmp)
        out.append(peek())
        i += 1
    return out


def shared_constant(n):
    out = []
    marker = None
    for i in range(n):
        if i & 1:
            a = b = marker
            out.append(a is b)
        else:
            a = 'x'
            b = i
        out.append(peek())
    return out


def deleted_and_rebound(n):
    out = []
    for k in range(n):
        if k % 2:
            y = k
            out.append(peek())
            del y
        out.append(peek())
        try:
            del y
        except NameError as e:
            out.append(str(e))
        try:
            out.append(y)
        except NameError as e:
            out.append(str(e))
    return out


def released(n):
    alive = []
    out = []
    for i in range(n):
        if i & 1:
            o = Obj()
            alive.append(weakref.ref(o))
        out.append(sum(r() is not None for r in alive))
    return out


def in_traceback(items):
    out = []
    for item in items:
        try:
            if item % 3 == 0:
                raise ValueError(item)
        except ValueError as e:
            out.append(sorted(e.__traceback__.tb_frame.f_locals, key=repr))
        if item & 1:
            last = item
    return out


def traced(n):
    seen = []

    def tracer(frame, event, arg):
        if frame.f_code is inner.__code__ and event == 'line':
            seen.append(sorted(frame.f_locals.items(), key=repr))
        return tracer

    def inner(n):
        for q in [1, None, 'a', n]:
            z = q
        return z

    sys.settrace(tracer)
    try:
        inner(n)
    finally:
        sys.settrace(None)
    return seen


def counters(items):
    a = b = c = 0
    neg = -3
    flag = False
    for v in items:
        if v % 2:
            a += 1
        elif v % 3:
            b -= 1
        else:
            c += 2
        if v % 5 == 0:
            neg -= v
            flag = not flag
    return a, b, c, neg, flag, a is b


CASES = [
    ('loop_variable', [([1, 'two', None, 3.5],)], {}),
    ('bound_on_one_path', [(8,)], {}),
    ('shared_constant', [(8,)], {}),
    ('deleted_and_rebound', [(6,)], {}),
    ('released', [(16,)], {}),
    ('in_traceback', [(list(range(10)),)], {}),
    ('traced', [(3,)], {}),
    ('counters', [(list(range(40)),), ([],), ([2, 4],)], {}),
]
