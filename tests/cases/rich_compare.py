# Rich comparison follows Objects/object.c:do_richcompare: each slot runs
# at most once (the reflected one first for a subclass), and when every
# slot says NotImplemented, == and != fall back to identity and ordering
# raises TypeError, without calling the slots again (KNOWN_BUGS F10).


class Left:
    def __init__(self, log):
        self.log = log

    def __eq__(self, other):
        self.log.append('Left.__eq__')
        return NotImplemented

    def __ne__(self, other):
        self.log.append('Left.__ne__')
        return NotImplemented

    def __lt__(self, other):
        self.log.append('Left.__lt__')
        return NotImplemented


class Right:
    def __init__(self, log):
        self.log = log

    def __eq__(self, other):
        self.log.append('Right.__eq__')
        return NotImplemented

    def __ne__(self, other):
        self.log.append('Right.__ne__')
        return NotImplemented

    def __gt__(self, other):
        self.log.append('Right.__gt__')
        return NotImplemented


class Sub(Left):
    def __eq__(self, other):
        self.log.append('Sub.__eq__')
        return NotImplemented


class Counted:
    def __init__(self, log):
        self.log = log

    def __eq__(self, other):
        self.log.append('Counted.__eq__')
        return 'eq'


def not_implemented(n):
    out = []
    for i in range(n):
        log = []
        a, b = Left(log), Right(log)
        r = [a == b, a != b, a == a, a != a]
        try:
            a < b
        except TypeError as e:
            r.append(str(e))
        s = Sub(log)
        r += [a == s, s == a, a != s]
        out.append((r, log))
    return out


def with_result(n):
    log = []
    a, b = Counted(log), Right(log)
    r = []
    for i in range(n):
        r.append((a == b, b == a, a != b))
    return r, log


CASES = [
    ('not_implemented', [(1,), (3,)], {}),
    ('with_result', [(3,)], {}),
]
