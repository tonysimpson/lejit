# Compiled functions entered from C through the eval hook
# (eval_hook.c:LeCode_Run): map() and sorted() call them, with few and many
# arguments (more than the fixed array of argument words holds), with
# arguments that are cells and with free variables, raising (the traceback
# names the callee) and reaching the recursion limit through C.
import sys


def add3(a, b, c):
    return a + b + c


def add20(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14,
          a15, a16, a17, a18, a19):
    return (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 +
            a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19)


def few_and_many(n):
    few = list(map(add3, range(n), range(n), range(n)))
    many = list(map(add20, *[range(i, i + n) for i in range(20)]))
    return few, many


def cell_arg(x, y):
    def inner():
        return x * 10 + y
    x += 1
    return inner()


def with_cells(n):
    return list(map(cell_arg, range(n), range(n, 0, -1)))


def make_scaled(k):
    def scaled(v):
        return v * k
    return scaled


def with_frees(n):
    return sorted(range(n), key=make_scaled(-1)), list(map(make_scaled(3),
                                                           range(n)))


def fails(v):
    if v == 2:
        raise ValueError(v)
    return v


def raising(n):
    try:
        list(map(fails, range(n)))
    except ValueError as e:
        tb = e.__traceback__
        names = []
        while tb is not None:
            names.append(tb.tb_frame.f_code.co_name)
            tb = tb.tb_next
        return 'raised', names
    return 'no raise'


def deep(n):
    return sum(map(deep, [n - 1])) + 1 if n > 0 else 1


def recursion_through_c(n):
    old = sys.getrecursionlimit()
    sys.setrecursionlimit(60)
    try:
        deep(n)
    except RecursionError:
        return 'limit'
    finally:
        sys.setrecursionlimit(old)
    return 'no limit'


CASES = [
    ('few_and_many', [(0,), (5,)], {}),
    ('with_cells', [(4,)], {}),
    ('with_frees', [(5,)], {}),
    ('raising', [(2,), (4,)], {}),
    ('recursion_through_c', [(3,), (200,)], {}),
]
