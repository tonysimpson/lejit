# A constant index whose scaled offset doesn't fit a 32-bit displacement
# (vcompiler.c:psyco_get_field_array, x64/v_mem_mov.h). Only in bounds for
# arrays of 2 GB, so this file makes one list of 2**28 + 1 items (2 GB,
# about a second) once per process.

N = 2**28
BIG = None


def big():
    global BIG
    if BIG is None:
        BIG = [0] * (N + 1)
        BIG[N] = 42
        BIG[0] = -1
    return BIG


def get_far(l):
    return l[2**28], l[2**28 - 1], l[0]


def put_far(l):
    l[2**28] = 7
    r = l[2**28]
    l[2**28] = 42
    return r, l[0]


CASES = [
    ('get_far', '[(big(),)]', {}),
    ('put_far', '[(big(),)]', {}),
]
