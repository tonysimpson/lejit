# Functions with cells of their own (a local or argument that a nested
# function, lambda or generator expression reads), called from compiled
# code: the binder makes the cells for the compiled or inlined callee
# (psyfunc.c:bind_cells), as CPython makes them for the call. Callees with
# no loop are inlined; those with one are called. closures.py tests the
# same code entered from the interpreter.
import sys


class D:
    # records when it is freed
    log = []

    def __init__(self, name):
        self.name = name

    def __del__(self):
        D.log.append(self.name)


def scaled_sum(xs, k):
    m = k + 1
    return sum(x * k + m for x in xs)


def scaled_sum_loop(xs, k):
    t = 0
    for j in range(3):
        t += sum(x * k + j for x in xs)
    return t


def genexpr_callees(n):
    # a generator expression over an argument and a local
    t = 0
    for i in range(n):
        t += scaled_sum((1, 2, 3), i) + scaled_sum_loop([i, 2.5], i)
    return t


def adder(k):
    return lambda x: x + k


def adder_loop(k):
    for _ in range(2):
        f = lambda x: x + k
    return f


def lambdas_kept(n):
    # the cell outlives the call, held by the returned function
    fs = []
    for i in range(n):
        fs.append(adder(i))
        fs.append(adder_loop(i * 10))
    return [f(100) for f in fs]


def counter(start):
    n = start

    def bump():
        nonlocal n
        n += 1
        return n
    return bump


def counter_loop(start):
    n = start
    for _ in range(1):
        def bump():
            nonlocal n
            n += 1
            return n
    return bump


def cell_changed_after(n):
    # the nested function changes the cell after the call has returned
    out = []
    for i in range(n):
        b, c = counter(i), counter_loop(i)
        out.append((b(), b(), c(), c()))
    return out


def set_after_closure(k):
    f = lambda: k
    k = k * 2            # STORE_DEREF on an argument's cell
    g = lambda: k
    return f(), g()


def closure_then_store(n):
    return [set_after_closure(i) for i in range(n)]


def deleted(x):
    y = x
    f = lambda: y
    del y
    try:
        return f()
    except NameError as e:
        return type(e).__name__, str(e)


def deleted_loop(x):
    y = x
    f = lambda: y
    for _ in range(2):
        pass
    del y
    try:
        return f()
    except NameError as e:
        return type(e).__name__, str(e)


def del_cell(n):
    return [(deleted(i), deleted_loop(i)) for i in range(n)]


def unbound_read(flag):
    if flag:
        v = 1
    g = lambda: v
    try:
        return g()
    except NameError as e:
        return type(e).__name__, str(e)


def unbound_cells(n):
    return [unbound_read(i % 2) for i in range(n)]


def own_locals(a):
    b = a * 2
    g = lambda: a + b
    loc = sys._getframe().f_locals
    return sorted((k, v) for k, v in loc.items() if k in ('a', 'b')), g()


def own_locals_loop(a):
    b = a * 2
    g = lambda: a + b
    for _ in range(2):
        loc = sys._getframe().f_locals
    return sorted((k, v) for k, v in loc.items() if k in ('a', 'b')), g()


def f_locals_cells(n):
    return [(own_locals(i), own_locals_loop(i)) for i in range(n)]


def callers_locals():
    # a nested function reads its caller's frame, the callee with the cells
    return sorted((k, v) for k, v in sys._getframe(1).f_locals.items()
                  if k in ('a', 'b'))


def seen_from_inner(a):
    b = a + 1
    g = lambda: a * b
    return callers_locals(), g()


def seen_from_inner_loop(a):
    b = a + 1
    g = lambda: a * b
    for _ in range(2):
        r = callers_locals()
    return r, g()


def frame_from_inner(n):
    return [(seen_from_inner(i), seen_from_inner_loop(i)) for i in range(n)]


def raises(a):
    b = [a]
    g = lambda: b[0] + a
    return g() / 0


def raises_loop(a):
    b = [a]
    g = lambda: b[0] + a
    for _ in range(2):
        pass
    return g() / 0


def tb_locals(f, a):
    try:
        f(a)
    except ZeroDivisionError as e:
        tb = e.__traceback__
        out = []
        while tb is not None:
            co = tb.tb_frame.f_code
            if co is f.__code__:
                loc = tb.tb_frame.f_locals
                out.append((co.co_name, tb.tb_lineno - co.co_firstlineno,
                            sorted((k, v) for k, v in loc.items()
                                   if k in ('a', 'b'))))
            tb = tb.tb_next
        return out


def traceback_cells(n):
    return [(tb_locals(raises, i), tb_locals(raises_loop, i))
            for i in range(n)]


def fact(n):
    # rec is a cell of fact and a free variable of rec
    def rec(k):
        return 1 if k <= 1 else k * rec(k - 1)
    return rec(n)


