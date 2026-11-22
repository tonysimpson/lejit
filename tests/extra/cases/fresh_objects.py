# An operation on constants (globals are constants to LeJIT) folds when LeJIT
# compiles, but its result must be a new object on every run where CPython
# makes one: identity shows it (is, id(), NaN in a set). Results CPython
# takes from a cache stay one object.


BIG = 10 ** 6
SMALL = 7
F = 2.5
S = 'abcdef'
T = ()
L = [1, 2]


class C:
    def m(self):
        return 1


OBJ = C()


def pair(make):
    a, b = make(), make()
    return a is b, a == b


def big_int():
    return [BIG + 1 for _ in range(2)][0] is [BIG + 1 for _ in range(2)][1], \
        pair(lambda: BIG * 3)


def small_int():
    return pair(lambda: SMALL + 1)


def floats():
    return pair(lambda: F * 2.0), pair(lambda: -F)


def tuples():
    return pair(lambda: (F, BIG)), pair(lambda: T + T)


def str_slices():
    return pair(lambda: S[1:4]), pair(lambda: S[2:3]), pair(lambda: S[:])


def bound_methods():
    return pair(lambda: OBJ.m), pair(lambda: L.append), pair(lambda: S.upper)


def slices():
    return pair(lambda: slice(SMALL, BIG))


def kept_in_list(n):
    out = []
    for _ in range(n):
        out.append(F + 1.0)
        out.append(BIG - 1)
    return len(set(map(id, out))), len(out)


CASES = [
    ('big_int', [()], {}),
    ('small_int', [()], {}),
    ('floats', [()], {}),
    ('tuples', [()], {}),
    ('str_slices', [()], {}),
    ('bound_methods', [()], {}),
    ('slices', [()], {}),
    ('kept_in_list', [(3,)], {}),
]
