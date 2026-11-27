# Compiled functions called from C (map, sorted, functools.partial) with
# keywords, defaults, keyword-only arguments and *args, which may run with no
# frame (eval_hook.c:le_frameless_vectorcall), keywords named by equal
# strings, and the calls that must go on as CPython's: argument errors,
# keywords named by a str subclass, other globals or builtins, tracing,
# generators, defaults replaced while the call runs, an __exit__ that
# frees the traceback it was lent.
import functools, sys, types


def f(x, y=2, *, z=3):
    return x * 100 + y * 10 + z


try:  # positional-only parameters are newer syntax
    exec('def pos_only(a, /, b):\n    return a - b\n')
except SyntaxError:
    pos_only = None


def keywords_and_defaults(n):
    return (list(map(f, range(n))), list(map(f, range(n), range(n))),
            [functools.partial(f, z=7)(i) for i in range(n)],
            [functools.partial(f, y=i, z=i)(i) for i in range(n)],
            [functools.partial(f, i)(z=1) for i in range(n)],
            sorted(range(n), key=functools.partial(f, y=0, z=0)))


def error(call):
    try:
        return call()
    except TypeError as e:
        return str(e)


def bind_errors(n):
    out = []
    for i in range(n):
        out.append(error(lambda: list(map(f, [1], [2], [3]))))
        out.append(error(lambda: functools.partial(f, w=1)(1)))
        out.append(error(lambda: functools.partial(f, 1, x=2)()))
        out.append(error(lambda: functools.partial(f, y=1)()))
        if pos_only is not None:
            out.append(error(lambda: functools.partial(pos_only, a=1)(2)))
            out.append(error(lambda: functools.partial(pos_only, b=1)(5)))
    return out


class Name(str):
    pass


def names_not_identical(n):
    y = ''.join(['y'])
    return ([functools.partial(f, **{y: 5})(i) for i in range(n)],
            [functools.partial(f, **{Name('y'): 6})(i) for i in range(n)],
            error(lambda: functools.partial(f, **{''.join(['x']): 1})(2)))


def h(a, b=(1,), *, k=[2]):
    h.__defaults__ = ((a,),)
    h.__kwdefaults__ = {'k': [a]}
    return a, b, k


def defaults_replaced(n):
    h.__defaults__ = ((1,),)
    h.__kwdefaults__ = {'k': [2]}
    return list(map(h, range(n)))


def g(x):
    return x + K + len(())


K = 1


def other_globals(n):
    other = types.FunctionType(g.__code__, {'K': 10, '__builtins__': __builtins__})
    mine = {'len': lambda s: 1000}
    rebuilt = types.FunctionType(g.__code__, {'K': 20, '__builtins__': mine})
    return list(map(g, range(n))), list(map(other, range(n))), list(
        map(rebuilt, range(n)))


def traced(n):
    events = []

    def tracer(frame, event, arg):
        if frame.f_code is g.__code__:
            events.append(event)
        return tracer
    first = list(map(g, range(n)))
    sys.settrace(tracer)
    try:
        second = list(map(g, range(n)))
    finally:
        sys.settrace(None)
    return first, second, events


def gen(x):
    yield x
    yield x + 1


def generators(n):
    return [list(it) for it in map(gen, range(n))]


def star(a, *rest, b=1, **kw):
    return a, rest, b, kw


class Loop:
    def call_soon(self, callback, *args, context=None):
        return callback, args, context


def star_arguments(n):
    loop = Loop()
    return (list(map(star, range(n))), list(map(star, range(n), range(n))),
            [functools.partial(star, b=i)(i, i) for i in range(n)],
            error(lambda: functools.partial(star, c=1)(0)),
            [functools.partial(loop.call_soon, len, context=i)(i)
             for i in range(n)])


class DropsTraceback:
    def __enter__(self):
        return self

    def __exit__(self, kind, value, tb):
        value.with_traceback(None)
        # what tb's memory would be taken by if it were freed now
        junk = [(i, i, i, i, i) for i in range(1000)]
        return type(tb) is types.TracebackType and tb.tb_lineno > 0 and len(
            junk) and kind is ValueError


# a with statement outside a function runs in ceval, which calls
# __exit__ from C with the traceback lent
WITH_IN_CEVAL = compile('with cm:\n    raise ValueError(i)\nout.append(i)',
                        '<with>', 'exec')


def with_in_ceval(i, out):
    exec(WITH_IN_CEVAL, {'cm': DropsTraceback(), 'i': i, 'out': out})


def exit_drops_traceback(n):
    out = []
    for i in range(n):
        with_in_ceval(i, out)
    return out


def who():
    return sys._getframe(1).f_code.co_name


def caller_frames(n):
    return [who() for _ in range(n)], list(map(lambda _: who(), range(n))), [
        w() for w in [who] * n]


CASES = [
    ('keywords_and_defaults', [(0,), (5,)], {}),
    ('bind_errors', [(2,)], {}),
    ('names_not_identical', [(3,)], {}),
    ('defaults_replaced', [(4,)], {}),
    ('other_globals', [(3,)], {}),
    ('traced', [(3,)], {}),
    ('generators', [(3,)], {}),
    ('star_arguments', [(3,)], {}),
    ('exit_drops_traceback', [(3,)], {}),
    ('caller_frames', [(3,)], {}),
]
