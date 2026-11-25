# f-strings of 0 to 20 parts, which BUILD_STRING joins from one array
# whatever their number: text, ints, floats, strs and str subclasses,
# objects with their own __format__ and __str__, conversions, format specs
# (nested ones too), and errors raised while formatting a part. Each must
# give what CPython gives: the string, the error, and which parts were
# formatted before it.


class Fmt:
    def __init__(self, log, name):
        self.log, self.name = log, name

    def __format__(self, spec):
        self.log.append(self.name)
        return '<%s:%s>' % (self.name, spec)

    def __str__(self):
        self.log.append('str ' + self.name)
        return 'S' + self.name

    def __repr__(self):
        return 'R' + self.name


class BadFormat:
    def __format__(self, spec):
        raise ValueError('bad format %r' % spec)

    def __repr__(self):
        return 'BadFormat()'


class BadStr:
    def __str__(self):
        raise KeyError('bad str')


class NotStr:
    def __format__(self, spec):
        return 42

    def __repr__(self):
        return 'NotStr()'


class Sub(str):
    pass


# parts alternate text and a formatted value, so an f-string of n parts
# has about n / 2 values; VALUES[k] goes in the k-th one
VALUES = ['i', 's', 'f', 'u', 'x', 'i', 'f!r', 's!a', 'i:>4', 'f:.2f',
          'x:sp', 'x!s', 'x!r', 'u:^5', 'i:{w}']


def _fstring(n):
    src, k = [], 0
    for p in range(n):
        if p % 2:
            v = VALUES[k % len(VALUES)]
            k += 1
            name, _, rest = v.partition('!') if '!' in v else v.partition(':')
            sep = '!' if '!' in v else ':' if ':' in v else ''
            src.append('{' + name + sep + rest + '}')
        else:
            src.append('t%d' % p)
    return "f'" + ''.join(src) + "'"


# one f-string of each width from 0 to 20 parts, written out so each is
# its own BUILD_STRING
exec('def widths_body(i, s, f, u, x, w):\n    return [' +
     ', '.join(_fstring(n) for n in range(21)) + ']\n')


def widths(n):
    out = []
    for r in range(n):
        log = []
        out.append(widths_body(r, 'ab' * r, r / 4, Sub('q%d' % r),
                               Fmt(log, 'x'), 3 + r % 3))
        out.append(log)
    return out


# all parts values: 20 ints, which no part writes inline past a few
def ints(n):
    out = []
    for a in range(n):
        b, c, d = a + 1, -a, a * 1000003
        out.append(f'{a}{b}{c}{d}{a}{b}{c}{d}{a}{b}{c}{d}{a}{b}{c}{d}{a}{b}{c}{d}')
        out.append(f'{a}-{b}-{c}-{d}-{a}-{b}-{c}-{d}-{a}-{b}')
    return out


def conversions(n):
    out = []
    for r in range(n):
        log = []
        x, y, z = Fmt(log, 'x'), 'é%d' % r, Sub('s')
        out.append(f'{x!s}{x!r}{x!a}{y!s}{y!r}{y!a}{z!s}{z!r}{z!a}{r!r}'
                   f'{x!r:>6}{y!a:<9}{z!s:*^7}{r!s:>4}')
        out.append(type(f'{z}').__name__)
        out.append(log)
    return out


def specs(n):
    out = []
    for r in range(n):
        log = []
        w, p = 5 + r % 4, r % 3
        x = Fmt(log, 'x')
        out.append(f'{r:>{w}}|{r / 7:.{p}f}|{r / 3:{w}.{p}}|{"s":{"<"}{w}}|'
                   f'{x:{w}{p}}|{r:x}|{r:#o}|{r:,}|{r * 1e6:e}|{r:+}|'
                   f'{x:}|{x!r:{w}}')
        out.append(log)
    return out


# an error in a part of a wide f-string: the parts before it were
# formatted, those after it weren't
def errors(n):
    out = []
    bads = [BadFormat(), BadStr(), NotStr()]
    for r in range(n):
        log = []
        a, b = Fmt(log, 'a'), Fmt(log, 'b')
        bad = bads[r % len(bads)]
        for make in (
                lambda: f'{a}{r}{a}{r}{a}{r}{a}{r}{bad}{b}{r}{b}{r}{b}',
                lambda: f'{a}{bad!s}{b}',
                lambda: f'{a:x}{b}{r}{a}{b}{r}{a}{b}{bad:{r}}{b}',
                lambda: f'1{a}2{a}3{a}4{a}5{a}6{a}7{a}8{a}9{a}10{bad}{b}'):
            try:
                out.append(make())
            except Exception as e:
                out.append((type(e).__name__, str(e)))
            out.append(log[:])
            del log[:]
    return out


CASES = [
    ('widths', [(1,), (6,)], {}),
    ('ints', [(1,), (8,)], {}),
    ('conversions', [(1,), (5,)], {}),
    ('specs', [(1,), (7,)], {}),
    ('errors', [(1,), (6,)], {}),
]
