# Code that ceval or C calls compiles at its first call where it has a
# loop, else at its 100th (eval_hook.c:le_code_to_run). tests/jit runs
# every other case with all code compiling at its first call (child.py), so
# these set the rule back for their own run, on functions made afresh for
# each run. Each is called through map, so that C calls it. A release
# build can't say what compiled.
import sys

HOT_CALLS = 100

SRC = '''
def square(x):
    return x * x

def short_loop(n):
    total = 0
    for i in range(n):
        total += i
    return total
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


# code with a loop compiles at its first call
def loop_first_call():
    def body(fns):
        short_loop = fns['short_loop']
        r = [call(short_loop, 3), compiled(short_loop, True)]
        r.append(call(short_loop, 4))
        return r
    return with_rule(body)


CASES = [
    ('no_loop_calls', [()], {}),
    ('loop_first_call', [()], {}),
]
