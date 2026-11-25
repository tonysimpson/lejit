# Small bodies that read globals are inlined into their compiled caller: a
# global changed, deleted or shadowed between calls, or by the inlined body
# itself, must be seen as CPython sees it, and the inlined frame's
# tracebacks and frames must be CPython's.
import sys
import types

SCALE = 2


def scaled(x):                         # a module global
    return x * SCALE


def rebound(n):
    global SCALE
    out = []
    for i in range(n):
        out.append(scaled(i))
        if i == n // 2:
            SCALE = 3.5                # an int, then a float
    SCALE = 2
    return out


def deleted(n):
    global SCALE
    out = []
    for i in range(n):
        if i == n // 2:
            del SCALE
        try:
            out.append(scaled(i))
        except NameError as e:
            tb = e.__traceback__
            out.append((str(e), tb.tb_next.tb_frame.f_code.co_name,
                        tb.tb_next.tb_lineno -
                        tb.tb_next.tb_frame.f_code.co_firstlineno))
    SCALE = 2
    return out


def length(x):                         # a builtin, later shadowed
    return len(x) + 1


def shadowed(n):
    out = []
    for i in range(n):
        if i == n // 2:
            globals()['len'] = lambda x: -1
        out.append(length('abc'))
    del globals()['len']
    return out


def bump():                            # changes the global it reads
    global COUNT
    COUNT = COUNT + 1
    return COUNT


COUNT = 0


def stores(n):
    global COUNT
    COUNT = 0
    return [bump() for i in range(n)]


def own_globals(n):
    # a function whose globals are another dict reads that dict's names
    g = {'SCALE': 10, '__builtins__': __builtins__}
    other = types.FunctionType(scaled.__code__, g)
    out = []
    for i in range(n):
        out.append((other(i), scaled(i)))
        if i == n // 2:
            g['SCALE'] = 20
    return out


def frame_info(x):                     # reads its frame and caller
    f = sys._getframe(0)
    return (x + 1, f.f_code.co_name, f.f_back.f_code.co_name,
            f.f_globals is globals(), f.f_lineno - f.f_code.co_firstlineno)


def frames(n):
    return [frame_info(i) for i in range(n)]


def locals_at_raise(a):                # its locals, from a traceback
    s = a * SCALE
    return s // (a - a)


def traceback_locals(n):
    out = []
    for i in range(n):
        try:
            locals_at_raise(i)
        except ZeroDivisionError as e:
            f = e.__traceback__.tb_next.tb_frame
            out.append((sorted(f.f_locals.items()), f.f_code.co_name))
    return out


def first(xs):                         # a loop that returns from its first
    for x in xs:                       # round: on 3.7 a loop block
        return x
    else:
        raise KeyError('empty')


def first_items(n):
    out = []
    for i in range(n):
        try:
            out.append(first([i] if i % 3 else []))
        except KeyError as e:
            out.append(str(e))
    return out


async def started(unread, box, factory):
    try:
        box.append(1)
        return factory()
    except ZeroDivisionError:
        box.clear()
        raise


async def awaits_started(factory):
    return await started(None, [], factory)


def coroutine_raises_inlined(n):
    # a raise in a body inlined into a coroutine maps the coroutine's
    # locals, 'unread' still its frame's where coroutines run on their frame
    out = []
    for i in range(n):
        def factory():                 # inlined: reads a global
            raise ZeroDivisionError
        try:
            awaits_started(factory).send(None)
        except ZeroDivisionError:
            out.append(i)
    return out


CASES = [
    ('rebound', [(8,)], {}),
    ('deleted', [(6,)], {}),
    ('shadowed', [(6,)], {}),
    ('stores', [(5,)], {}),
    ('own_globals', [(6,)], {}),
    ('frames', [(4,)], {}),
    ('traceback_locals', [(3,)], {}),
    ('first_items', [(6,)], {}),
    ('coroutine_raises_inlined', [(3,)], {}),
]
