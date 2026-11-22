# BUILD_TUPLE_UNPACK and BUILD_TUPLE_UNPACK_WITH_CALL, generated from
# ceval.c's BUILD_LIST_UNPACK case they fall into, with the policy's
# virtual concatenation of known tuples (P22). Parts: tuples (known and
# not), lists, generators, strings and non-iterables, with CPython's
# messages.


def msg(f, *a, **k):
    try:
        return ('ok', f(*a, **k))
    except Exception as e:
        return (type(e).__name__, str(e))


def f(*args):
    return args


def known(x, y):
    a = (x, y)
    b = (1, 2, 3)
    return [(*a, *b), (*b,), f(*a, *b), f(0, *a, *b), (*a, *(), *b),
            f(*a, *(x,))[1:]]


def unknown(a, b):
    def gen():
        yield 'g'
        yield 'h'
    return [(*a, *b), f(*a, *b), f(*a, *gen()), (*gen(), *a), f(*'xy', *b),
            (*a, *[7, 8]), f(*[], *b)]


def bad(a, b):
    def raising():
        yield 1
        raise ValueError('in gen')

    def typeerr():
        yield 1
        raise TypeError('from gen')
    out = []
    out.append(msg(lambda: (*a, *b)))
    out.append(msg(lambda: f(*a, *b)))
    out.append(msg(lambda: f(*a, *raising())))
    out.append(msg(lambda: f(*a, *typeerr())))
    out.append(msg(lambda: (*a, *typeerr())))
    out.append(msg(lambda: len(*a, *b)))
    return out


CASES = [
    ('known', [(1, 2), ('p', None)], {}),
    ('unknown', [((1,), (2, 3)), ([1], (2,)), ((), [])], {}),
    ('bad', [((1,), 5), ((), None), ((1,), (2,)), (5, ())], {}),
]
