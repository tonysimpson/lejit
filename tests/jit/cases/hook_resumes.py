# A compiled generator resumed by C code (sum, deque), so through the eval
# hook (eval_hook.c:LeGen_Eval): each resume counts a level of recursion as
# ceval does, and leaves the run to ceval while an event is flagged
# (signals, KeyboardInterrupt) or a tool is monitoring.
import collections, signal, sys


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    # a release build can't say what compiled
    if lejit is None or not hasattr(lejit._core, '_code_info'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


# Each level is one generator frame, resumed by sum.
def nest(k):
    if k:
        yield sum(nest(k - 1))
    yield 1


def deepest(limit):
    old = sys.getrecursionlimit()
    sys.setrecursionlimit(limit)
    try:
        k = 1
        while True:
            try:
                sum(nest(k))
            except RecursionError as e:
                return k, str(e)
            k += 1
    finally:
        sys.setrecursionlimit(old)


# RecursionError at the same level as in CPython. Before sys.monitoring
# came, CPython counted C calls and Python frames in one count, which
# compiled code counts less of (the depth differs); since, Python frames
# have a count of their own.
def recursion_depth():
    if not hasattr(sys, 'monitoring'):
        return None
    out = []
    for limit in (60, 90, 60):
        out.append(deepest(limit))
        out.append(sum(nest(20)))
    return out, compiled(nest)


# With no Python limit in reach, C's own check stops the nesting before the
# stack runs out (since sys.monitoring came; before, plain CPython crashes
# here), at the same depth where C counts levels. Where it measures the
# stack instead, compiled levels take less of it and go deeper, so only the
# error is compared. The error branch is first compiled there.
def stack_limit():
    if not hasattr(sys, 'monitoring'):
        return None
    try:
        import _testinternalcapi
    except ImportError:
        _testinternalcapi = None
    stack_measured = hasattr(_testinternalcapi, 'get_stack_margin')
    old = sys.getrecursionlimit()
    try:
        sys.setrecursionlimit(10 ** 7)
        k = 1000
        while k < 10 ** 6:
            try:
                sum(nest(k))
            except RecursionError:
                depth = 'stopped' if stack_measured else k
                return depth, sum(nest(30)), compiled(nest)
            k *= 2
        return 'no error'
    finally:
        sys.setrecursionlimit(old)


# Signals arrive while C code resumes a compiled generator for ever: the
# handler runs, and KeyboardInterrupt ends the loop.
def counting(box):
    n = 0
    while not box.get('stop'):
        n += 1
        yield n


def handler_runs():
    box = {}

    def handle(signum, frame):
        box['stop'] = True
    old = signal.signal(signal.SIGALRM, handle)
    try:
        signal.setitimer(signal.ITIMER_REAL, 0.002)
        collections.deque(counting(box), maxlen=0)
        return 'stopped', compiled(counting)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


def keyboard_interrupt():
    old = signal.signal(signal.SIGALRM, signal.default_int_handler)
    try:
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.002)
            collections.deque(counting({}), maxlen=0)
        except KeyboardInterrupt:
            return 'interrupted', compiled(counting)
        return 'not interrupted'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


# sys.monitoring switched on between resumes: the resumes after it give
# their events.
def watched(n):
    for i in range(n):
        yield i


def monitored(n):
    mon = sys.monitoring
    tool = mon.PROFILER_ID
    me = watched.__code__
    events = []
    mon.use_tool_id(tool, 'test')
    mon.register_callback(
        tool, mon.events.PY_RESUME,
        lambda code, off: events.append('resume') if code is me else None)
    mon.register_callback(
        tool, mon.events.PY_YIELD,
        lambda code, off, value: events.append('yield')
        if code is me else None)
    try:
        g = watched(n)
        total = next(g) + next(g)
        mon.set_events(tool, mon.events.PY_RESUME | mon.events.PY_YIELD)
        total += sum(g)
    finally:
        mon.set_events(tool, 0)
        mon.free_tool_id(tool)
    return total, events


def monitoring_between_resumes(n):
    if not hasattr(sys, 'monitoring'):
        return None
    return [monitored(n) for k in range(3)], compiled(watched)


CASES = [
    ('recursion_depth', [()], {}),
    ('stack_limit', [()], {}),
    ('handler_runs', [()], {}),
    ('keyboard_interrupt', [()], {}),
    ('monitoring_between_resumes', [(10,)], {}),
]
