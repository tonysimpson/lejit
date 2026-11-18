"""Ni against plain CPython 3.7: each benchmark in a fresh process per mode,
best of three runs, results checked equal.

    python3.7 tools/bench.py            # table
    python3.7 tools/bench.py -k mandel  # only names containing 'mandel'

Use a release build (rm ni*.so; python3.7 setup.py build_ext --inplace);
the build left by tools/check.sh is a debug build.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---- integers --------------------------------------------------------------

def count_primes(n):
    """Trial division (int %, comparisons, nested loops)."""
    count = 0
    for i in range(2, n):
        j = 2
        prime = True
        while j * j <= i:
            if i % j == 0:
                prime = False
                break
            j = j + 1
        if prime:
            count = count + 1
    return count


def sieve(n):
    """Sieve of Eratosthenes (list stores, int loops)."""
    flags = [True] * n
    count = 0
    for i in range(2, n):
        if flags[i]:
            count = count + 1
            k = i + i
            while k < n:
                flags[k] = False
                k = k + i
    return count


def collatz(n):
    """Longest Collatz chain below n (// and %, while loops)."""
    best = 0
    for start in range(1, n):
        x = start
        steps = 0
        while x != 1:
            if x % 2 == 0:
                x = x // 2
            else:
                x = 3 * x + 1
            steps = steps + 1
        if steps > best:
            best = steps
    return best


def digit_sums(n):
    """Sum of decimal digits of 1..n (divmod)."""
    t = 0
    for i in range(1, n):
        x = i
        while x:
            x, d = divmod(x, 10)
            t = t + d
    return t


# ---- floats ----------------------------------------------------------------

def mandel(w, h, iters):
    """Mandelbrot escape counts (float arithmetic)."""
    total = 0
    for py in range(h):
        y0 = py * 2.0 / h - 1.0
        for px in range(w):
            x0 = px * 3.0 / w - 2.0
            x = 0.0
            y = 0.0
            i = 0
            while i < iters and x * x + y * y < 4.0:
                x, y = x * x - y * y + x0, 2.0 * x * y + y0
                i = i + 1
            total = total + i
    return total


def quantise(n):
    """Pixel maths: float to int and back (int(), float(), round())."""
    t = 0
    for i in range(n):
        v = i * 0.37
        r = int(v * 255.0 / (v + 1.0))
        g = round(v % 7.5)
        b = int(float(i % 1000) / 3.9)
        t = t + r + g + b
    return t


def stats(n):
    """Running min, max, sums and rounding (min, max, sum, round, abs)."""
    lo = 1e300
    hi = -1e300
    acc = 0.0
    for i in range(n):
        a = (i * 7919) % 1000 * 0.01
        b = abs(a - 5.0)
        c = a * b % 3.0
        lo = min(lo, a, b, c)
        hi = max(hi, a, b, c)
        acc = acc + sum((a, b, c))
    return round(lo, 3), round(hi, 3), round(acc)


# ---- objects and builtins --------------------------------------------------

class Circle:
    def __init__(self, r):
        self.r = r


class Square:
    def __init__(self, s):
        self.s = s


def shapes(n):
    """Dispatch on isinstance over mixed objects (ints, floats, strs, classes)."""
    items = [3, 2.5, 'abc', Circle(1.0), Square(2.0), 7, b'xy', None] * 100
    t = 0.0
    for k in range(n):
        for x in items:
            if isinstance(x, int):
                t = t + x
            elif isinstance(x, float):
                t = t + x * 2.0
            elif isinstance(x, (str, bytes)):
                t = t + len(x)
            elif isinstance(x, Circle):
                t = t + 3.0 * x.r
            elif isinstance(x, Square):
                t = t + x.s
    return t


def shapes_locals(n):
    """shapes with the builtins and classes copied into locals: no global
    lookups in the loop (each LOAD_GLOBAL is a run-time dict lookup)."""
    isi, I, F, S, B, C, Q = isinstance, int, float, str, bytes, Circle, Square
    L = len
    items = [3, 2.5, 'abc', Circle(1.0), Square(2.0), 7, b'xy', None] * 100
    t = 0.0
    for k in range(n):
        for x in items:
            if isi(x, I):
                t = t + x
            elif isi(x, F):
                t = t + x * 2.0
            elif isi(x, (S, B)):
                t = t + L(x)
            elif isi(x, C):
                t = t + 3.0 * x.r
            elif isi(x, Q):
                t = t + x.s
    return t


def hashing(n):
    """hash() of ints and floats, buckets by %."""
    buckets = [0] * 64
    for i in range(n):
        buckets[hash(i * 31) % 64] += 1
        buckets[hash(i * 0.5) % 64] += 1
    return max(buckets), min(buckets)


# ---- from tools/demo.py ----------------------------------------------------

def place(n, row, cols, d1, d2):
    if row == n:
        return 1
    count = 0
    for c in range(n):
        if not cols[c] and not d1[row + c] and not d2[row - c + n]:
            cols[c] = True; d1[row + c] = True; d2[row - c + n] = True
            count += place(n, row + 1, cols, d1, d2)
            cols[c] = False; d1[row + c] = False; d2[row - c + n] = False
    return count


def nqueens(n):
    """n-queens (recursion, lists of bools)."""
    return place(n, 0, [False] * n, [False] * (2 * n), [False] * (2 * n + 1))


def nbody(steps):
    """Sun and four planets, Euler steps (floats, nested lists)."""
    bodies = [[0.0, 0.0, 0.0, 0.0, 39.47],
              [4.84, -1.16, 0.606, 2.81, 0.0377],
              [8.34, 4.12, -1.01, 1.82, 0.0113],
              [12.89, -15.11, 1.08, 0.869, 0.00172],
              [15.38, -25.92, 0.979, 0.594, 0.00203]]
    dt = 0.001
    for s in range(steps):
        for i in range(5):
            bi = bodies[i]
            for j in range(i + 1, 5):
                bj = bodies[j]
                dx = bi[0] - bj[0]
                dy = bi[1] - bj[1]
                d2 = dx * dx + dy * dy + 0.01
                mag = dt / (d2 * d2 ** 0.5)
                bi[2] -= dx * bj[4] * mag
                bi[3] -= dy * bj[4] * mag
                bj[2] += dx * bi[4] * mag
                bj[3] += dy * bi[4] * mag
        for b in bodies:
            b[0] += dt * b[2]
            b[1] += dt * b[3]
    return round(bodies[1][0], 6)


def by_count(kv):
    return -kv[1]


TEXT = ('It was the best of times, it was the worst of times, it was the age '
        'of wisdom, it was the age of foolishness, it was the epoch of belief.')


def word_counts(repeat):
    """Count words (str methods, dicts, sorted with a key)."""
    counts = {}
    for r in range(repeat):
        for word in TEXT.split():
            word = word.strip('.,;').lower()
            counts[word] = counts.get(word, 0) + 1
    return sorted(counts.items(), key=by_count)[:3]


# ---- targets: range, comprehensions, strings, generators, instances -------

def range_loops(n):
    """Nested range loops with steps (virtual range)."""
    t = 0
    for i in range(n):
        for j in range(0, 100, 3):
            t = t + (i ^ j)
    return t


def comprehensions(n):
    """List comprehensions and sum over them (listcomp calls)."""
    t = 0
    for k in range(n):
        xs = [i * i for i in range(50)]
        ys = [x + 1 for x in xs if x % 3]
        t = t + len(ys) + xs[-1]
    return t


def short_strings(n):
    """Short string building, comparison and indexing."""
    t = 0
    for i in range(n):
        s = 'ab' + str(i % 10)
        if s == 'ab3':
            t = t + 1
        t = t + len(s) + (s[0] == 'a')
    return t


def char_scan(repeat):
    """Character loop over a str (str iteration, == on one-char strings)."""
    text = TEXT * 4
    vowels = 0
    for r in range(repeat):
        for ch in text:
            if ch == 'a' or ch == 'e' or ch == 'i' or ch == 'o' or ch == 'u':
                vowels = vowels + 1
    return vowels


def gen_squares(n):
    for i in range(n):
        yield i * i


def generators(n):
    """A generator and a generator expression."""
    t = 0
    for k in range(n):
        t = t + sum(gen_squares(30)) + sum(x for x in range(30) if x & 1)
    return t


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y


def points(n):
    """Instances: creation and attribute access."""
    t = 0.0
    p = Point(1.0, 2.0)
    for i in range(n):
        q = Point(p.y, p.x + 0.5)
        t = t + q.x * q.y
        p = q
    return round(t, 3)


BENCHMARKS = [
    ('count_primes', count_primes, (60000,)),
    ('sieve', sieve, (1000000,)),
    ('collatz', collatz, (60000,)),
    ('digit_sums', digit_sums, (300000,)),
    ('mandel', mandel, (240, 120, 300)),
    ('quantise', quantise, (500000,)),
    ('stats', stats, (300000,)),
    ('shapes', shapes, (500,)),
    ('shapes_locals', shapes_locals, (500,)),
    ('hashing', hashing, (300000,)),
    ('nqueens', nqueens, (10,)),
    ('nbody', nbody, (100000,)),
    ('word_counts', word_counts, (20000,)),
    ('range_loops', range_loops, (20000,)),
    ('comprehensions', comprehensions, (20000,)),
    ('short_strings', short_strings, (300000,)),
    ('char_scan', char_scan, (3000,)),
    ('generators', generators, (20000,)),
    ('points', points, (300000,)),
]


def child(mode, name):
    if mode == 'ni':
        sys.path.insert(0, ROOT)
        import ni
    fn, args = {b[0]: b[1:] for b in BENCHMARKS}[name]
    best = None
    for k in range(3):
        t = time.perf_counter()
        r = fn(*args)
        dt = time.perf_counter() - t
        best = dt if best is None else min(best, dt)
    compiled = ''
    if mode == 'ni':
        compiled = ni._code_info(fn.__code__)
        if ni._debug_build:
            compiled += ' (debug build)'
    print(repr((best, repr(r), compiled)))


def main():
    names = [b[0] for b in BENCHMARKS]
    if '-k' in sys.argv:
        names = [n for n in names if sys.argv[sys.argv.index('-k') + 1] in n]
    print('%-14s %9s %9s %7s  %s' % ('', 'CPython', 'Ni', 'speed-up', ''))
    tp = tn = 0.0
    for name in names:
        res = {}
        for mode in ('plain', 'ni'):
            out = subprocess.run([sys.executable, __file__, '--child', mode, name],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 universal_newlines=True)
            if out.returncode:
                res[mode] = None
                print('%s %s failed: %s' % (name, mode, out.stderr[-300:]))
            else:
                res[mode] = eval(out.stdout.strip().splitlines()[-1])
        if None in res.values():
            continue
        (p, rp, _), (n, rn, info) = res['plain'], res['ni']
        tp += p
        tn += n
        note = info if info != 'compiled' else ''
        if rp != rn:
            note += ' RESULTS DIFFER: %s vs %s' % (rp, rn)
        print('%-14s %8.3fs %8.3fs %7.1fx  %s' % (name, p, n, p / n, note))
    if tn:
        print('%-14s %8.3fs %8.3fs %7.1fx' % ('total', tp, tn, tp / tn))
    else:
        print('(no benchmark matches)')


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], sys.argv[3])
    else:
        main()
