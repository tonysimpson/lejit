# The recursion depth that compiled calls count inline.
# Depths differ between CPython and LeJIT (inlined calls aren't counted), so
# the cases return only what both agree on.
import sys
import threading


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


# Generators run from C in CPython (by sum(), next(), send()) and run
# directly by compiled code: each level is a call, then a generator, so
# only C's own check stops the nesting before the stack runs out (since
# sys.monitoring came; before, plain CPython crashes here). Each call
# catches RecursionError, so it is caught where raised. Where the check
# measures the stack, compiled levels take less of it and go far deeper
# than plain CPython's, so the levels run in a thread with a small stack;
# where it counts levels, the thread's stack is the default one, which
# the count fits. The thread gets the GIL when this one waits in join(),
# never by asking for it (the switch interval is far longer than this
# one runs), so no switch lands in the counted rounds' code.
def level_sum(k):
    try:
        return sum(nest_sum(k))
    except RecursionError:
        return -1


def nest_sum(k):
    yield level_sum(k - 1) if k else 1


# a generator's second step is run directly
def level_next(k):
    try:
        g = nest_next(k)
        next(g)
        return next(g)
    except RecursionError:
        return -1


def nest_next(k):
    yield 0
    yield level_next(k - 1) if k else 1


def level_send(k):
    try:
        g = nest_send(k)
        g.send(None)
        return g.send(None)
    except RecursionError:
        return -1


def nest_send(k):
    yield 0
    yield level_send(k - 1) if k else 1


def c_stack_limit():
    if not hasattr(sys, 'monitoring'):
        return None
    try:
        import _testinternalcapi
    except ImportError:
        _testinternalcapi = None
    stack_measured = hasattr(_testinternalcapi, 'get_stack_margin')
    out = []
    old_limit = sys.getrecursionlimit()
    old_size = threading.stack_size(1024 * 1024 if stack_measured else 0)
    old_interval = sys.getswitchinterval()
    sys.setrecursionlimit(10 ** 7)
    sys.setswitchinterval(100)
    try:
        def body():
            for f in (level_sum, level_next, level_send):
                out.append(f(10 ** 6))

        t = threading.Thread(target=body)
        t.start()
        t.join()
    finally:
        sys.setswitchinterval(old_interval)
        sys.setrecursionlimit(old_limit)
        threading.stack_size(old_size)
    return out


CASES = [
    ('recursion_error_recovers', [()], {}),
    ('limit_changed_at_run_time', [()], {}),
    ('overflow_handled_twice', [()], {}),
    ('recursion_through_exceptions', [()], {}),
    ('c_stack_limit', [()], {}),
]
