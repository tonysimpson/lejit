# Counters that start as the constant 0 and are updated only on some paths
# through a loop, as in go's Board.useful: at the loop head each counter is
# a constant int object, a virtual int with a known value or one with a
# run-time value (CODE_SIZE.md option 2, dispatcher.c:canonical_int).

def counters(items):
    a = b = c = d = e = 0
    for x in items:
        if x == 1:
            a += 1
        elif x == 2:
            b += 1
            if c:
                d += 1
        elif x == 3:
            c -= 1
        else:
            e += x
    return a, b, c, d, e, (a == 0) + (b == 0) * 2 + (c == 0) * 4


def flags(items):
    seen_neg = False
    seen_big = False
    total = 0
    for x in items:
        if x < 0:
            seen_neg = True
        elif x > 100:
            seen_big = True
            total += x
    return seen_neg, seen_big, total, seen_neg is True, seen_big is False


def escapes(items):
    # the counter is passed out while it may still be the constant
    n = 0
    out = []
    for x in items:
        out.append(n)
        if x:
            n += 1
    return out, [v is out[0] for v in out]


def leaves_small_range(k):
    # past the small int cache (256) and below -5
    up = 0
    down = 0
    for i in range(k):
        if i % 3:
            up += 7
        else:
            down -= 2
    return up, down


def nested_branches(n):
    hits = misses = 0
    for i in range(n):
        for j in range(3):
            if (i + j) % 4 == 0:
                hits += 1
            elif i == j:
                misses -= 1
    return hits, misses


CASES = [
    ('counters', [([],), ([1],), ([2, 2],), ([3, 2],), ([1, 2, 3, 4],),
                  ([5, 3, 3, 2, 1, 1, 9],)], {}),
    ('flags', [([],), ([1, 2],), ([-1],), ([200, -3, 150],)], {}),
    ('escapes', [([],), ([0, 0],), ([1, 0, 1, 1],)], {}),
    ('leaves_small_range', [(0,), (1,), (5,), (200,)], {}),
    ('nested_branches', [(0,), (1,), (7,), (40,)], {}),
]
