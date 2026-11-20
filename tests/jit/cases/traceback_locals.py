# A traceback keeps each frame's locals as they were when the exception
# passed: they show in tb_frame.f_locals and stay alive while the traceback
# does, for compiled frames as for interpreted ones, with locals not read
# again (dead_local).
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


# changes in a finally after the raise reach the traceback's frame when the
# exception goes on: a del, a rebinding, a deleted
# argument. The catching frame's own del, after its handler, is not seen
# while it runs (dies_by_count)
class Big:
    pass


def changed_in_finally(n, arg):
    x = [n]
    y = 'y%d' % n
    try:
        raise ValueError(n)
    finally:
        del x
        y = None
        del arg


def finally_changes(n):
    try:
        changed_in_finally(n, Big())
    except ValueError as e:
        loc = e.__traceback__.tb_next.tb_frame.f_locals
        return sorted(loc.items(), key=repr)


def drop_in_finally(b):
    try:
        raise ValueError(b)
    finally:
        del b


def dies_by_count(n):
    import gc
    import weakref
    enabled = gc.isenabled()
    gc.disable()
    b = Big()
    r = weakref.ref(b)
    try:
        drop_in_finally(b)
    except ValueError as e:
        e.args = ()
    del b
    dead = r() is None
    if enabled:
        gc.enable()
    return dead


CASES = [
    ('kept', [(3,)], {}),
    ('inlined', [(4,)], {}),
    ('own_frame', [(2,)], {}),
    ('rebound', [(3,)], {}),
    ('through', [(2,)], {}),
    ('dead_local', [(2,)], {}),
    ('finally_changes', [(3,)], {}),
    ('dies_by_count', [(1,)],
     {'xfail': "a running catcher's frame keeps the locals written as the exception passed"}),
]
