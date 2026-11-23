# Generator functions consumed whole by sum, any, all, min, max, list,
# tuple, set and str.join run fused with the consumer, as generator
# expressions do (psyfunc.c:psyco_call_genexpr): the call binds the
# arguments, and no generator object or frame is made. Only a temporary
# with one yield and no handler around it is fused; any other is a real
# generator.
import sys


class D:
    log = []

    def __init__(self, name):
        self.name = name

    def __del__(self):
        D.log.append(self.name)


def counted(n, k=2):
    for i in range(n):
        yield i * k


def evens(xs):
    for x in xs:
        if x % 2 == 0:
            yield x


def straight(a, b):
    yield a + b


def consumers(n):
    return (list(counted(n)), tuple(counted(n, 3)), sorted(set(counted(n))),
            sum(counted(n)), sum(counted(n), 100), sum(counted(n, 0.5)),
            any(counted(n)), all(counted(n)), any(evens([1, 3])),
            all(evens([2, 4])), min(counted(n)), max(counted(n)),
            '-'.join(str(x) for x in counted(n)), list(straight(n, 1)),
            list(evens(range(n))))


def empty(n):
    out = []
    for f in (list, tuple, set, sum, any, all):
        out.append(f(counted(0)))
    for f in (min, max):
        try:
            f(counted(0))
        except ValueError as e:
            out.append(str(e))
    return out


def with_varargs(*xs):
    for x in xs:
        yield x + 1


def keyword_only(n, *, k=1):
    for i in range(n):
        yield i * k


def arguments(n):
    return (list(counted(n)), list(counted(n, 5)), list(with_varargs()),
            list(with_varargs(n, n + 1)), list(keyword_only(n)),
            list(keyword_only(n, k=3)), list(counted(n, k=4)),
            list(counted(k=1, n=n)), list(counted(*(n, 2))))


def wrong_count(n):
    out = []
    for args in ((), (1, 2, 3)):
        try:
            g = sum(counted(*args))
            out.append(('no error', g))
        except TypeError as e:
            out.append(str(e))
    try:
        list(straight(n))
    except TypeError as e:
        out.append(str(e))
    return out


def set_default(v):
    counted.__defaults__ = (v,)
    return 0


def defaults_bound_at_call(n):
    # the defaults are the call's, though changed before the consumer runs
    try:
        return sum(counted(n), set_default(10)), list(counted(n))
    finally:
        counted.__defaults__ = (2,)


def returns_value(n):
    for i in range(n):
        if i == 3:
            return 'done'
        yield i
    return [n]


def return_values(n):
    return list(returns_value(n)), sum(returns_value(n)), any(returns_value(0))


def stops(n):
    for i in range(n):
        if i == 2:
            raise StopIteration('inner')
        yield i


def pep479(n):
    out = []
    for f in (list, sum, any):
        try:
            out.append(f(stops(n)))
        except RuntimeError as e:
            out.append((type(e).__name__, str(e), type(e.__cause__).__name__))
    return out


def fails(n):
    for i in range(n):
        yield 10 // (2 - i)


def traceback_frames(n):
    try:
        list(fails(n))
    except ZeroDivisionError as e:
        out = []
        tb = e.__traceback__
        while tb is not None:
            f = tb.tb_frame
            out.append((f.f_code.co_name, tb.tb_lineno - f.f_code.co_firstlineno,
                        f.f_back is None if f.f_code is fails.__code__ else '-',
                        sorted((k, v) for k, v in f.f_locals.items()
                               if k in ('i', 'n'))))
            tb = tb.tb_next
        return out


def finalised(n):
    d = D('local')
    for i in range(n):
        yield i


def finalised_last(n):
    for i in range(n):
        d = D(i)
        yield i


def early_stop(n):
    out = []
    for f in (any, all):
        D.log = []
        r = f(finalised(n))
        out.append((r, list(D.log)))
        D.log = []
        r = f(finalised_last(n))
        out.append((r, list(D.log)))
    return out


def own_cells(n):
    cols = range(n)
    for v in [(0, 1, 2), (2, 1, 0), (1, 0, 2)]:
        if n == len(set(v[i] + i for i in cols)) == len(set(v[i] - i for i in cols)):
            yield v
        yield (lambda: v)()


def cells_in_generator(n):
    return list(own_cells(n)), sum(len(v) for v in own_cells(n))


def nested_generator(n):
    k = n * 2

    def inner(m):
        for i in range(m):
            yield i + k
    return list(inner(n)), sum(inner(n)), max(inner(n))


class Box:
    def __init__(self, xs):
        self.xs = xs

    def items(self, k):
        for x in self.xs:
            yield x * k


def methods(n):
    b = Box(list(range(n)))
    return list(b.items(2)), sum(b.items(3))


def yield_in_try(n):
    try:
        for i in range(n):
            yield i
    finally:
        D.log.append('finally')


def yield_in_with(n):
    class C:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            D.log.append('exit')
    with C():
        for i in range(n):
            yield i


