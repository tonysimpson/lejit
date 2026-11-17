# Code with cell and free variables, compiled at the frame entry
# (pycompiler.c LOAD_CLOSURE, LOAD_DEREF, STORE_DEREF): genexprs and
# lambdas over locals and over arguments (cell arguments, which CPython
# moves into the cell and leaves NULL), nonlocal writes from an inner
# function between reads, unbound cells, and free variables read by an
# inner function that is itself compiled.


def genexpr_over_local(n):
    t = 0
    for k in range(n):
        t += sum(x + k for x in (1, 2, 3))
    return t


def genexpr_over_arg(xs, k):
    return [sum(x * k for x in xs) for _ in range(3)], k


def lambda_counter(n):
    count = 0

    def bump(d):
        nonlocal count
        count += d
    for i in range(n):
        bump(i)
        if count > 1000:      # read after a write through the inner function
            count = count % 7  # STORE_DEREF
    return count


def inner_reads(a, b):
    def f(x):
        return x * a + b      # free variables (LOAD_DEREF in f)
    t = 0
    for i in range(50):
        t += f(i)
    return t


def unbound_cell(flag):
    if flag:
        v = 5

    def g():
        return v
    try:
        return v + g()
    except UnboundLocalError as e:
        return str(e)


def unbound_free(flag):
    def g():
        return w
    try:
        r = g()
    except NameError as e:
        r = str(e)
    if flag:
        w = 1
    return r


def rebind_arg_cell(k):
    f = lambda: k
    before = f()
    k = k + 1                 # STORE_DEREF on a cell argument
    return before, f(), k


def calls_from_list(k):
    # new lambdas each call, called where Ni didn't see them made: the call
    # site promotes their (shared) code, not each function object
    fs = [lambda: k, lambda: k * 2]
    return [f() for f in fs]


def nqueens_like(n):
    cols = range(n)
    return sum(1 for vec in ((i, (i * 3) % n, (i * 5) % n) for i in cols)
               if n == len(set(vec[i] + i for i in range(3))) + n - 3)


def make_adders(n):
    return [(lambda x, i=i: x + i)(10) for i in range(n)]


class WithSuper(list):
    def size(self):
        return super().__len__()   # __class__ cell: stays interpreted


def super_call(n):
    return WithSuper(range(n)).size()


CASES = [
    ('genexpr_over_local', [(200,)], {}),
    ('genexpr_over_arg', [([1, 2, 3], 4), ((1.5,), 2)], {}),
    ('lambda_counter', [(100,), (500,)], {}),
    ('inner_reads', [(3, 4), (2.5, 1)], {}),
    ('unbound_cell', [(True,), (False,)], {}),
    ('unbound_free', [(True,), (False,)], {}),
    ('rebind_arg_cell', [(5,), (2.5,)], {}),
    ('calls_from_list', [(5,), (2.5,)], {}),
    ('nqueens_like', [(5,), (8,)], {}),
    ('make_adders', [(6,)], {}),
    ('super_call', [(7,)], {}),
]
