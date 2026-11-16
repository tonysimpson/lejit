"""Ni demo: run with and without the JIT and compare.

    python3.7 tools/demo.py ni
    python3.7 tools/demo.py plain
"""
import sys, time
USE_NI = len(sys.argv) > 1 and sys.argv[1] == 'ni'
if USE_NI:
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
    import ni
    if ni._debug_build:
        print('note: ni.so is a debug build (left by tools/check.sh), about 10% '
              'slower;\n      for timings: rm ni*.so; python3.7 setup.py build_ext --inplace\n')


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
    """Count the solutions to the n-queens puzzle (recursion, lists)."""
    return place(n, 0, [False] * n, [False] * (2 * n), [False] * (2 * n + 1))


def nbody(steps):
    """Sun and four planets, simple Euler steps (floats, nested lists)."""
    bodies = [[0.0, 0.0, 0.0, 0.0, 39.47],
              [4.84, -1.16, 0.606, 2.81, 0.0377],
              [8.34, 4.12, -1.01, 1.82, 0.0113],
              [12.89, -15.11, 1.08, 0.869, 0.00172],
              [15.38, -25.92, 0.979, 0.594, 0.00203]]
    dt = 0.001
    n = len(bodies)
    for s in range(steps):
        for i in range(n):
            bi = bodies[i]
            for j in range(i + 1, n):
                bj = bodies[j]
                dx = bi[0] - bj[0]
                dy = bi[1] - bj[1]
                d2 = dx * dx + dy * dy
                mag = dt / (d2 * d2 ** 0.5)
                bi[2] -= dx * bj[4] * mag
                bi[3] -= dy * bj[4] * mag
                bj[2] += dx * bi[4] * mag
                bj[3] += dy * bi[4] * mag
        for b in bodies:
            b[0] += dt * b[2]
            b[1] += dt * b[3]
    e = 0.0
    for b in bodies:
        e += 0.5 * b[4] * (b[2] * b[2] + b[3] * b[3])
    return round(e, 6)


def mandel_art(w, h, iters):
    """An ASCII Mandelbrot set, returned as a list of strings."""
    chars = ' .:-=+*#%@'
    rows = []
    for py in range(h):
        y0 = py * 2.4 / h - 1.2
        line = []
        for px in range(w):
            x0 = px * 3.2 / w - 2.2
            x = 0.0
            y = 0.0
            i = 0
            while i < iters and x * x + y * y < 4.0:
                t = x * x - y * y + x0
                y = 2.0 * x * y + y0
                x = t
                i += 1
            line.append(chars[i * (len(chars) - 1) // iters])
        rows.append(''.join(line))
    return rows


def word_counts(text, repeat):
    """Count words in a text (str methods, dicts)."""
    counts = {}
    for r in range(repeat):
        for word in text.split():
            word = word.strip('.,;').lower()
            counts[word] = counts.get(word, 0) + 1
    best = sorted(counts.items(), key=by_count)
    return best[:3]


def by_count(kv):
    return -kv[1]


TEXT = ('It was the best of times, it was the worst of times, it was the age '
        'of wisdom, it was the age of foolishness, it was the epoch of belief.')

total = 0.0
for name, f, args in [('nqueens(10)', nqueens, (10,)),
                      ('nbody', nbody, (200000,)),
                      ('mandel_art', mandel_art, (72, 28, 5000)),
                      ('word_counts', word_counts, (TEXT, 20000))]:
    t = time.perf_counter()
    r = f(*args)
    dt = time.perf_counter() - t
    total += dt
    compiled = USE_NI and ni._code_info(f.__code__) == 'compiled'
    shown = 'picture below' if name == 'mandel_art' else r
    print('%-12s %-42s %7.3fs  %s' % (name, shown, dt, 'compiled' if compiled else ''))
    if name == 'mandel_art':
        art = r
print('%-12s %-42s %7.3fs' % ('total', '', total))
print('\n'.join(art))