def try_elsewhere(n):
    try:
        k = 10 // n
    except ZeroDivisionError:
        k = -1
    for i in range(3):
        yield i * k


def handlers(n):
    # a yield inside try or with is never fused: closing it runs code
    D.log = []
    r = (any(yield_in_try(n)), list(yield_in_try(n)), all(yield_in_with(n)),
         list(try_elsewhere(n)), list(try_elsewhere(0)))
    return r, D.log


def two_yields(n):
    yield n
    yield n + 1


def yield_from(n):
    yield from range(n)


def not_fused(n):
    return list(two_yields(n)), sum(yield_from(n)), list(yield_from(n))


def escapes(n):
    g = counted(n)
    first = next(g)
    rest = list(g)
    h = counted(n)
    both = list(zip(h, h))
    kept = counted(n)
    return first, rest, both, sum(kept), list(kept), list(iter(counted(n)))


def passed_twice(n):
    g = counted(n)
    return list(g), list(g), type(counted(n)).__name__


def names(n):
    g = counted(n)
    return g.__name__, g.__qualname__, g.gi_running, g.gi_frame is not None


def deep(d, n):
    if d == 0:
        return sum(counted(n))
    return deep(d - 1, n) + 0


def recursion_limit(n):
    out = []
    for _ in range(3):
        try:
            out.append(deep(n, 3))
        except RecursionError:
            out.append('RecursionError')
    return out


def recursion_edge(n):
    # any() stopping early at each depth up to the limit: CPython closes the
    # generator there, which could fail and be reported as unraisable.
    # Depths differ under LeJIT (inlined calls aren't counted), so only the
    # outcomes are compared.
    old = sys.getrecursionlimit()
    unraisable = []
    old_hook = getattr(sys, 'unraisablehook', None)
    if old_hook is not None:
        sys.unraisablehook = lambda u: unraisable.append(u.exc_type.__name__)
    out = set()
    try:
        sys.setrecursionlimit(80)
        for d in range(50, 80):
            try:
                out.add(deep_any(d))
            except RecursionError:
                out.add('RecursionError')
    finally:
        sys.setrecursionlimit(old)
        if old_hook is not None:
            sys.unraisablehook = old_hook
    return sorted(out, key=str), unraisable


def deep_any(d):
    if d == 0:
        return any(counted(3))
    return deep_any(d - 1)


class Num:
    def __init__(self, v):
        self.v = v

    def __add__(self, o):
        return Num(self.v + (o.v if isinstance(o, Num) else o))

    __radd__ = __add__


def values(xs):
    for x in xs:
        yield x


def many_types(n):
    # one generator, many item types: fusing stops once its sites go
    # megamorphic (mergepoints.h:psyco_fuse_stopped)
    out = []
    for i in range(n):
        for xs in ([1, 2], [1.5, 2], [Num(1)], ['a', 'b'], [(1,), (2,)],
                   [[1], [2]], [b'a'], [True], [1j], [frozenset()]):
            try:
                out.append(repr(sum(values(xs)))[:20])
            except TypeError as e:
                out.append(str(e))
            out.append(len(list(values(xs))))
    return out


def tracing(n):
    events = []

    def tracer(frame, event, arg):
        if frame.f_code is counted.__code__:
            events.append(event)
        return tracer
    sum(counted(n))
    sys.settrace(tracer)
    try:
        r = sum(counted(n))
    finally:
        sys.settrace(None)
    return r, events


CASES = [
    ('consumers', [(5,), (0,)], {}),
    ('empty', [(0,)], {}),
    ('arguments', [(3,)], {}),
    ('wrong_count', [(2,)], {}),
    ('defaults_bound_at_call', [(3,)], {}),
    ('return_values', [(5,), (2,)], {}),
    ('pep479', [(4,)], {}),
    ('traceback_frames', [(4,)], {}),
    ('early_stop', [(3,)], {}),
    ('cells_in_generator', [(3,), (2,)], {}),
    ('nested_generator', [(4,)], {}),
    ('methods', [(4,)], {}),
    ('handlers', [(3,)], {}),
    ('not_fused', [(3,)], {}),
    ('escapes', [(4,)], {}),
    ('passed_twice', [(3,)], {}),
    ('names', [(3,)], {}),
    ('recursion_limit', [(30,), (100000,)], {}),
    ('recursion_edge', [(0,)], {}),
    ('many_types', [(2,)], {}),
    ('tracing', [(4,)], {}),
]


def stored_locals(n):
    # a generator a local holds is made as it is stored, so the frame's
    # locals show it
    g = counted(n)
    h = (x for x in range(n))
    loc = sys._getframe().f_locals
    return sorted(loc), type(loc['g']).__name__, list(g), list(h)


CASES.append(('stored_locals', [(3,)], {}))


class Holder:
    def items(self, n):
        for i in range(n):
            yield i


def method_on_temporary(n):
    # the instance lives only as the bound method's self, and the method
    # is gone before the generator is made
    g = Holder().items(n)
    return list(g)


CASES.append(('method_on_temporary', [(3,)], {}))
