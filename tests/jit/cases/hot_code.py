# Code that ceval or C calls compiles only once hot (eval_hook.c:
# le_code_to_run): at its 100th call, or once its loops have jumped back
# LE_HOT_JUMPS (1,000) times, where the frame making the last jump is taken
# over at that loop head (osr.c). tests/jit runs every other case with
# code compiling at its first call (child.py), so these set the rule back
# for their own run, on functions made afresh for each run. Each is called
# through map, so that C calls it. A release build can't say what compiled.
import sys

HOT_CALLS = 100

SRC = '''
def square(x):
    return x * x

def long_loop(n):
    total = 0
    for i in range(n):
        total += i % 7
    return total

def short_loop(n):
    total = 0
    for i in range(n):
        total += i
    return total

def raises_late(n):
    for i in range(n):
        if i == n - 1:
            raise ValueError(i)
'''


def core():
    c = getattr(sys.modules.get('lejit'), '_core', None)
    return c if hasattr(c, '_hot_calls') else None


# whether fn has compiled; 'want' where nothing can say (plain CPython, a
# release build)
def compiled(fn, want):
    c = core()
    if c is None:
        return want
    return c._code_info(fn.__code__) == 'compiled'


# frames taken over at a loop head so far; 0 where nothing can say
def switches():
    c = core()
    return c._osr_switches() if c is not None else 0


def with_rule(body):
    c = core()
    old = c._hot_calls(HOT_CALLS) if c is not None else None
    fns = {}
    exec(SRC, fns)
    try:
        return body(fns)
    finally:
        if old is not None:
            c._hot_calls(old)


def call(fn, *args):
    return next(map(fn, *[[a] for a in args]))


def no_loop_calls():
    def body(fns):
        square = fns['square']
        out = []
        for i in range(HOT_CALLS + 5):
            out.append(call(square, i))
            if i == HOT_CALLS - 3:
                out.append(compiled(square, False))
        out.append(compiled(square, True))
        return out
    return with_rule(body)


# one call whose loop runs long is taken over in it, and the next call
# compiles at its start
def called_once_loop(n):
    def body(fns):
        long_loop = fns['long_loop']
        s = switches()
        r = [call(long_loop, n), switches() - s == (core() is not None)]
        r.append(call(long_loop, 10))
        r.append(compiled(long_loop, True))
        return r
    return with_rule(body)


# loops that jump back fewer times wait for the 100th call
def short_loops():
    def body(fns):
        short_loop = fns['short_loop']
        out = [call(short_loop, 3) for _ in range(HOT_CALLS - 2)]
        early = compiled(short_loop, False)
        out += [call(short_loop, 3) for _ in range(5)]
        return sum(out), early, compiled(short_loop, True)
    return with_rule(body)


# raised after the take-over: the traceback is CPython's
def taken_over_raises(n):
    def body(fns):
        try:
            call(fns['raises_late'], n)
        except ValueError as e:
            tb = e.__traceback__
            names = []
            while tb is not None:
                names.append(tb.tb_frame.f_code.co_name)
                tb = tb.tb_next
            return e.args, names
    return with_rule(body)


CASES = [
    ('no_loop_calls', [()], {}),
    ('called_once_loop', [(5000,)], {}),
    ('short_loops', [()], {}),
    ('taken_over_raises', [(5000,)], {}),
]
