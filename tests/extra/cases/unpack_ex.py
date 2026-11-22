# Starred assignment targets (UNPACK_EX): every shape, every source kind,
# and ceval's unpack_iterable messages.


def gen(n):
    for i in range(n):
        yield i


def explode(n):
    for i in range(n):
        yield i
    raise KeyError('from the generator')


def star_last(x):
    a, *b = x
    return a, b


def star_first(x):
    *a, b = x
    return a, b


def star_middle(x):
    a, *b, c = x
    return a, b, c


def star_only(x):
    *a, = x
    return a


def star_many(x):
    a, b, *c, d, e, f = x
    return a, b, c, d, e, f


def nested_star(x):
    (a, *b), *c = x
    return a, b, c


def in_for(rows):
    out = []
    for first, *rest in rows:
        out.append((first, len(rest)))
    return out


def loop(n):
    t = 0
    for i in range(n):
        a, *b, c = (i, i + 1, i + 2, i + 3)
        t += a + len(b) + c
    return t


def star_type(x):
    *a, b = x
    a.append('new')
    return type(a).__name__, a


def long_before(x):
    # ten targets before the star (a target after it needs EXTENDED_ARG,
    # as in star_middle)
    (a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, *rest) = x
    return a0, a9, len(rest)


def err_not_enough(x):
    a, *b, c = x
    return a, b, c


def err_not_enough_after(x):
    a, *b, c, d, e = x
    return a, b, c, d, e


def err_not_iterable(x):
    a, *b = x
    return a, b


def err_generator(n):
    a, *b, c = explode(n)
    return a, b, c


CASES = [
    ('star_last', "[((1, 2, 3),), ([1],), ('xyz',), (range(4),), (gen(3),), "
                  "({'k': 1, 'j': 2},), (iter([5, 6]),)]", {}),
    ('star_first', "[((1, 2, 3),), ([1],), ('ab',), (gen(2),)]", {}),
    ('star_middle', "[((1, 2, 3, 4),), ((1, 2),), ([1, 2, 3],), ('abc',), "
                    "(gen(5),)]", {}),
    ('star_only', "[((),), ((1, 2),), (gen(3),)]", {}),
    ('star_many', "[(range(5),), (range(9),), (list(range(6)),)]", {}),
    ('nested_star', "[(([1, 2, 3], 4, 5),), (('ab',),)]", {}),
    ('in_for', "[([(1, 2, 3), [4], 'xyz'],)]", {}),
    ('loop', [(100,)], {}),
    ('star_type', "[((1, 2),), ('ab',)]", {}),
    ('long_before', "[(range(10),), (range(300),)]", {}),
    ('err_not_enough', "[((),), ((1,),), ([1],), ('a',), (gen(1),), "
                       "(iter([]),)]", {}),
    ('err_not_enough_after', "[((1, 2),), ((1, 2, 3),), (gen(3),), "
                             "([1, 2, 3, 4],)]", {}),
    ('err_not_iterable', "[(5,), (None,), (1.5,)]", {}),
    ('err_generator', [(0,), (1,), (5,)], {}),
]
