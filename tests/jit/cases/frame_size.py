# Code with thousands of locals (a namedtuple of thousands of fields makes
# such a __new__) runs in ceval, and gives what ceval gives. Code of any
# length is analysed.
from collections import namedtuple
import threading

_N = 3000
exec('def many_locals(%s):\n    return sum([%s])'
     % (', '.join('a%d' % i for i in range(_N)),
        ', '.join('a%d' % i for i in range(_N))))


Big = namedtuple('Big', ['f%d' % i for i in range(6000)])


def big_namedtuple(n):
    b = Big(*range(n))
    return b == tuple(range(n)), b.f7, len(b._asdict())


# A long function and a long generator, first called in a thread with a
# small stack: analysing their code takes no C stack per instruction.
_LONG = 30000
_LONG_BODY = ''.join('    a = a + %d\n' % (i % 7) for i in range(_LONG))
exec('def long_function(a):\n' + _LONG_BODY + '    return a\n')
exec('def long_generator(a):\n' + _LONG_BODY + '    yield a\n')


def long_bodies():
    out = []

    def run():
        out.append(long_function(0))
        out.append(list(long_generator(1)))
    old = threading.stack_size(256 * 1024)
    try:
        t = threading.Thread(target=run)
        t.start()
        t.join()
    finally:
        threading.stack_size(old)
    return out


CASES = [
    ('many_locals', '[tuple(range(_N))]',
     {'interpreted': 'more locals than the compiler copies at each merge point'}),
    ('big_namedtuple', [(6000,)], {}),
    ('long_bodies', [()], {}),
]
