# Struct members read by compiled code. Today attribute lookup is a
# residual call, so this checks nothing in ni/Objects/pstructmember.c yet;
# it will once lookups are specialised. In
# 3.7's code objects co_argcount and co_kwonlyargcount are adjacent C
# ints (T_INT): an 8-byte read of the first gave argcount + (kwonly << 32)
# (KNOWN_BUGS M10).


def with_kwonly(a, b, *, c, d, e):
    pass


def neg_lineno():
    pass


def code_ints(f):
    co = f.__code__
    return co.co_argcount, co.co_kwonlyargcount, co.co_nlocals, co.co_flags


def code_ints_loop(f, n):
    t = 0
    co = f.__code__
    for i in range(n):
        t += co.co_argcount + co.co_kwonlyargcount
    return t


CASES = [
    ('code_ints', "[(with_kwonly,), (neg_lineno,)]", {}),
    ('code_ints_loop', "[(with_kwonly, 10)]", {}),
]
