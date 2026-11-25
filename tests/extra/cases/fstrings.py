# f-strings: FORMAT_VALUE (conversions !s !r !a, format specs, a str
# passed through) and BUILD_STRING (any number of parts, joined from one
# array), as in ceval.
import collections


class Odd:
    def __format__(self, spec):
        return 'odd<%s>' % spec

    def __repr__(self):
        return 'Odd()'


class S(str):
    pass


def basic(x, s):
    return f'{x}', f'{s}', f'a{x}b', f'{x}{s}{x}{s}', f'{x}-{s}-{x}-{s}-{x}'


def conversions(x):
    return f'{x!s}', f'{x!r}', f'{x!a}', f'{"é"!a}'


def specs(x, w):
    return f'{x:>6}', f'{x:{w}}', f'{x:.3f}', f'{Odd():xy}', f'{Odd()!r:>8}'


def str_subclass(s):
    r = f'{s}'
    return r, type(r).__name__


def errors(x):
    out = []
    for f in (lambda: f'{x:q}', lambda: f'{Odd()!r:{x}}'):
        try:
            out.append(f())
        except (ValueError, TypeError) as e:
            out.append((type(e).__name__, str(e)))
    return out


def loop(n):
    t = 0
    for i in range(n):
        t += len(f'{i}:{i * 2}')
    return t


Pair = collections.namedtuple('Pair', 'a b')


def namedtuple_methods(n):
    # _make and _replace hold f-strings (their error messages)
    p = Pair(1, 2)
    for i in range(n):
        p = p._replace(a=i)
    try:
        Pair._make([1])
    except TypeError as e:
        return p, str(e)


CASES = [
    ('basic', [(5, 'x'), (2.5, '')], {}),
    ('conversions', '[(5,), ("q",), (Odd(),)]', {}),
    ('specs', [(3.14159, 9), (42, 4)], {}),
    ('str_subclass', '[(S("z"),)]', {}),
    ('errors', [(5,), ('abc',)], {}),
    ('loop', [(100,)], {}),
    ('namedtuple_methods', [(10,)], {}),
]
