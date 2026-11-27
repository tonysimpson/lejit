# The order finalisers run in, as in the adversarial suite's finalisers: a
# returning function releases its locals and cells in its frame's order,
# and a dropped list its items last to first.
ORDER = []


class Handle:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        ORDER.append(self.name)


def three_locals(i):
    a = Handle('a%d' % i)
    b = Handle('b%d' % i)
    c = Handle('c%d' % i)
    ORDER.append('end')
    return len(a.name) + len(b.name) + len(c.name)


def locals_released():
    del ORDER[:]
    for i in range(3):
        three_locals(i)
    return ORDER[:]


def pairs_dropped(variant=0):
    del ORDER[:]
    for i in range(3):
        pair = [Handle('p%d' % i), Handle('q%d' % i)]
        if variant and i % 2:
            pair.reverse()
        pair = None
    return ORDER[:]


def pair_kept_to_end():
    del ORDER[:]
    for i in range(3):
        pair = [Handle('p%d' % i), Handle('q%d' % i)]
    pair = None
    return ORDER[:]


def closure_locals(i):
    a = Handle('a%d' % i)
    b = Handle('b%d' % i)
    c = Handle('c%d' % i)
    g = lambda: b
    return 0


def cell_released_from_c():
    # map calls closure_locals from C, so it runs in a frame ceval made,
    # which holds b's cell
    del ORDER[:]
    for i in range(3):
        list(map(closure_locals, [i]))
    return ORDER[:]


CASES = [
    ('locals_released', "[()]", {}),
    ('pairs_dropped', "[(), (1,)]", {}),
    ('pair_kept_to_end', "[()]", {}),
    ('cell_released_from_c', "[()]", {}),
]
