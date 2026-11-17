# Calls from compiled code to closures that are not the virtual function
# MAKE_FUNCTION made in the caller (KNOWN_BUGS F8): a constant function
# (a decorated global, a method) has its cells read now as constants; one
# known only at run time has its code promoted and its cells loaded from
# func_closure at each call (psyfunc.c:psyco_call_closure). Zero-argument
# super() in a directly called method reads the compiled frame
# (pbltinmodule.c:psuper_noargs).
import sys
import traceback


def make_adder(k):
    def add(x):
        return x + k
    return add


adder = make_adder(3)


def const_closure(n):
    t = 0
    for i in range(n):
        t += adder(i)
    return t


def run(f, n):
    t = 0
    for i in range(n):
        t += f(i)
    return t


def runtime_closures(n):
    # one call site, a new closure each time, different cells
    return [run(make_adder(k), n) for k in (1, 2.5, 'x' and 7, -4)]


def deco(fn):
    def wrapper(*args, **kwargs):
        return fn(*args, **kwargs)
    return wrapper


def deco_pos(fn):
    def wrapper(*args):
        return fn(*args) * 2
    return wrapper


@deco
def wrapped(x, y=1):
    return x + y


@deco_pos
def wrapped_pos(x, y=1):
    return x - y


def decorated(n):
    t = 0
    for i in range(n):
        t += wrapped(i) + wrapped(i, y=i) + wrapped_pos(i) + wrapped_pos(i, 3)
    return t


def ident(f):
    return f


def nonlocal_writes(n):
    total = 0
    k = 1

    def bump(x):
        nonlocal total
        total += x * k
    bump = ident(bump)          # a run-time function now
    for i in range(n):
        bump(i)
        if i == n // 2:
            k = 10              # the cell changes between calls
    return total


def loop_variables(n):
    late = [lambda: i for i in range(4)]            # all see the last i
    early = [(lambda j: lambda: j * 2)(i) for i in range(4)]
    t = []
    for _ in range(n):
        t.append([f() for f in late] + [f() for f in early])
    return t[-1], len(t)


def counter():
    c = 0

    def get():
        return c

    def put(v):
        nonlocal c
        c = v
    return get, put


get_c, put_c = counter()


def cell_changes(n):
    out = []
    for i in range(n):
        out.append(get_c())
        put_c(i * i)
    out.append(get_c())
    return out


def recursion_through_closure(n):
    def fact(k):
        return 1 if k <= 1 else k * fact(k - 1)
    fact = ident(fact)
    return [fact(k) for k in range(n)]


def deep_closure(n):
    def down(k):
        return 0 if k == 0 else 1 + down(k - 1)
    down = ident(down)
    try:
        return down(n)
    except RecursionError:
        return 'RecursionError'


def make_raiser(d):
    def raiser(x):
        return x / d
    return raiser


raise0 = make_raiser(0)


def tracebacks(n):
    out = []
    for f in (raise0, make_raiser(0)):
        try:
            f(n)
        except ZeroDivisionError:
            tb = traceback.extract_tb(sys.exc_info()[2])
            out.append([(e.name, e.line) for e in tb])
    return out


def traced(n):
    adder(1)
    f = make_adder(2)
    run(f, 3)                   # both call sites compiled first
    events = []

    def tracer(frame, event, arg):
        if frame.f_code is f.__code__:
            events.append(event)
        return tracer
    sys.settrace(tracer)
    try:
        r = run(f, n), const_closure(2)
    finally:
        sys.settrace(None)
    return r, events


def make_with_default(k):
    def f(x, y=k):
        return x * y
    return f


with_default = make_with_default(5)


def defaults(n):
    g = make_with_default(7)
    t = [with_default(n), with_default(n, 2), run(g, 3), g(n), g(n, 1)]
    with_default.__defaults__ = (100,)
    t.append(with_default(n))
    with_default.__defaults__ = (5,)
    return t


_ns = {'G': 1000}
exec("def mk(k):\n    def f(x):\n        return x + k + G\n    return f\n", _ns)


def other_globals(n):
    # a closure whose globals aren't the caller's: the real call
    return run(_ns['mk'](1), n), run(make_adder(1), n)


def code_swapped(n):
    # a run-time closure's code is read at each call
    f = make_adder(2)
    a = run(f, n)
    f.__code__ = make_mul(1).__code__
    return a, run(f, n)


def make_mul(k):
    def mul(x):
        return x * k
    return mul


class Base:
    def __init__(self, v):
        self.v = v

    def get(self, k):
        return self.v + k


class Child(Base):
    def __init__(self, v):
        super().__init__(v + 1)     # self is dead after this

    def get(self, k):
        return super().get(k) * 2

    def lam(self):
        return (lambda: super().get(1))()


class Grand(Child):
    def get(self, k):
        return super().get(k) + 1


def super_methods(n):
    c, g = Child(n), Grand(n)
    t = 0
    for i in range(n):
        t += c.get(i) + g.get(i)
    try:
        r = c.lam()
    except RuntimeError as e:
        r = str(e)
    return t, r


def plain_one(a):
    return a


def wrong_counts(n):
    # CPython's TypeError wording, from the real call (F92)
    out = []
    for f in (plain_one, adder, make_adder(n), make_with_default(n)):
        for k in range(4):
            try:
                if k == 0:
                    out.append(f())
                elif k == 1:
                    out.append(f(1))
                elif k == 2:
                    out.append(f(1, 2))
                else:
                    out.append(f(1, 2, 3))
            except TypeError as e:
                out.append(str(e))
    return out


def callnum(num):
    def deco(func):
        return lambda: num
    return deco


class Checker:
    def check(self, a, b, msg):
        return a == b

    def small_closure(self, f):
        # an inlinable closure known at run time, its result an argument of
        # a method call: inlining it crashed (C49), so it is called
        return self.check(f(), 2, "x")


def inlinable_runtime(n):
    c = Checker()
    return [c.small_closure(callnum(k)(None)) for k in range(n)]


CASES = [
    ('inlinable_runtime', [(4,)], {}),
    ('wrong_counts', [(2,)], {}),
    ('const_closure', [(50,)], {}),
    ('runtime_closures', [(20,)], {}),
    ('decorated', [(30,)], {}),
    ('nonlocal_writes', [(20,)], {}),
    ('loop_variables', [(5,)], {}),
    ('cell_changes', [(6,)], {}),
    ('recursion_through_closure', [(8,)], {}),
    ('deep_closure', [(50,), (100000,)], {}),
    ('tracebacks', [(1,)], {}),
    ('traced', [(3,)], {}),
    ('defaults', [(4,)], {}),
    ('other_globals', [(5,)], {}),
    ('code_swapped', [(5,)], {}),
    ('super_methods', [(6,)], {}),
]
