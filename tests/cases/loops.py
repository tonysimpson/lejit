# Loops whose body needs a respawn (here: the index-is-negative guard) and
# carries a variable that changes between iterations (KNOWN_BUGS C8).

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

C8 = {'xfail': 'C8: loop + respawn + loop-carried variable reads a stale list length'}
CASES = [
    ('neg_index_loop', "[(['a'],)]", {}),
    ('neg_index_loop', "[(['a', 'bb'],)]", C8),
    ('neg_index_del', "[(['a', 'bb'],)]", C8),
    ('runtime_neg_index', "[(['a', 'bb'], -1)]", C8),
    ('pos_index_loop', "[(['a', 'bb', 'c'],)]", {}),
    ('straight_line', "[(['a', 'bb', 'ccc'],)]", {}),
]
