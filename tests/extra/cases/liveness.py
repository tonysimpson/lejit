# Locals not read again are kept, as CPython keeps them (mergepoints.c).
# Results must not change: locals read later, on any path (loops,
# handlers, finally, break/continue), must survive, and objects with
# __del__ must die when CPython kills them, not earlier.

LOG = []


class Noisy:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append('del ' + self.name)


def dead_before_loop(n, flag):
    y = 1.5 if flag else 'abc'
    z = len(str(y))
    t = 0
    for i in range(n):
        t += i * z
    return t


def changing_type_in_loop(n):
    out = []
    for i in range(n):
        x = i if i % 2 else str(i)
        out.append(x)
        x = None
    return out


def live_in_handler(a):
    keep = len(a)
    try:
        b = a[3]
    except IndexError:
        return keep
    return b + keep


def live_after_finally_break(n):
    found = -1
    for i in range(n):
        tmp = i * 2
        try:
            if tmp > 6:
                found = tmp
                break
        finally:
            last = i
    return found, last


def live_after_continue_in_try(n):
    t = 0
    for i in range(n):
        w = i * 3
        try:
            if i % 2:
                continue
            t += w
        finally:
            t += 1
    return t


def loop_carried(n):
    a, b = 0, 1
    for i in range(n):
        a, b = b, a + b
    return a


# CPython frees 'd' when it is rebound or the function returns, not when
# it is last read
def del_timing(n):
    del LOG[:]
    d = Noisy('first')
    LOG.append('made')
    t = 0
    for i in range(n):
        t += i
    LOG.append('loop done')
    d = Noisy('second')
    LOG.append('rebound')
    return t


def del_order(n):
    del_timing(n)
    LOG.append('returned')
    return list(LOG)


CASES = [
    ('dead_before_loop', [(10, True), (10, False), (0, True)], {}),
    ('changing_type_in_loop', [(6,)], {}),
    ('live_in_handler', "[([1, 2],), ([1, 2, 3, 4],)]", {}),
    ('live_after_finally_break', [(10,), (2,)], {}),
    ('live_after_continue_in_try', [(7,)], {}),
    ('loop_carried', [(30,)], {}),
    ('del_order', [(5,)], {}),
]
