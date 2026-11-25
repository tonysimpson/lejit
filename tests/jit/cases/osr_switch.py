# A function's frame that ceval runs, taken over at a loop head
# (eval_hook.c:le_osr_switch) after K backward jumps, armed by
# lejit._core._osr_arm: the compiled rest gives what ceval would, and
# leaves the frame as ceval would by each way out (a return, a raise, going
# on interpreted). Each loop is in a function called through map, so that
# ceval runs its frame. Plain CPython, and a release build, which has no
# _osr_arm, run the same loops interpreted throughout.
import sys, threading

K = 10


def arm(fn, k=K):
    core = getattr(sys.modules.get('lejit'), '_core', None)
    a = getattr(core, '_osr_arm', None)
    if a is not None:
        a(fn.__code__, k)


def switches():
    core = getattr(sys.modules.get('lejit'), '_core', None)
    s = getattr(core, '_osr_switches', None)
    return s() if s is not None else 0


def run(fn, *args):
    arm(fn)
    try:
        return next(map(fn, *[[a] for a in args]))
    finally:
        arm(fn, 0)


def lines(tb):
    out = []
    while tb is not None:
        code = tb.tb_frame.f_code
        out.append((code.co_name, tb.tb_lineno - code.co_firstlineno))
        tb = tb.tb_next
    return out


def sum_to(n):
    total = 0
    for i in range(n):
        total += i * i
    return total


def for_return(n):
    return run(sum_to, n)


def while_raises(n):
    i = 0
    while True:
        i += 1
        if i == n:
            raise ValueError(i)


def raise_no_handler(n):
    try:
        run(while_raises, n)
    except ValueError as e:
        return e.args, lines(e.__traceback__)


def kept_traceback(n, saved):
    for i in range(n):
        try:
            if i == n - 1:
                1 / 0
        except ZeroDivisionError as e:
            saved.append(e.__traceback__)
            raise


# the traceback entry the compiled code made is the one the error keeps
def traceback_identity(n):
    saved = []
    try:
        run(kept_traceback, n, saved)
    except ZeroDivisionError as e:
        return lines(e.__traceback__), e.__traceback__.tb_next is saved[0]


def loop_in_try(n):
    try:
        for i in range(n):
            if i == n - 1:
                raise ValueError(i)
    except KeyError:
        return 'caught'


# a handler covers the loop head, and doesn't match
def try_not_matching(n):
    try:
        run(loop_in_try, n)
    except ValueError as e:
        return e.args, lines(e.__traceback__)


def loop_then_reraise(n):
    x = 0
    try:
        for i in range(n):
            x += i
            if i == n - 1:
                raise KeyError(x)
    except KeyError:
        x = -x
        raise


def handled_then_reraised(n):
    try:
        run(loop_then_reraise, n)
    except KeyError as e:
        return e.args, lines(e.__traceback__)


def loop_caught(n):
    total = 0
    try:
        for i in range(n):
            total += i
            if i == n - 1:
                raise KeyError(total)
    except KeyError as e:
        return 'caught', e.args
    return total


def try_caught(n):
    return run(loop_caught, n)


class Noisy:
    def __init__(self, name, log):
        self.name = name
        self.log = log

    def __del__(self):
        self.log.append(self.name)


def frees(n, log):
    a = Noisy('a', log)
    for i in range(n):
        b = Noisy('b%d' % i, log)
    c = Noisy('c', log)
    log.append('return')
    return len(log)


# objects held in locals are freed in the same order as ceval frees them
def free_order(n):
    log = []
    r = run(frees, n, log)
    return r, log


def keeps_frame(n, out):
    out.append(sys._getframe())
    total = 0
    for i in range(n):
        total += i
    return total


# the frame left for ceval at the return says where it ended
def kept_frame(n):
    out = []
    r = run(keeps_frame, n, out)
    f = out[0]
    return (r, f.f_lineno - f.f_code.co_firstlineno, f.f_lasti >= 0,
            sorted(f.f_locals.items(), key=repr)[:1])


def traced_rest(n, events):
    total = 0
    for i in range(n):
        total += i
        if i == n // 2:
            def tracer(frame, event, arg):
                if frame.f_code is traced_rest.__code__:
                    events.append(
                        (event, frame.f_lineno - frame.f_code.co_firstlineno))
                return tracer
            sys.settrace(tracer)
            sys._getframe().f_trace = tracer
    sys.settrace(None)
    return total


