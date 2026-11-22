# Indexing with an object that has __index__ (pabstract.c: PsycoObject_GetItem
# and SetItem on types with sq_item but no mp_subscript, e.g. deque).
# PyNumber_AsSsize_t's result may be negative, so a negative index must
# still get the "add the length" step.
from collections import deque


class I:
    def __init__(self, v):
        self.v = v

    def __index__(self):
        return self.v


def get(d, ks):
    out = []
    for k in ks:
        try:
            out.append(d[k])
        except IndexError as e:
            out.append(str(e))
    return out


def set_(d, k):
    d[k] = 'x'
    return list(d)


def repeat(s, k):
    return s * k


CASES = [
    ('get', '[(deque([1, 2, 3]), [I(0), I(-1), I(-3), I(-4), I(3)])]', {}),
    ('set_', '[(deque([1, 2, 3]), I(-1)), (deque([1, 2, 3]), I(0))]', {}),
    ('repeat', '[([1, 2], I(-1)), ([1, 2], I(2)), ("ab", I(3))]', {}),
]
