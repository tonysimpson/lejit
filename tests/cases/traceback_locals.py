# A traceback keeps each frame's locals as they were when the exception
# passed: they show in tb_frame.f_locals and stay alive while the traceback
# does, for compiled frames as for interpreted ones. Compiled code drops a
# dead local of a type with no finaliser (dead_local).
import sys
import weakref


class Obj:
    pass


def raiser(n):                        # a loop: called, not inlined
    o = Obj()
    w = weakref.ref(o)
    t = 0
    for i in range(n):
        t += i
    raise ValueError(w, t + n + i)


def kept(n):
    try:
        raiser(n)
    except ValueError as e:
        w = e.args[0]
        tb = e.__traceback__
    alive = w() is not None
    f = tb.tb_next.tb_frame
    names = sorted(k for k in f.f_locals if k != 'o')
    return alive, names, f.f_locals['t'], f.f_locals['n']


def small(a):                         # inlined into its caller
    b = a * 2
    return [b][a]


def inlined(n):
    try:
        small(n)
    except IndexError:
        tb = sys.exc_info()[2]
    f = tb.tb_next.tb_frame
    return f.f_code.co_name, sorted(f.f_locals.items())


def own_frame(n):
    x = n + 1
    y = [x]
    try:
        y[5]
    except IndexError:
        tb = sys.exc_info()[2]
    loc = tb.tb_frame.f_locals
    return loc['x'], loc['y'], loc['n'], x + n


def rebound(n):
    for i in range(n):
        try:
            raise KeyError(i)
        except KeyError:
            pass
    try:
        n = 'changed'
        raise KeyError(n)
    except KeyError:
        tb = sys.exc_info()[2]
    return sorted((k, v) for k, v in tb.tb_frame.f_locals.items()
                  if k in ('n', 'i')), n, i


def passes(n):                        # 'except KeyError: pass' re-raises
    s = [n] * 3
    for i in range(n):
        try:
            raise ValueError(i)
        except KeyError:
            pass
    return s, i, n


def through(n):
    try:
        passes(n)
    except ValueError as e:
        tb = e.__traceback__
    loc = tb.tb_next.tb_frame.f_locals
    return loc['s'], loc['i'], loc['n']


def dead_local(n):
    k = n * 3
    try:
        raiser(n)
    except ValueError as e:
        tb = e.__traceback__
    return 'k' in tb.tb_frame.f_locals


CASES = [
    ('kept', [(3,)], {}),
    ('inlined', [(4,)], {}),
    ('own_frame', [(2,)], {}),
    ('rebound', [(3,)], {}),
    ('through', [(2,)], {}),
    ('dead_local', [(2,)], {'xfail': 'C18: a dead int local is dropped'}),
]
