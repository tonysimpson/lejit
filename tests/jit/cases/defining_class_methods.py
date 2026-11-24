# C methods that take their defining class (METH_METHOD, as re.Pattern's
# and re.Match's): called straight after the load they make no bound
# method; loaded and kept, passed on or looked at, they are the bound
# method CPython makes.
import re

P = re.compile(r'(\w)(\d)?')


def calls(n):
    out = []
    for i in range(n):
        s = 'a1 b c%d' % i
        out.append((P.sub('-', s),
                     P.sub(repl='+', string=s, count=1), P.findall(s),
                     P.split(s, maxsplit=1), P.match(s).groups(),
                     P.fullmatch('a1') is not None, P.search(s).span(),
                     P.match(s).group(1, 2)))
    return out


def iterated(n):
    return [[m.group(0) for m in P.finditer('a1 b c%d' % i)] for i in range(n)]


def kept(n):
    out = []
    for i in range(n):
        f = P.finditer
        g = P.match('a1').group
        out.append((type(f).__name__, f.__self__ is P, f.__name__,
                    [m.group() for m in f('x y')], g(1), f == P.finditer,
                    f is P.finditer))
    return out


def passed(n):
    def call(f, *a):
        return f(*a)
    return [(call(P.sub, '', 'a1 b'), call(P.findall, 'q%d' % i))
            for i in range(n)]


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: P.sub(), lambda: P.match(5), lambda: P.sub('x'),
                  lambda: P.split('a', nope=1),
                  lambda: re.Pattern.match('notapattern', 'a')):
            try:
                out.append(f())
            except TypeError as e:
                out.append(str(e))
    return out


CASES = [
    ('calls', [(0,), (3,)], {}),
    ('iterated', [(0,), (3,)],
     {'leaks': "plain CPython keeps more blocks after finditer's rounds too"}),
    ('kept', [(2,)], {}),
    ('passed', [(2,)], {}),
    ('errors', [(2,)], {}),
]
