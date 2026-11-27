"""Deep Python recursion: where CPython's Python calls take no C stack, it
returns at any depth the recursion limit allows, and LeJIT's compiled
calls must too, by moving to a stack segment when the stack runs low.
Each case runs in a fresh process, plain and with LeJIT, and the output
must match. A case plain CPython can't run (its calls take C stack, and
it overflows) is skipped.

    python3.X tests/jit/deep_stacks.py [-k substring]
"""
import os, subprocess, sys, threading

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


def through_c_still_limited():
    sys.setrecursionlimit(10 ** 6 + 100)
    try:
        return Bound().down(3 * 10 ** 5, k=1)
    except RecursionError:
        return 'RecursionError'


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


CASES = [deep_returns, deep_keyword_calls, deep_calls_through_cpython,
         through_c_still_limited, limit_raised_then_reached, generator_for_loops,
         stepped_generators,
         small_thread_stacks, traceback_from_deep,
         caller_locals_across_segments]


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
