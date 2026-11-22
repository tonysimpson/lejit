# A local holding a large int constant (outside -5..256), a small one and
# a run-time one at the same merge point (deltablue's change(v, 17 / 1050 /
# 5 / 2000)). From the second block on, the large constant is made a
# run-time object (dispatcher.c:psyco_unpromote_large_ints): the same
# object, so `is` against the constant must still hold, as in CPython.

BIG = 1050


def change(v, new_value):
    out = []
    for i in range(v):
        out.append(new_value + i)
    return out, new_value is BIG, new_value is 1050


def chain():
    r = []
    r.append(change(3, 17))
    r.append(change(3, BIG))
    r.append(change(3, 5))
    r.append(change(3, 2000))
    r.append(change(2, BIG))
    r.append(change(2, 10 ** 30))
    return r


def loop_identity(n):
    # a large constant in a loop-carried local, then replaced by run-time
    # values: identity with the constant must survive the merge point
    x = 100000
    k = 7
    same = 0
    for i in range(n):
        if x is 100000:
            same += 1
        if i == 2:
            x = x + i       # a new object: no longer the constant
        if i == 4:
            x = 100000
        k += 1
    return same, x, k


def mixed(seq):
    # small, large and run-time ints in one local at a loop head
    acc = []
    last = 300
    for s in seq:
        if s < 0:
            last = 300
        elif s == 0:
            last = 7
        else:
            last = s * 1000
        acc.append((last, last is 300))
    return acc


CASES = [
    ('change', [(3, 17), (3, 1050), (3, 5), (3, 2000), (0, 2 ** 70)], {}),
    ('chain', [()], {}),
    ('loop_identity', [(0,), (1,), (3,), (6,), (10,)], {}),
    ('mixed', [([],), ([1, -1, 0, 2, -1],), ([0, 0, -1, 5, 9, -1, -1],)], {}),
]
