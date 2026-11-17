# str == and != (Objects/punicodeobject.c): two constants fold at compile
# time, so a test on a cached global is decided while compiling; against a
# run-time str it is a C call giving a known bool. Subclasses compare as str
# unless they override; anything else is NotImplemented.
MODE = 'fast'
NAMES = ('fast', 'slow', 'fa' + 'st', ''.join(['fa', 'st']), 'é', '😀', '')


class S(str):
    pass


class Loud(str):
    def __eq__(self, other):
        return 'loud'

    __hash__ = str.__hash__


def constant_test(n):
    t = 0
    for i in range(n):
        if MODE == 'fast':
            t += 1
        if MODE != 'fast':
            t -= 100
    return t


def runtime_eq(xs):
    return [(a == b, a != b, b == a) for a in xs for b in NAMES]


def ordering(xs):
    return [(a < b, a <= b, a > b, a >= b) for a in xs for b in NAMES]


def subclasses(x):
    s = S(x)
    return (s == x, x == s, s != x, S('a') == S('a'), Loud(x) == x,
            x == Loud(x), Loud(x) != x)


def mixed(x):
    out = [x == 1, 1 == x, x != None, x == b'fast', x == ('fast',)]
    try:
        x < 1
    except TypeError as e:
        out.append(str(e))
    return out


CASES = [
    ('constant_test', [(10,)], {}),
    ('runtime_eq', '[(NAMES,), (["".join(["sl", "ow"]), "x"],)]', {}),
    ('ordering', [(['a', 'fast', 'é'],)], {}),
    ('subclasses', [('fast',), ('',)], {}),
    ('mixed', [('fast',)], {}),
]
