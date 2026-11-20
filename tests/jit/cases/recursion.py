# The recursion depth that compiled calls count inline.
# Depths differ between CPython and Ni (inlined calls aren't counted), so
# the cases return only what both agree on.
import sys


def down(n):
    return down(n + 1) + 1


# no comparison: CPython's int compare counts a level of its own and names
# itself in the message ("in comparison"), compiled code's doesn't
def depth(n):
    if not n:
        return 0
    return depth(n - 1) + 1


def limit_reached():
    try:
        down(0)
    except RecursionError as e:
        return str(e)
    return 'no error'


# RecursionError with CPython's message, and the depth is given back: the
# program can recurse again afterwards, and overflow again at the limit
def recursion_error_recovers():
    out = []
    for i in range(3):
        out.append(limit_reached())
        out.append(depth(500))
    return out


# the limit is read at run time: lowered and raised after compilation
def limit_changed_at_run_time():
    old = sys.getrecursionlimit()
    out = [depth(400)]
    try:
        sys.setrecursionlimit(200)
        try:
            out.append(depth(400))
        except RecursionError as e:
            out.append(str(e))
        sys.setrecursionlimit(5000)
        out.append(depth(2000))
        out.append(limit_reached())
    finally:
        sys.setrecursionlimit(old)
    out.append(depth(400))
    return out


def helper(n):
    return n + 1


# the handler at the limit calls on (CPython allows 50 more calls while
# 'overflowed' is set); leaving clears it, so a second overflow raises again
# instead of running on to "Cannot recover from stack overflow"
def handle_at_limit(n):
    try:
        return handle_at_limit(n + 1)
    except RecursionError:
        helper(n)
        depth(10)
        return 'handled'


def overflow_handled_twice():
    return [handle_at_limit(0), handle_at_limit(0), limit_reached()]


def raise_down(n):
    if not n:
        raise ValueError('bottom')
    try:
        return raise_down(n - 1)
    except ValueError:
        if n % 3:
            raise
        return n


def wrap_down(n):
    try:
        return wrap_down(n + 1)
    except RecursionError:
        raise
    except ValueError:
        return -1


# recursion through exceptions: raised and caught at many levels, and a
# RecursionError re-raised by every level on the way out
def recursion_through_exceptions():
    out = [raise_down(10), raise_down(200)]
    try:
        wrap_down(0)
    except RecursionError as e:
        out.append(str(e))
    out.append(depth(500))
    return out


CASES = [
    ('recursion_error_recovers', [()], {}),
    ('limit_changed_at_run_time', [()], {}),
    ('overflow_handled_twice', [()], {}),
    ('recursion_through_exceptions', [()], {}),
]
