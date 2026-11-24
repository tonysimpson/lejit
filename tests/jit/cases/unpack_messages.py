# The errors of a, b, c = x: too many values from a list, tuple or dict
# (whose size the message may give), or from another iterable; not enough.


def msg(f, *a):
    try:
        return ('ok', f(*a))
    except Exception as e:
        return (type(e).__name__, str(e))


def three(x):
    a, b, c = x
    return [a, b, c]


def starred(x):
    a, *b, c, d = x
    return [a, b, c, d]


def known(n):
    items = list(range(n))
    a, b, c = items
    return [a, b, c]


def errors(n):
    values = [list(range(n)), tuple(range(n)), dict.fromkeys(range(n)),
              set(range(n)), 'xyzw'[:n], range(n), iter(list(range(n)))]
    return [msg(three, v) for v in values] + [msg(starred, v) for v in values]


CASES = [
    ('errors', [(2,), (3,), (4,), (5,)], {}),
    ('known', [(3,), (4,), (2,)], {}),
]
