# An exception leaving a deep stack of compiled calls: each call's
# traceback entry finds the innermost compiled frame, which must not take
# a walk of the whole stack each time. 20,000 levels, 110 rounds: quick
# when each level costs the same, past the runner's timeout when each
# costs a walk. Run in a thread with a stack big enough for plain
# CPython's levels where they take C stack, once this thread waits in
# join(): no switch to it lands in the counted rounds' code.
import json
import json.scanner
import sys
import threading

DEPTH = 20000


def raise_at_bottom(n):
    if not n:
        raise ValueError('bottom')
    return raise_at_bottom(n - 1) + 1


def shallow(n):
    if not n:
        return 0
    return shallow(n - 1) + 1


# some levels catch it, call deeper while handling it, and raise it again
def reraise_on_the_way(n):
    if not n:
        raise ValueError('bottom')
    try:
        return reraise_on_the_way(n - 1) + 1
    except ValueError:
        if n % 1000 == 0:
            shallow(50)
        raise


def nest_sum(k):
    if not k:
        raise ValueError('bottom')
    yield sum(nest_sum(k - 1))


def entries(tb):
    count = linked = 0
    lines = set()
    while tb is not None:
        nxt = tb.tb_next
        if nxt is not None and nxt.tb_frame.f_back is tb.tb_frame:
            linked += 1
        lines.add(tb.tb_lineno)
        count += 1
        tb = nxt
    return count, linked, sorted(lines)


def unwound(f, n):
    try:
        f(n)
    except ValueError as e:
        return entries(e.__traceback__)


def run(f, n):
    out = []
    old_limit = sys.getrecursionlimit()
    old_size = threading.stack_size(256 * 1024 * 1024)
    sys.setrecursionlimit(3 * n + 1000)
    try:
        go = threading.Event()

        def body():
            go.wait()
            out.append(unwound(f, n))

        t = threading.Thread(target=body)
        t.start()
        go.set()
        t.join()
    finally:
        sys.setrecursionlimit(old_limit)
        threading.stack_size(old_size)
    return out


def deep_calls():
    return run(raise_at_bottom, DEPTH)


def deep_reraises():
    return run(reraise_on_the_way, DEPTH)


# generator bodies run directly by compiled code; plain CPython takes C
# stack for each level here, so these stay shallow
def deep_generators():
    return run(lambda n: sum(nest_sum(n)), 2000)


# json's pure-Python scanner, an array in an array 2,000 deep left by the
# error at the end of the text: each level's frame is kept as the
# traceback passes, and must be found again without a search each time
def json_arrays(n):
    scan = json.scanner.py_make_scanner(json.JSONDecoder())
    return scan('[' * n, 0)


def deep_json():
    return run(json_arrays, 2000)


CASES = [
    ('deep_calls', [()], {}),
    ('deep_json', [()], {}),
    ('deep_reraises', [()], {}),
    ('deep_generators', [()], {}),
]
