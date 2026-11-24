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
    ('written_beside_a_list', "[(5,)]", {}),
]
