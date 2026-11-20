# The frame that catches an exception shows its locals as they are when
# Python reads them, not as they were when the exception passed:
# each way of reading a frame's locals, with them changed after the raise,
# in the case's own frame (the one NiCode_Run runs) and in a compiled
# callee (whose traceback frame stands in for it).
import gc
import inspect
import io
import pdb
import sys
import traceback


class Big:
    pass


def raiser(n):                        # a loop: called, not inlined
    t = 0
    for i in range(n):
        t += i
    raise ValueError(t)


def shown(d, names=('x', 'y', 'n')):
    return [(k, d[k]) for k in names if k in d]


def read_in_callee(tb):               # a loop: a compiled call
    out = []
    for k in ('x', 'y'):
        out.append(tb.tb_frame.f_locals.get(k))
    return out


def read_inlined(tb):                 # small: inlined into the catcher
    return tb.tb_frame.f_locals.get('x')


def plain(n):
    x = 1
    try:
        raiser(n)
    except Exception as e:
        tb = e.__traceback__
        x = 2
        first = tb.tb_frame.f_locals['x']
        x = 3
        y = 'later'
        return first, shown(tb.tb_frame.f_locals)


def callee_plain(n):
    return plain(n), plain(n + 1)


def own_raise(n):                     # raised in the catcher itself
    x = [n]
    try:
        x[5]
    except IndexError:
        tb = sys.exc_info()[2]
    x = n * 10
    y = n + 1
    return shown(tb.tb_frame.f_locals)


def callee_own_raise(n):
    return own_raise(n), own_raise(n)


def from_callee(n):                   # read by a compiled callee
    x = 'before'
    try:
        raiser(n)
    except ValueError as e:
        tb = e.__traceback__
    x = 'after'
    y = n
    return read_in_callee(tb), read_inlined(tb)


def callee_from_callee(n):
    return from_callee(n), from_callee(n + 1)


def walk(n):                          # traceback.walk_tb, capture_locals
    x = 'before'
    try:
        raiser(n)
    except ValueError as e:
        tb = e.__traceback__
    x = 'after'
    frames = [(f.f_code.co_name, sorted(k for k in f.f_locals
                                        if k in ('x', 'n', 't', 'i')))
              for f, _ in traceback.walk_tb(tb)]
    summary = traceback.StackSummary.extract(traceback.walk_tb(tb),
                                             capture_locals=True)
    return frames, [(s.name, s.locals.get('x')) for s in summary]


def callee_walk(n):
    return walk(n), walk(n)


def with_inspect(n):                  # inspect.getinnerframes, getargvalues
    x = 'before'
    try:
        raiser(n)
    except ValueError:
        tb = sys.exc_info()[2]
    n = n + 100
    x = 'after'
    infos = inspect.getinnerframes(tb, context=0)
    args = inspect.getargvalues(infos[0].frame)
    return ([i.function for i in infos], args.args,
            args.locals['n'], args.locals['x'])


def callee_inspect(n):
    return with_inspect(n), with_inspect(n)


def getframe(n):                      # sys._getframe(0) in the handler
    x = 'before'
    try:
        raiser(n)
    except ValueError:
        f = sys._getframe()
        x = 'during'
        during = f.f_locals['x']
        tb = sys.exc_info()[2]
    x = 'after'
    return during, f.f_locals['x'], tb.tb_frame.f_locals['x']


def callee_getframe(n):
    return getframe(n), getframe(n)


def caller_of(depth):                 # a loop: called, not inlined
    for _ in range(1):
        pass
    return sys._getframe(depth).f_locals.get('x')


def getframe_caller(n):               # sys._getframe(1) from a callee
    x = 'before'
    try:
        raiser(n)
    except ValueError:
        x = 'during'
        seen = caller_of(1)
    x = 'after'
    return seen, caller_of(1)


def callee_getframe_caller(n):
    return getframe_caller(n), getframe_caller(n)


LOCALS = getattr(__builtins__, 'locals', None) or __builtins__['locals']
VARS = getattr(__builtins__, 'vars', None) or __builtins__['vars']


def by_other_name(n):                 # locals and vars reached by other names
    x = 'before'
    try:
        raiser(n)
    except ValueError:
        x = 'during'
        a = LOCALS()['x']
        tb = sys.exc_info()[2]
    x = 'after'
    return a, VARS()['x'], tb.tb_frame.f_locals['x']


def callee_by_other_name(n):
    return by_other_name(n), by_other_name(n)


TRACED = []


def tracer(frame, event, arg):
    if frame.f_code.co_name in ('traced', 'profiled'):
        TRACED.append((event, frame.f_locals.get('x')))
    return tracer


def traced(n):                        # tracing starts in the handler
    del TRACED[:]
    x = 'before'
    try:
        raiser(n)
    except ValueError as e:
        tb = e.__traceback__
        sys._getframe().f_trace = tracer
        sys.settrace(tracer)
        x = 'during'
        mid = tb.tb_frame.f_locals['x']
    x = 'after'
    sys.settrace(None)
    return mid, tb.tb_frame.f_locals['x'], TRACED[:3]


def callee_traced(n):
    return traced(n), traced(n)


