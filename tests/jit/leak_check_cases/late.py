# Cases for tests/jit/leak_check.py.
import sys

CALLS = [0]
KEPT = []
# an instance of a new class at each of 30 calls in the counted rounds
# (calls 11 to 110 of the run): reading its attribute compiles a path for
# that class, keeping about 180 blocks, more than the leak check allows
LATE = {n: type('Late%d' % n, (), {'a': n})() for n in range(20, 50)}
EARLY = type('Early', (), {'a': 0})()


def read(x):
    return x.a


def read_again(x):
    return x.a


def compiles_late():
    CALLS[0] += 1
    x = LATE.get(CALLS[0], EARLY)
    read(x)
    read_again(x)
    return 1


def compiles_late_and_leaks():
    compiles_late()
    leaks()
    return 1


# the leak check counts what LeJIT keeps more than plain CPython does
def leaks():
    if 'lejit' in sys.modules:
        KEPT.append(object())
    return 1


CASES = [
    ('compiles_late', [()], {}),
    ('compiles_late_and_leaks', [()], {}),
    ('leaks', [()], {}),
]
