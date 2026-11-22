# Builders (punicodeobject.c), and str % args beside them: f-strings whose
# int parts are written into the BUILD_STRING's one result without their
# str; sep.join of a list, a tuple and a generator expression fused into a
# list. The identities CPython gives ('%s' % s is s, the cached character,
# the shared '', a one-item join), str subclasses, __format__, __str__ and
# __repr__ with side effects, non-ASCII text widening the result, and
# errors mid-build, with what ran before them.
import sys

LOG = []


class S(str):
    pass


class Noisy:
    def __init__(self, name):
        self.name = name

    def __str__(self):
        LOG.append('str ' + self.name)
        return 'N(%s)' % self.name

    def __repr__(self):
        LOG.append('repr ' + self.name)
        return 'R(%s)' % self.name

    def __format__(self, spec):
        LOG.append('format %s %r' % (self.name, spec))
        return S('F(%s)' % self.name)


class Bad:
    def __str__(self):
        LOG.append('bad str')
        raise ValueError('no str')

    def __format__(self, spec):
        LOG.append('bad format')
        raise KeyError('no format')


class Seq:
    def __getitem__(self, k):
        return 'item'

    def __str__(self):
        return 'Seq'


def percent(xs, s):
    out = []
    for x in xs:
        out.append(('%d' % x, '%s' % x, '%r' % x, '%5d|%-5d|%05d' % (x, x, x),
                    '%x %X %o %#x' % (x, x, x, x), '%+d % d' % (x, x),
                    '%.3f %e %g' % (x / 7, x / 7, x / 7), '%c' % (65 + x % 26),
                    'line %06d: %s%%' % (x, s), '[%s]' % (s,), '%s' % s,
                    '%-8s|%8s|%.2s' % (s, s, s), '%a' % s, '%%'  % ()))
    return out


def percent_identity(s):
    one = '%s' % s
    empty = '%s' % ''
    char = '%s' % 'q'
    digit = '%d' % 7
    return (one is s, empty is '', char is 'q', digit is '7', digit is '%d' % 7,
            ('%s' % S('x')) is 'x', type('%s' % S('x')).__name__,
            ('%s%s' % (s, '')) is s, ('<%s>' % s) is ('<%s>' % s))


def percent_args(x, t):
    # the argument count is checked against the plan at run time for a
    # tuple not known when compiled, and anything else is CPython's
    out = []
    for f in ('%s', '%s %s'):
        try:
            out.append(f % t)
        except TypeError as e:
            out.append(('TypeError', str(e)))
    try:
        out.append('%s %s' % x)
    except TypeError as e:
        out.append(('TypeError', str(e)))
    try:
        out.append('abc' % x)
    except TypeError as e:
        out.append(('TypeError', str(e)))
    out.append('abc' % {})
    out.append('%s' % {'k': 1})
    out.append('%s' % [1, 2])
    out.append('%s' % Seq())
    out.append('%(k)s' % {'k': x})
    out.append('%*d|%.*f' % (5, x, 2, x / 3))
    for f in ('%', '%z', '%(k', 'x%'):
        try:
            out.append(f % x)
        except (TypeError, ValueError) as e:
            out.append((type(e).__name__, str(e)))
    return out


def percent_objects(names):
    del LOG[:]
    out = []
    for n in names:
        a, b = Noisy(n), Noisy(n + '2')
        out.append('%s and %r' % (a, b))
        out.append('<%s>' % a)
    try:
        out.append('%s %s %d' % (Noisy('p'), Noisy('q'), 'not an int'))
    except TypeError as e:
        out.append(('TypeError', str(e)))
    try:
        out.append('%s|%s' % (Noisy('r'), Bad()))
    except ValueError as e:
        out.append(('ValueError', str(e)))
    return out, list(LOG)


def percent_wide(xs):
    out = []
    for x in xs:
        r = '%s|%d|%s' % (x, len(x), x[::-1])
        out.append((r, len(r), max(map(ord, r)) if r else 0))
        out.append('é%s€%s😀' % (x, x))
        out.append('%5s|%-5s|%.1s' % (x, x, x))
    return out


def fstrings(ks, s):
    out = []
    for k in ks:
        out.append((f'line {k}: {s}', f'{k}{k}', f'{-k}|{k * 1000000007}',
                    f'{k}é{s}', f'{k}€', f'{k:>4}<{k}>', f'{k}', f'{k}|{2 ** 64 + k}',
                    f'{k}|{-(2 ** 63)}|{2 ** 63 - 1}', f'{True}{k}', f'{k!r}{k}',
                    f'{k}{s}{k}{s}{k}{s}{k}'))
    return out


