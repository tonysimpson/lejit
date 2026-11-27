"""Deep Python recursion: where CPython's Python calls take no C stack, it
returns at any depth the recursion limit allows, and LeJIT's compiled
calls must too, by moving to a stack segment when the stack runs low.
Each case runs in a fresh process, plain and with LeJIT, and the output
must match. A case plain CPython can't run (its calls take C stack, and
it overflows) is skipped.

    python3.X tests/jit/deep_stacks.py [-k substring]
"""
import os, subprocess, sys, threading, weakref

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
TIMEOUT = 120


def down(n):
    return down(n - 1) + 1 if n else 0


def deep_returns():
    sys.setrecursionlimit(10 ** 6 + 100)
    return [down(1000), down(10 ** 6), down(10 ** 6)]


def kdown(n, *, step=1):
    return kdown(n - step, step=step) + 1 if n else 0


def adown(n, step):
    return adown(n=n - step, step=step) + 1 if n else 0


def deep_keyword_calls():
    sys.setrecursionlimit(10 ** 6 + 100)
    return [kdown(3 * 10 ** 5), adown(3 * 10 ** 5, 1)]


# calls compiled code makes through CPython, which CPython makes with no
# C stack
def vkdown(n, **kw):
    return vkdown(n - 1, **kw) + 1 if n else 0


def vadown(n, *a):
    return vadown(n - 1, *a) + 1 if n else 0


class Deep:
    def __init__(self, n):
        self.n = n

    @property
    def down(self):
        if not self.n:
            return 0
        self.n -= 1
        return self.down + 1


class DeepGetattribute:
    def __init__(self, n):
        self.n = n

    def __getattribute__(self, name):
        n = object.__getattribute__(self, 'n')
        if not n:
            return 0
        self.n = n - 1
        return self.down + 1


def deep_calls_through_cpython():
    sys.setrecursionlimit(10 ** 6 + 100)
    return [vkdown(3 * 10 ** 5, k=1), vadown(3 * 10 ** 5, 1),
            Deep(3 * 10 ** 5).down, DeepGetattribute(3 * 10 ** 5).down]


# a bound method's f(*a, **kw) is a call through C in CPython too
class Bound:
    def down(self, n, **kw):
        return self.down(n - 1, **kw) + 1 if n else 0

    def star(self, n):
        return self.star(*(n - 1,)) + 1 if n else 0


def through_c_still_limited():
    sys.setrecursionlimit(10 ** 6 + 100)
    out = []
    for call in (lambda: Bound().down(3 * 10 ** 5, k=1),
                 lambda: Bound().down(3 * 10 ** 5),
                 lambda: Bound().star(3 * 10 ** 5)):
        try:
            out.append(call())
        except RecursionError:
            out.append('RecursionError')
    return out


def limit_raised_then_reached():
    sys.setrecursionlimit(3000)
    out = []
    for f in down, vkdown, vadown:
        try:
            f(5000)
        except RecursionError as e:
            out += [str(e), f(2000)]
    return out


def nest(k):
    if k:
        for x in nest(k - 1):
            yield x + 1
    else:
        yield 0


def generator_for_loops():
    sys.setrecursionlimit(10 ** 6 + 100)
    return [list(nest(10 ** 6))]


def one(n):
    yield n


# a generator that only compiled code sees, stepped at every depth
def down_stepping(n):
    for m in one(n):
        return down_stepping(m - 1) + 1 if m else 0


def stepped_generators():
    sys.setrecursionlimit(10 ** 6 + 100)
    return [down_stepping(10 ** 6)]


