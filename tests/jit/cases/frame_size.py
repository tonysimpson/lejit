# Code with thousands of locals (a namedtuple of thousands of fields makes
# such a __new__) runs in ceval, and gives what ceval gives.
from collections import namedtuple

_N = 3000
exec('def many_locals(%s):\n    return sum([%s])'
     % (', '.join('a%d' % i for i in range(_N)),
        ', '.join('a%d' % i for i in range(_N))))


Big = namedtuple('Big', ['f%d' % i for i in range(6000)])


def big_namedtuple(n):
    b = Big(*range(n))
    return b == tuple(range(n)), b.f7, len(b._asdict())


CASES = [
    ('many_locals', '[tuple(range(_N))]',
     {'interpreted': 'more locals than the compiler copies at each merge point'}),
    ('big_namedtuple', [(6000,)], {}),
]
