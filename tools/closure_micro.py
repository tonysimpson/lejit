"""Per-call cost of calling a closure from compiled code, against a plain
function (doc/FINDINGS.md). Run under perf twice with different counts and
take the difference per call:

    perf stat -e instructions:u python3.7 tools/closure_micro.py CASE N

CASE is one of the names in CASES. Without ni (NI=0 in the environment) it
runs under plain CPython.
"""
import os
import sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
if os.environ.get('NI', '1') != '0':
    import ni  # noqa: F401


def plain(x):
    return x + 1


def make_adder(k):
    def add(x):
        return x + k
    return add


adder = make_adder(1)


def deco(fn):
    def wrapper(*args, **kwargs):
        return fn(*args, **kwargs)
    return wrapper


def deco_pos(fn):
    def wrapper(*args):
        return fn(*args)
    return wrapper


@deco
def wrapped(x):
    return x + 1


@deco_pos
def wrapped_pos(x):
    return x + 1


def loop_plain(n):
    t = 0
    for i in range(n):
        t += plain(i)
    return t


def loop_const(n):          # a closure in a global: a constant at the call
    t = 0
    for i in range(n):
        t += adder(i)
    return t


def loop_arg(f, n):         # a closure passed in: only its code is known
    t = 0
    for i in range(n):
        t += f(i)
    return t


def loop_nested(n):         # a nested helper over a local, which escapes
    k = 1

    def helper(x):
        return x + k
    keep = [helper]
    t = 0
    for i in range(n):
        t += helper(i)
    return t + len(keep)


def loop_nonlocal(n):       # a nested helper writing a nonlocal
    total = 0

    def bump(x):
        nonlocal total
        total += x
    keep = [bump]
    for i in range(n):
        bump(i)
    return total + len(keep)


def loop_deco(n):
    t = 0
    for i in range(n):
        t += wrapped(i)
    return t


def loop_deco_pos(n):
    t = 0
    for i in range(n):
        t += wrapped_pos(i)
    return t


CASES = {
    'plain': lambda n: loop_plain(n),
    'const': lambda n: loop_const(n),
    'arg': lambda n: loop_arg(make_adder(1), n),
    'nested': lambda n: loop_nested(n),
    'nonlocal': lambda n: loop_nonlocal(n),
    'deco': lambda n: loop_deco(n),
    'deco_pos': lambda n: loop_deco_pos(n),
}

if __name__ == '__main__':
    case, n = sys.argv[1], int(sys.argv[2])
    CASES[case](1000)       # compile
    print(case, CASES[case](n))