def in_threads(target, n, stack_size=256 * 1024):
    out = [None] * n

    def run(i):
        out[i] = target(i)
    threading.stack_size(stack_size)
    ts = [threading.Thread(target=run, args=(i,)) for i in range(n)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return out


def small_thread_stacks():
    sys.setrecursionlimit(10 ** 6 + 100)
    return in_threads(lambda i: down(10 ** 5 + i), 4)


def raise_down(n):
    if not n:
        raise ValueError('bottom')
    return raise_down(n - 1) + 1


def traceback_from_deep():
    import traceback

    def run(i):
        try:
            raise_down(50000)
        except ValueError as e:
            tb = traceback.extract_tb(e.__traceback__)
            return [len(tb)] + [(f.name, f.lineno) for f in tb[:3] + tb[-3:]]
    sys.setrecursionlimit(10 ** 5)
    return in_threads(run, 1, 1024 * 1024)


def locals_down(n, seen):
    k = n * 3
    if n:
        return locals_down(n - 1, seen) + k
    f = sys._getframe()
    while f is not None:
        if f.f_code is locals_down.__code__:
            seen.append((f.f_locals['n'], f.f_locals['k'], f.f_lineno))
        f = f.f_back
    return 0


# every caller's locals, those where a run of compiled code goes on
# on a segment too
def caller_locals_across_segments():
    def run(i):
        seen = []
        r = locals_down(20000, seen)
        return r, len(seen), hash(tuple(seen))
    sys.setrecursionlimit(10 ** 5)
    return in_threads(run, 1)


class Link:
    def __init__(self, next):
        self.next = next


# objects nested n deep, each kind freed by compiled code: through a
# variable of a type known when compiling ('top') and of one that isn't
# ('x'). CPython frees each level from the one above it, deferring the
# deepest to the trashcan, so the C stack never runs out.
def free_nested(n):
    out = []
    for make in (lambda x: [x], lambda x: (x,), lambda x: {'k': x}, Link,
                 lambda x: [(x, {'k': Link(x)})]):
        x = None
        for i in range(n):
            x = make(x)
        top = [x]
        x = None
        top = None
        for i in range(n):
            x = make(x)
        x = None
        out.append(top is None)
    return out


def deep_frees():
    return [free_nested(10 ** 6), free_nested(10 ** 5)]


def free_nested_down(n, depth):
    return free_nested_down(n, depth - 1) if depth else free_nested(n)


# where the C stack is small or already deep
def deep_frees_short_of_stack():
    sys.setrecursionlimit(10 ** 5)
    return (in_threads(lambda i: free_nested(10 ** 5), 2) +
            [free_nested_down(10 ** 5, 20000)])


def free_lists_down(n, depth):
    if depth:
        return free_lists_down(n, depth - 1)
    x = Link(None)
    deepest = weakref.ref(x)
    for i in range(n):
        x = [x]
    top = [x]
    x = None
    top = None
    return deepest() is None


# from every depth of a small stack, so that some free is just above where
# CPython defers. Where the trashcan is emptied by the free that deferred
# to it, the deepest object is gone when that free returns, except where
# compiled calls have taken so much C stack that the free itself defers:
# from some depth down, never above it.
def frees_at_every_depth():
    def at_every_depth(i):
        freed = [free_lists_down(2000, depth) for depth in range(6000)]
        return freed == sorted(freed, reverse=True)
    sys.setrecursionlimit(10 ** 5)
    return in_threads(at_every_depth, 1)


CASES = [deep_returns, deep_keyword_calls, deep_calls_through_cpython,
         through_c_still_limited, limit_raised_then_reached, generator_for_loops,
         stepped_generators,
         small_thread_stacks, traceback_from_deep,
         caller_locals_across_segments, deep_frees, deep_frees_short_of_stack,
         frees_at_every_depth]


def child(name, lejit_mode):
    sys.path.insert(0, ROOT)
    if lejit_mode == '1':
        import lejit
        lejit.start()
    try:
        print(repr(globals()[name]()))
    except Exception as e:
        print('exc %s: %s' % (type(e).__name__, e))


def run(name, lejit_mode):
    try:
        p = subprocess.run([sys.executable, os.path.abspath(__file__),
                            '--child', name, lejit_mode],
                           capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, 'timeout after %ds' % TIMEOUT
    if p.returncode != 0:
        return None, 'exit %d: %s' % (p.returncode, p.stderr[-2000:])
    return p.stdout, p.stderr


def main():
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], sys.argv[3])
        return
    k = sys.argv[sys.argv.index('-k') + 1] if '-k' in sys.argv else ''
    failed = 0
    for case in CASES:
        name = case.__name__
        if k not in name:
            continue
        want, err = run(name, '0')
        if want is None:
            print('%s: skipped, plain CPython fails it (%s)'
                  % (name, err.splitlines()[0] if err else ''))
            continue
        got, err = run(name, '1')
        if got != want:
            failed += 1
            print('%s: FAIL\n  plain: %s\n  lejit: %s\n%s'
                  % (name, want[:500], (got or '')[:500], err))
        else:
            print('%s: ok' % name)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
