# A compiled frame's locals read and written through its frame object's
# f_locals while it runs, as CPython sees them: where f_locals is a
# frame-locals proxy (PEP 667) a write is the frame's, elsewhere it is a
# dict that the frame never reads back. Each case checks both by matching
# the interpreter without LeJIT.
import sys


def written_then_read(a):
    x = a
    sys._getframe().f_locals['x'] = a * 10
    return x


def kept_then_changed(a):
    x = a
    d = sys._getframe().f_locals
    x = a + 1
    return d['x'], x


def updated(a):
    x = a
    y = a + 1
    sys._getframe().f_locals.update({'x': 5, 'y': 6})
    return x, y


def set_default_and_pop(a):
    x = a
    d = sys._getframe().f_locals
    d.setdefault('x', 9)
    return x, sorted(k for k in d if k != 'd')


def caller_written(a):
    x = a
    set_callers_x(a * 3)
    return x


def set_callers_x(v):
    sys._getframe(1).f_locals['x'] = v


def written_in_loop(n):
    total = 0
    for i in range(n):
        x = i
        if i == 2:
            sys._getframe().f_locals['x'] = 100
        total += x
    return total


def written_through_kept_proxy(n):
    # the same f_locals each round, so its type is known after the first
    x = -1
    d = sys._getframe().f_locals
    total = 0
    for i in range(n):
        d['x'] = i
        total = total * 10 + x
    return total


def written_after_list_store(items, n):
    # a store into a list, whose type is known, then one through f_locals
    x = 0
    d = sys._getframe().f_locals
    total = 0
    for i in range(n):
        items[0] = i
        d['x'] = items[0] * 2
        total = total * 10 + x
    return total, items


def extra_key_deleted(n):
    x = n
    d = sys._getframe().f_locals
    d['extra'] = 1
    del d['extra']
    d['x'] = n + 1
    return x, 'extra' in d


# a list local compiled code keeps virtual, which the proxy can't read,
# goes on in ceval with the write
def written_beside_a_list(n):
    x = -1
    out = []
    for i in range(n):
        sys._getframe().f_locals['x'] = i
        out.append(x)
    return out


CASES = [
    ('written_then_read', "[(1,), (2,)]", {}),
    ('kept_then_changed', "[(1,), (2,)]", {}),
    ('updated', "[(1,)]", {}),
    ('set_default_and_pop', "[(1,)]", {}),
    ('caller_written', "[(2,)]", {}),
    ('written_in_loop', "[(5,)]", {}),
    ('written_through_kept_proxy', "[(5,)]", {}),
    ('written_after_list_store', "[([0], 5)]", {}),
    ('extra_key_deleted', "[(3,)]", {}),
    ('written_beside_a_list', "[(5,)]", {}),
]
