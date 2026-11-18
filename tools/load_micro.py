"""Attribute loads on classes that change after their code compiled
(doc/FINDINGS.md):

    python3.7 tools/load_micro.py ni|plain [mode ...]

Modes: 'stable' (the class never changes), 'late' (a class constant is
set once, after the first run compiled the loop, as raytrace's
Vector.ZERO), 'bump' (a class attribute used as a counter, bumped every
iteration), 'bump100' (bumped every 100th), 'abc' (a class with a
metaclass, which CPython 3.7 never gives a version tag). Each loop does
three loads per iteration: an instance attribute, a method call, a class
constant. Prints ms per loop, best of seven runs after the first two; for
counts, run under perf stat with one mode. tools/loadguards.py runs
main() (every mode once per call)."""
import abc
import os
import sys
import time

N = 200000
MODES = ('stable', 'late', 'bump', 'bump100', 'abc')


def make(meta=type):
    class C(metaclass=meta):
        K = 3
        count = 0

        def __init__(self):
            self.x = 1

        def m(self):
            return 1
    return C


def loop(o, n, every):
    C = type(o)
    t = 0
    i = 0
    while i < n:
        t = t + o.x + o.m() + o.K
        if every and i % every == 0:
            C.count = C.count + 1
        i = i + 1
    return t


class Run:
    def __init__(self, mode):
        self.mode = mode
        self.o = make(abc.ABCMeta if mode == 'abc' else type)()
        self.every = {'bump': 1, 'bump100': 100}.get(mode, 0)
        self.runs = 0

    def __call__(self, n):
        if self.mode == 'late' and self.runs == 1:
            type(self.o).ZERO = 0       # one class change after compiling
        self.runs += 1
        return loop(self.o, n, self.every)


_runs = {}


def main():
    for mode in MODES:
        if mode not in _runs:
            _runs[mode] = Run(mode)
        _runs[mode](N // 10)


if __name__ == '__main__':
    if sys.argv[1] == 'ni':
        sys.path.insert(0, os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))))
        import ni  # noqa
    for mode in sys.argv[2:] or MODES:
        r = Run(mode)
        best = None
        for k in range(9):
            t0 = time.perf_counter()
            r(N)
            t = time.perf_counter() - t0
            if k >= 2:
                best = t if best is None else min(best, t)
        print('%-8s %8.2f ms' % (mode, best * 1e3))