def fstring_identity(k, s):
    a = f'{k}{s}'
    b = f'{s}{s}'
    return (a is f'{k}{s}', f'{s}' is s, f'{""}{""}' is '', f'{k}' is str(k),
            len(a), type(f'{k}{S("x")}').__name__, b == s + s)


def fstring_objects(names):
    del LOG[:]
    out = []
    for i, n in enumerate(names):
        out.append(f'{i}:{Noisy(n)}:{i}')
        out.append(f'{i}{Noisy(n)!s}{i}{Noisy(n)!r}')
    try:
        out.append(f'{len(names)}|{Bad()}|{len(names)}')
    except KeyError as e:
        out.append(('KeyError', str(e)))
    return out, list(LOG)


def fstring_huge(k):
    # an int of more digits than int_max_str_digits is CPython's ValueError
    # at its FORMAT_VALUE, before what follows it runs
    big = 10 ** 5000 + k
    try:
        return f'{big}{Noisy("after")}', list(LOG)
    except ValueError as e:
        return 'ValueError', str(e)[:40], list(LOG)


def fstring_wide(ks):
    out = []
    for k in ks:
        out.append((f'{k}😀{k}', f'ÿ{k}', f'{k}{"日本"}{k}'))
    return out


def joins(xs, sep):
    out = []
    for k in range(3):
        parts = []
        for x in xs:
            parts.append('%s' % x)
        out.append(sep.join(parts))
        out.append(sep.join(tuple(parts)))
        out.append(sep.join(str(x) for x in xs))
        out.append(sep.join(p.upper() for p in parts if p))
        out.append(sep.join(x for x in []))
        out.append(''.join(str(x * k) for x in xs))
        out.append(sep.join(iter(parts)))
    return out


def join_identity(s):
    one = [s]
    return (''.join(one) is s, ''.join(x for x in one) is s,
            ','.join(x for x in [s]) is s, ''.join(x for x in []) is '',
            ''.join(x for x in [S('a')]) is 'a', type(''.join([S('a')])).__name__,
            ''.join(x for x in ['', '']) is '', ''.join(['q']) is 'q')


def join_errors(xs):
    del LOG[:]
    out = []

    def gen():
        for x in xs:
            LOG.append('yield %r' % (x,))
            yield x

    for bad in ([1], ['a', 2, 'b'], [S('a'), None]):
        try:
            out.append(''.join(x for x in bad))
        except TypeError as e:
            out.append(('TypeError', str(e)))
    try:
        out.append('-'.join(y for y in gen()))
    except TypeError as e:
        out.append(('TypeError', str(e)))
    try:
        out.append(''.join(1 // 0 if x == 'z' else x for x in xs))
    except ZeroDivisionError as e:
        out.append(('ZeroDivisionError', str(e)))
    try:
        out.append(''.join(5))
    except TypeError as e:
        out.append(('TypeError', str(e)))
    return out, list(LOG)


def join_wide(xs):
    return ('é'.join(xs), '😀'.join(x for x in xs), ''.join(x[::-1] for x in xs))


def frame_locals(k):
    # a frame read while an f-string's parts are on the stack
    return f'{k}:{sys._getframe().f_lineno > 0}:{k}'


CASES = [
    ('percent', [([0, 1, -5, 42, 999, 10 ** 20, -(2 ** 70)], 'ab'),
                 ([3], ''), ([7], 'é€😀')], {}),
    ('percent_identity', [('abc',), ('',), ('x',), ('é',)], {}),
    ('percent_args', [(3, (1,)), (4, (1, 2)), (5, ()), (6, 'xy')], {}),
    ('percent_objects', [(['a', 'b'],), ([],)], {}),
    ('percent_wide', [(['abc', 'é', '€uro', '😀', ''],)], {}),
    ('fstrings', [([0, 1, -7, 12345, 10 ** 18, -(10 ** 18), 2 ** 62], 'ab'),
                  ([5], ''), ([3], 'é€😀')], {}),
    ('fstring_identity', [(0, 'ab'), (5, ''), (-1, 'é')], {}),
    ('fstring_objects', [(['a', 'b'],), ([],)], {}),
    ('fstring_huge', [(1,)], {}),
    ('fstring_wide', [([0, 9, -99, 10 ** 15],)], {}),
    ('joins', [([1, 22, 333], ', '), ([], '-'), (['x'], ''), (['é', '😀', 'a'], '|')], {}),
    ('join_identity', [('abc',), ('',), ('z',)], {}),
    ('join_errors', [(['a', 'b', 3],), (['a', 'z', 'b'],)], {}),
    ('join_wide', [(['abc', 'é', '€', '😀'],), ([],)], {}),
    ('frame_locals', [(4,)], {}),
]
