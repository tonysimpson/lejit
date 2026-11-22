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
        r = str(e), getattr(e, 'name', None)
    if flag:
        w = 1
    return r


def rebind_arg_cell(k):
    f = lambda: k
    before = f()
    k = k + 1                 # STORE_DEREF on a cell argument
    return before, f(), k


def calls_from_list(k):
    # new lambdas each call, called where LeJIT didn't see them made: the call
    # site promotes their (shared) code, not each function object
    fs = [lambda: k, lambda: k * 2]
    return [f() for f in fs]


def nqueens_like(n):
    cols = range(n)
    return sum(1 for vec in ((i, (i * 3) % n, (i * 5) % n) for i in cols)
               if n == len(set(vec[i] + i for i in range(3))) + n - 3)


def make_adders(n):
    return [(lambda x, i=i: x + i)(10) for i in range(n)]


class Base:
    def __init__(self, v):
        self.v = v

    def get(self, k):
        return self.v + k


class WithSuper(Base):
    def __init__(self, v):
        super().__init__(v * 2)     # __class__ and the frame's self

    def get(self, k):
        t = 0
        for i in range(k):
            t += super().get(i)
        return t

    def rebinds(self, k):
        x = super().get(k)
        self = None                 # rebinds the first argument: interpreted
        return x, self


class SuperOfList(list):
    def size(self):
        return super().__len__()


def super_call(n):
    w = WithSuper(n)
    return w.get(5), w.rebinds(3), SuperOfList(range(n)).size()


# Comprehensions and lambdas whose function is still the one MAKE_FUNCTION
# made: the call is compiled (or inlined) with the cells passed in
# (psyfunc.c:psyco_call_pyfunc) instead of a real call.

def comp_reads_cells(xs, k, d):
    t = [x + k for x in xs]
    m = {x: d.get(x, k) for x in xs}
    s = {x * k for x in xs}
    return t, sorted(m.items()), sorted(s)


def comp_cell_changes(n):
    out = []
    k = 0
    for i in range(n):
        out.append([x * k for x in range(3)])
        k = k + i            # the cell changes between comprehensions
    return out


def comp_raises(xs, k):
    try:
        return [x / k for x in xs]
    except ZeroDivisionError as e:
        return 'caught', str(e)


def local_lambda(n, k):
    add = lambda x: x + k   # kept in a local, still virtual
    t = 0
    for i in range(n):
        t = add(t)
        if i == 3:
            k = 100          # read through the cell on the next call
    return t


def nested_comps(rows, k):
    # the outer comprehension passes k on to the inner one as a free variable
    return [[c + k for c in row] for row in rows]


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
    ('comp_reads_cells', [([1, 2, 3], 5, {1: 'a'}), ((2.5,), 1, {})], {}),
    ('comp_cell_changes', [(6,)], {}),
    ('comp_raises', [([1, 2], 2), ([1, 2], 0)], {}),
    ('local_lambda', [(10, 1), (3, 2.5)], {}),
    ('nested_comps', [([[1, 2], [3]], 10)], {}),
]
