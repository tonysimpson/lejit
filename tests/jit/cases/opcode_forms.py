# Forms the bytecode compiler chooses differently across versions: truth
# tests, pairs of locals, f-string conversions, function attributes, calls
# by keyword or through a bound attribute, and the end of for loops.


class Truthy:
    def __init__(self, v):
        self.v = v

    def __bool__(self):
        return self.v


class NoTruth:
    def __bool__(self):
        raise ValueError('no truth')


class Cmp:
    """comparisons give objects that are not bools"""

    def __init__(self, v):
        self.v = v

    def __lt__(self, other):
        return Truthy(self.v < other.v)

    def __eq__(self, other):
        return NoTruth() if other is None else Truthy(self.v == other.v)

    __hash__ = None


def truth(values):
    out = []
    for v in values:
        if v:
            out.append(1)
        elif not v:
            out.append(0)
        out.append(bool(v))
    return out


def compare_kept(a, b):
    # the comparison's own result, then its truth
    r = a < b
    return type(r).__name__, 'lt' if a < b else 'ge'


def compare_raises(a):
    try:
        if a == None:  # noqa: E711
            return 'eq'
        return 'ne'
    except ValueError as e:
        return str(e)


def truth_raises(x):
    try:
        while x:
            return 'true'
        return 'false'
    except ValueError as e:
        return str(e)


def swaps(a, b, n):
    for _ in range(n):
        a, b = b, a + b
    x, y = a, b
    return x, y


def store_then_load(n):
    t = 0
    while n:
        n -= 1
        t += n
    return t


def conversions(x, w):
    return [f'{x!r}', f'{x!s}', f'{x!a}', f'{x:>{w}}', f'{x!r:>{w}}', f'{w}{x}']


class Shout(str):
    def __format__(self, spec):
        return 'SHOUT'

    def __str__(self):
        return self


def converted_subclass(s):
    return f'{s!s}', f'{s}', f'{s!s:3}'


def function_attributes(a):
    def inner(x=a, *, y=a + 1) -> int:
        return x + y + a
    return (inner(), inner(1, y=2), inner.__defaults__, inner.__kwdefaults__,
            inner.__annotations__, inner.__closure__[0].cell_contents)


def lambda_defaults(a):
    fs = [lambda i=i: i * a for i in range(3)]
    return [f() for f in fs]


class Obj:
    def __init__(self):
        self.fn = lambda x, y=0: (x, y)

    def m(self, x, y=0):
        return (self.__class__.__name__, x, y)


def calls_by_keyword(o, f):
    return (o.m(1, y=2), o.fn(3, y=4), f(5, y=6), o.m(*(7,), **{'y': 8}),
            f(*(9, 10)), dict(a=1, b=2))


class Base:
    def m(self, x, y=0):
        return ('base', x, y)


class Derived(Base):
    def m(self, x, y=0):
        return super().m(x, y=y + 1)


def super_by_keyword(x):
    return Derived().m(x, y=1)


def loop_ends(items):
    out = []
    for i in items:
        for j in items:
            if j > i:
                break
        else:
            out.append(('else', i))
        out.append(i)
    for k in ():
        out.append(k)
    return out, [i * 2 for i in items if i], sum(i for i in items)


CASES = [
    ('truth', "[([0, 1, '', 'x', [], [0], None, Truthy(True), Truthy(False)],)]",
     {}),
    ('compare_kept', "[(Cmp(1), Cmp(2)), (Cmp(2), Cmp(1)), (1, 2)]", {}),
    ('compare_raises', "[(Cmp(1),), (1,), (None,)]", {}),
    ('truth_raises', "[(NoTruth(),), (1,), (0,)]", {}),
    ('swaps', [(0, 1, 10), (1.5, 2, 3)], {}),
    ('store_then_load', [(10,), (0,)], {}),
    ('conversions', [('é', 4), (12, 5), (None, 6)], {}),
    ('converted_subclass', "[(Shout('x'),), ('y',)]", {}),
    ('function_attributes', [(1,), (10,)], {}),
    ('lambda_defaults', [(2,)], {}),
    ('calls_by_keyword', "[(Obj(), lambda x, y=0: (x * 2, y))]", {}),
    ('super_by_keyword', [(1,)], {}),
    ('loop_ends', [([0, 1, 2],), ([],)], {}),
]