# tracing set in the compiled rest: the loop goes on in ceval, which gives
# each line from there
def tracing_started(n):
    events = []
    r = run(traced_rest, n, events)
    return r, events


class Cm:
    def __init__(self, log):
        self.log = log

    def __enter__(self):
        self.log.append('enter')
        return self

    def __exit__(self, *exc):
        self.log.append(('exit', exc[0] is not None))
        return False


def loop_in_with(n, log):
    with Cm(log):
        total = 0
        for i in range(n):
            total += i
        return total


def with_loop(n):
    log = []
    r = run(loop_in_with, n, log)
    return r, log


def with_in_loop(n, log):
    total = 0
    for i in range(n):
        with Cm(log) as c:
            total += i
    return total, len(log)


def with_each_pass(n):
    log = []
    return run(with_in_loop, n, log), log[-2:]


def nest(n):
    total = 0
    for i in range(n):
        j = 0
        while j < i:
            total += j
            j += 1
    return total


def nested_loops(n):
    return run(nest, n)


def loop_in_except(n):
    try:
        raise KeyError
    except KeyError:
        total = 0
        for i in range(n):
            total += i
        return total


def head_in_except(n):
    return run(loop_in_except, n)


def returns_none(n):
    for i in range(n):
        pass


def returns_const(n):
    return run(returns_none, n)


def in_thread(n):
    out = []
    t = threading.Thread(target=lambda: out.append(run(sum_to, n)))
    t.start()
    t.join()
    return out


def gettrace_seen(n, seen):
    for i in range(n):
        seen.add(sys.gettrace())
    return len(seen)


audits = []


def audit(event, args):
    if event == 'sys.settrace':
        audits.append(event)


def hidden(n):
    if not audits:
        sys.addaudithook(audit)
        audits.append('hook')
    seen = set()
    run(gettrace_seen, n, seen)
    return seen, audits


# whether each shape was taken over: LeJIT's own count, for a checked build
def taken_over(n):
    out = []
    for fn, args in ((sum_to, (n,)), (loop_in_except, (n,)),
                     (nest, (n,)), (returns_none, (n,))):
        before = switches()
        run(fn, *args)
        out.append(switches() > before)
    return out


# Where sys.monitoring counts the jumps, a few hundred blocks more are kept
# over the first rounds after a collection where LeJIT turns the code's
# events off from its callback, and none in the next rounds (blocks_level
# checks that instead); before, the usual leak check
WARM = {}
if hasattr(sys, 'monitoring'):
    WARM = {'leaks': 'blocks kept in early rounds only: see blocks_level'}


# the blocks allocated stop growing, for a way out: some rounds after a
# collection keep none, where a leak would keep some in each. Measured on
# the first call, which later calls (the leak check's rounds) give again.
level = []


def blocks_level(name, n, rounds):
    import gc
    fn = globals()[name]
    if not level:
        kept = []
        for _ in range(8):
            gc.collect()
            before = sys.getallocatedblocks()
            for _ in range(rounds):
                fn(n)
            gc.collect()
            kept.append(sys.getallocatedblocks() - before)
        level.append(min(kept[2:]) < 20)
    return level[0]


CASES = [
    ('for_return', [(5,), (100,)], WARM),
    ('raise_no_handler', [(50,)], WARM),
    ('traceback_identity', [(50,)], WARM),
    ('try_not_matching', [(50,)], WARM),
    ('handled_then_reraised', [(50,)], WARM),
    ('try_caught', [(50,)], WARM),
    ('free_order', [(30,)], WARM),
    ('kept_frame', [(50,)], WARM),
    ('tracing_started', [(40,)], WARM),
    ('with_loop', [(50,)], WARM),
    ('with_each_pass', [(50,)], WARM),
    ('nested_loops', [(30,)], WARM),
    ('head_in_except', [(50,)], WARM),
    ('returns_const', [(50,)], WARM),
    ('in_thread', [(50,)], {'leaks': 'a thread each call'}),
    ('hidden', [(50,)], WARM),
    ('blocks_level', [('for_return', 100, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('raise_no_handler', 50, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('traceback_identity', 50, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('try_not_matching', 50, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('handled_then_reraised', 50, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('free_order', 30, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('kept_frame', 50, 100)], {'leaks': 'checks its own'}),
    ('blocks_level', [('tracing_started', 40, 100)], {'leaks': 'checks its own'}),
    ('taken_over', [(50,)], dict(WARM, checked='reads lejit._core._osr_switches',
                             expected=[[True, False, True, True]])),
]