def profiled(n):                      # profiling starts in the handler
    del TRACED[:]
    x = 'before'
    try:
        raiser(n)
    except ValueError as e:
        tb = e.__traceback__
        sys.setprofile(tracer)
        x = 'during'
        mid = tb.tb_frame.f_locals['x']
    x = 'after'
    sys.setprofile(None)
    return mid, tb.tb_frame.f_locals['x'], sorted(set(TRACED))


def callee_profiled(n):
    return profiled(n), profiled(n)


def post_mortem(n):                   # pdb.post_mortem prints a local
    x = 'before'
    try:
        raiser(n)
    except ValueError:
        tb = sys.exc_info()[2]
    x = 'after'
    out = io.StringIO()
    p = pdb.Pdb(stdin=io.StringIO('up\np x\np n\nquit\n'), stdout=out,
                nosigint=True)
    p.use_rawinput = False
    p.reset()
    p.interaction(None, tb)
    return [l for l in out.getvalue().splitlines() if '.py(' not in l]


def callee_post_mortem(n):
    return post_mortem(n), post_mortem(n)


def referents(n):                     # gc.get_referents of the frame
    x = Big()
    try:
        raiser(n)
    except ValueError:
        tb = sys.exc_info()[2]
    old = x
    x = Big()
    found = gc.get_referents(tb.tb_frame)
    new_seen = old_seen = False
    for o in found:
        new_seen = new_seen or o is x
        old_seen = old_seen or o is old
    return new_seen, old_seen


def callee_referents(n):
    return referents(n), referents(n)


def returns_tb(n):                    # read after the catcher returned
    x = 'before'
    try:
        raiser(n)
    except ValueError as e:
        tb = e.__traceback__
    x = 'after'
    y = n
    return tb


def after_return(n):
    tb = returns_tb(n)
    return shown(tb.tb_frame.f_locals), tb.tb_frame.f_back.f_code.co_name


def one_frame(n):                     # one frame for the call, as CPython's
    tbs = []
    for i in range(n):
        x = i
        try:
            raise KeyError(i)
        except KeyError:
            tbs.append(sys.exc_info()[2])
    x = 'end'
    return ([tb.tb_frame is tbs[0].tb_frame for tb in tbs],
            tbs[0].tb_frame.f_locals['x'], tbs[0].tb_frame.f_locals['i'])


def callee_one_frame(n):
    return one_frame(n), one_frame(n)


class Exit:
    def __enter__(self):
        return self

    def __exit__(self, t, v, tb):
        self.seen = tb.tb_frame.f_locals.get('x')
        return True


def with_exit(n):                     # a with block's __exit__ reads it
    x = 'before'
    cm = Exit()
    with cm:
        x = 'inside'
        raiser(n)
    return cm.seen


def callee_with_exit(n):
    return with_exit(n), with_exit(n)


def dropped(n):                       # 'except E: pass', read later
    x = 'before'
    for i in range(n):
        try:
            raise KeyError(i)
        except KeyError:
            pass
    x = 'after'
    try:
        raise KeyError(n)
    except KeyError:
        tb = sys.exc_info()[2]
    x = 'last'
    return tb.tb_frame.f_locals['x'], tb.tb_frame.f_locals['i']


def callee_dropped(n):
    return dropped(n), dropped(n)


def deleted(n):                       # a del after the raise frees the value
    import weakref
    enabled = gc.isenabled()
    gc.disable()
    b = Big()
    r = weakref.ref(b)
    try:
        raiser(n)
    except ValueError:
        tb = sys.exc_info()[2]
    del b
    dead = r() is None
    if enabled:
        gc.enable()
    return dead, 'b' in tb.tb_frame.f_locals


def callee_deleted(n):
    return deleted(n), deleted(n)


CASES = [
    ('plain', [(2,)], {}),
    ('callee_plain', [(2,)], {}),
    ('own_raise', [(2,)], {}),
    ('callee_own_raise', [(2,)], {}),
    ('from_callee', [(2,)], {}),
    ('callee_from_callee', [(2,)], {}),
    ('walk', [(2,)], {}),
    ('callee_walk', [(2,)], {}),
    ('with_inspect', [(2,)], {}),
    ('callee_inspect', [(2,)], {}),
    ('getframe', [(2,)], {}),
    ('callee_getframe', [(2,)], {}),
    ('getframe_caller', [(2,)], {}),
    ('callee_getframe_caller', [(2,)], {}),
    ('by_other_name', [(2,)], {}),
    ('callee_by_other_name', [(2,)], {}),
    ('traced', [(2,)], {}),
    ('callee_traced', [(2,)],
     {'xfail': "the traceback's frame keeps the raise's locals"}),
    ('profiled', [(2,)], {}),
    ('callee_profiled', [(2,)], {}),
    ('post_mortem', [(2,)], {'leaks': 'on 3.10'}),
    ('callee_post_mortem', [(2,)], {'leaks': 'on 3.10'}),
    ('referents', [(2,)], {}),
    ('callee_referents', [(2,)], {}),
    ('after_return', [(2,)], {}),
    ('one_frame', [(3,)], {}),
    ('callee_one_frame', [(3,)], {}),
    ('with_exit', [(2,)], {}),
    ('callee_with_exit', [(2,)], {}),
    ('dropped', [(3,)], {}),
    ('callee_dropped', [(3,)], {}),
    ('deleted', [(2,)], {}),
    ('callee_deleted', [(2,)], {}),
]
