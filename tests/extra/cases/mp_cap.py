# Eight counters, each still the constant it started as or run-time
# depending on the paths taken so far: at the loop head every combination
# is a different state, so without a cap the loop is compiled again for
# each one (vcompiler.c:mp_capped). Past the cap the
# known values are forgotten, and the results must not change.

def counters8(items):
    a = b = c = d = 0
    e = f = g = h = 1
    for x in items:
        if x == 1:
            a += x
        elif x == 2:
            b += x
        elif x == 3:
            c += x
        elif x == 4:
            d += x
        elif x == 5:
            e *= x
        elif x == 6:
            f -= x
        elif x == 7:
            g = x
        elif x == 8:
            h = -x
        else:
            a, h = h, a
    return a, b, c, d, e, f, g, h, (a == 0) + (h == 1) * 2


def orders(n):
    # every pair of paths, in both orders, then all of them
    out = []
    for i in range(1, 10):
        for j in range(1, 10):
            out.append(counters8([i, j] * n))
    out.append(counters8(list(range(10)) * n))
    return out


def flags_and_counts(rows):
    # bools and small ints mixed, compared after the loop
    neg = big = odd = False
    n0 = n1 = n2 = 0
    for r in rows:
        if r < 0:
            neg = True
            n0 += 1
        elif r > 100:
            big = True
            n1 += r
        elif r & 1:
            odd = not odd
            n2 -= 1
    return neg, big, odd, n0, n1, n2, neg is True, n0 + n1 + n2


CASES = [
    ('counters8', [([],), ([1],), ([9],), ([1, 2, 3, 4, 5, 6, 7, 8, 9],),
                   ([8, 8, 9, 5, 5, 6],), ([7, 1, 9, 9, 2],)], {}),
    ('orders', [(1,), (2,), (3,)], {}),
    ('flags_and_counts', [([],), ([1, -1, 200],), ([3, 5, 7, -2, 150, 4],),
                          ([-1] * 5 + [101] * 3,)], {}),
]