def down_closure(n):
    # recursion through a function with a cell of its own
    f = lambda: n
    if n == 0:
        return f()
    return down_closure(n - 1) + f()


def recursion(n):
    return [fact(i) for i in range(n)], down_closure(n * 20)


def too_deep(n):
    f = lambda: n
    return too_deep(n + 1) + f()


def recursion_limit():
    out = []
    for _ in range(3):
        try:
            too_deep(0)
        except RecursionError:
            out.append('RecursionError')
        out.append(down_closure(100))
    return out


def tracer_events(n):
    events = []
    codes = (own_locals.__code__, adder.__code__)

    def tracer(frame, event, arg):
        if frame.f_code in codes:
            events.append((frame.f_code.co_name, event,
                           sorted((k, v) for k, v in frame.f_locals.items()
                                  if k in ('a', 'b', 'k'))))
        return tracer
    own_locals(n)
    adder(n)
    sys.settrace(tracer)
    try:
        r = own_locals(n), adder(n)(1)
    finally:
        sys.settrace(None)
    return r, events, own_locals(n), adder(n)(1)


def start_tracing(tracer):
    sys.settrace(tracer)


def traced_midway(a):
    b = a + 1
    g = lambda: a + b
    start_tracing(traced_midway.tracer)
    b = b + 1
    return g()


def traced_from_callee(n):
    traced_midway.tracer = None
    events = []

    def tracer(frame, event, arg):
        if frame.f_code is traced_midway.__code__:
            events.append((event, sorted(
                (k, v) for k, v in frame.f_locals.items() if k in ('a', 'b'))))
        return tracer
    traced_midway.tracer = tracer
    try:
        r = traced_midway(n)
    finally:
        sys.settrace(None)
    traced_midway.tracer = None
    return r, events


def order_locals():
    a = D('a')
    b = D('b')
    c = D('c')
    g = lambda: b
    return 0


def order_arg_cell(x):
    x = D('x')
    a = D('a')
    (lambda: x)
    return 0


def order_arg_cell_loop(x):
    x = D('x2')
    a = D('a2')
    (lambda: x)
    for _ in range(2):
        pass
    return 0


def order_inlined(make):              # inlined: reads no global
    a = make('a')
    b = make('b')
    c = make('c')
    g = lambda: b
    return 0


def order_inlined_raises(make):
    a = make('a')
    b = make('b')
    c = make('c')
    g = lambda: b
    raise ValueError


def free_order_inlined(n):
    # the same for a frame inlined into its caller, left by a return or
    # an exception
    D.log = []
    for _ in range(n):
        order_inlined(D)
        try:
            order_inlined_raises(D)
        except ValueError:
            pass
    return D.log[:6]


def free_order(n):
    # the frame's locals and its cells are freed in its slots' order
    D.log = []
    for _ in range(n):
        order_locals()
        order_arg_cell(0)
        order_arg_cell_loop(0)
    return D.log[:12]


def kept_by_closure(n):
    out = []
    for i in range(n):
        D.log = []
        f = adder(D('k'))
        out.append(list(D.log))
        del f
        out.append(list(D.log))
    return out


def with_defaults(a, b=2, *rest):
    return lambda: (a, b, rest)


def defaults_and_varargs(n):
    return [(with_defaults(i)(), with_defaults(i, 3)(), with_defaults(i, 3, 4, 5)())
            for i in range(n)]


def wrong_count(n):
    out = []
    for i in range(n):
        try:
            adder()
        except TypeError as e:
            out.append(str(e))
        try:
            adder(1, 2)
        except TypeError as e:
            out.append(str(e))
    return out


class Holder:
    def __init__(self, k):
        self.k = k

    def scaled(self, xs):
        k = self.k
        return [x * k for x in xs], sum(x + k for x in xs)

    def method_cell(self):
        return lambda: self.k


def methods(n):
    out = []
    for i in range(n):
        h = Holder(i)
        out.append((h.scaled([1, 2]), h.method_cell()()))
    return out


CASES = [
    ('genexpr_callees', [(20,)], {}),
    ('lambdas_kept', [(5,)], {}),
    ('cell_changed_after', [(4,)], {}),
    ('closure_then_store', [(4,)], {}),
    ('del_cell', [(3,)], {}),
    ('unbound_cells', [(4,)], {}),
    ('f_locals_cells', [(3,)], {}),
    ('frame_from_inner', [(3,)], {}),
    ('traceback_cells', [(3,)], {}),
    ('recursion', [(6,)], {}),
    ('recursion_limit', [()], {}),
    ('tracer_events', [(3,)], {}),
    ('traced_from_callee', [(3,)], {}),
    ('free_order', [(2,)], {}),
    ('free_order_inlined', [(2,)], {}),
    ('kept_by_closure', [(3,)], {}),
    ('defaults_and_varargs', [(3,)], {}),
    ('wrong_count', [(2,)], {}),
    ('methods', [(3,)], {}),
]
