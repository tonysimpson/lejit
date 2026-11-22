# Loops whose body needs a respawn (here: the index-is-negative guard) and
# carries a variable that changes between iterations; the -1 check after
# PyLong_AsLong(-1) must leave the stack as it was.

def neg_index_loop(c):
    k = 0
    while c:
        k = len(c[-1])
        del c[-1]
    return k

def neg_index_del(c):
    k = 0
    while c:
        x = c[-1]
        k = len(x)
        del c[-1]
    return k

def runtime_neg_index(c, i):
    k = 0
    while c:
        k = len(c[i])
        del c[i]
    return k

def pos_index_loop(c):
    k = 0
    while c:
        k = len(c[len(c) - 1])
        del c[-1]
    return k

def straight_line(c):
    k = len(c[-1])
    del c[-1]
    k = len(c[-1])
    del c[-1]
    return k, c


# At the loop head x and y are distinct the first time round and one value
# afterwards: psyco_unify must put that value in two registers.
def shared_after_loop(n):
    x = n * 3
    y = n * 5
    s = 0
    for i in range(n):
        s += x * 7 + y
        x = y
    return s, x, y

# The same, with the sharing set up on alternate iterations.
def alternating_share(n):
    a = n + 1
    b = n + 2
    i = 0
    while i < n:
        i += 1
        if i & 1:
            b = a
        else:
            a = a + b
    return a, b

CASES = [
    ('neg_index_loop', "[(['a'],)]", {}),
    ('neg_index_loop', "[(['a', 'bb'],)]", {}),
    ('neg_index_del', "[(['a', 'bb'],)]", {}),
    ('runtime_neg_index', "[(['a', 'bb'], -1)]", {}),
    ('pos_index_loop', "[(['a', 'bb', 'c'],)]", {}),
    ('straight_line', "[(['a', 'bb', 'ccc'],)]", {}),
    ('shared_after_loop', "[(1,), (2,), (6,)]", {}),
    ('alternating_share', "[(1,), (2,), (3,), (6,)]", {}),
]
