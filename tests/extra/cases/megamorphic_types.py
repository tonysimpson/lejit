# A type-promotion site (Psyco_NeedType) that keeps seeing new types: past
# MEGAMORPHIC_MAX the frame goes on in ceval from that instruction, so
# the compiled code stops growing and holding each class. Fresh classes
# on every call, so the leak check sees compiled code that kept them.
import collections


def width(row):
    return len(row)                   # inlined: len promotes row's type


def sizes(n):
    out = []
    for k in range(n):
        Row = collections.namedtuple('Row%d' % k, ['a', 'b', 'c'][:1 + k % 3])
        row = Row(*range(1 + k % 3))
        out.append((len(row), width(row), row == Row(*range(1 + k % 3))))
    return out


class Base:
    def __len__(self):
        return 3


def loop_goes_on(n):
    # the rest of the loop and the code after it run in ceval
    total = 0
    seen = []
    for k in range(n):
        cls = type('C%d' % k, (Base,), {'__len__': lambda self, k=k: k})
        obj = cls()
        total += len(obj) * 2
        seen.append(k)
    after = total + len(seen)
    return total, after, seen[-3:]


def in_handlers(n):
    out = []
    for k in range(n):
        cls = type('D%d' % k, (Base,), {})
        try:
            try:
                x = len(cls())
                if k % 4 == 0:
                    raise ValueError(k)
                out.append(x)
            finally:
                out.append('f')
        except ValueError as e:
            out.append(('caught', e.args))
    return out


def with_cells(n):
    acc = []

    def add(v):
        acc.append(v)
    for k in range(n):
        cls = type('E%d' % k, (Base,), {})
        add(len(cls()) + k)
    return acc, len(acc)


def gen_sizes(n):
    for k in range(n):
        cls = type('G%d' % k, (Base,), {})
        yield len(cls())


def in_generator(n):
    return list(gen_sizes(n))


CASES = [
    ('sizes', [(12,)], {}),
    ('loop_goes_on', [(12,)], {}),
    ('in_handlers', [(12,)], {}),
    ('with_cells', [(12,)], {}),
    ('in_generator', [(12,)], {}),
]
