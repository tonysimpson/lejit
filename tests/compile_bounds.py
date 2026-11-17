"""Bounds on how much Ni compiles (run under Ni by tools/check.sh).

Behaviour tests can't see code that is compiled over and over while giving
the right answer. Each check runs a function until its code is warm, then
on growing inputs that only take paths already seen, and fails if more
code buffers are started than a small allowance.

    python3.7 tests/compile_bounds.py [-v]
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import ni

VERBOSE = '-v' in sys.argv


def buffers():
    return ni._stats()['code_buffers']


# A loop whose condition is a chain of popping jumps back to the loop head
# (POP_JUMP_IF_TRUE to FOR_ITER): the target's merge point must be used, or
# every back-edge compiles a new copy of the body (found while doing D10).
def place(n, row, cols, d1, d2):
    if row == n:
        return 1
    count = 0
    for c in range(n):
        if not cols[c] and not d1[row + c] and not d2[row - c + n]:
            cols[c] = True; d1[row + c] = True; d2[row - c + n] = True
            count += place(n, row + 1, cols, d1, d2)
            cols[c] = False; d1[row + c] = False; d2[row - c + n] = False
    return count


def nqueens(n):
    return place(n, 0, [False] * n, [False] * (2 * n), [False] * (2 * n + 1))


def while_continue(n):
    i = t = 0
    while i < n:
        i += 1
        if i % 3 == 0 or i % 5 == 0:
            continue
        t += i
    return t


CHECKS = [
    # name, function, warm-up arguments, then arguments that must add at
    # most 'allow' code buffers in total. New argument values may compile a
    # little (about 10 here); the bug these guard against gave 150000.
    ('popping back-edges', nqueens, [(6,), (7,)], [(8,), (9,), (10,)], 50),
    ('while/continue', while_continue, [(100,)], [(1000,), (10000,)], 50),
]


# the liveness analysis itself (D15): what is dead at each merge point
def dead_before_loop(n, flag):
    y = 1.5 if flag else 'abc'
    z = len(str(y))
    t = 0
    for i in range(n):
        t += i * z
    return t


def check_dead_locals():
    dead = ni._dead_locals(dead_before_loop.__code__)
    # at the loop head (FOR_ITER) only t and z are needed; after the loop, t
    sets = [set(names) for names in dead.values()]
    ok = {'n', 'flag', 'y', 'i'} in sets and {'n', 'flag', 'y', 'z', 'i'} in sets
    if not ok or VERBOSE:
        print('%-4s dead locals: %r' % ('ok' if ok else 'FAIL', dead))
    return 0 if ok else 1


# which code is inlined (B6): small straight-line code without globals
def small(a, b):
    return a + b


def with_loop(n):
    for i in range(n):
        pass


def uses_global(a):
    return len(a)


def check_inlinable():
    got = (ni._inlinable(small.__code__), ni._inlinable(with_loop.__code__),
           ni._inlinable(uses_global.__code__))
    ok = got == (True, False, False)
    if not ok or VERBOSE:
        print('%-4s inlinable small/with_loop/uses_global: %r'
              % ('ok' if ok else 'FAIL', got))
    return 0 if ok else 1


def check_metas_registered():
    # a meta written for other ml_flags isn't registered (KNOWN_BUGS F13)
    got = ni._unregistered_metas()
    if got or VERBOSE:
        print('%-4s unregistered metas: %r' % ('FAIL' if got else 'ok', got))
    return 1 if got else 0


def main():
    failed = check_dead_locals() + check_inlinable() + check_metas_registered()
    for name, fn, warm, more, allow in CHECKS:
        for args in warm:
            fn(*args)
        before = buffers()
        for args in more:
            fn(*args)
        grew = buffers() - before
        bad = grew > allow
        failed += bad
        if bad or VERBOSE:
            print('%-4s %s: %d new code buffers after warm-up (allowed %d)'
                  % ('FAIL' if bad else 'ok', name, grew, allow))
    print('%d compile-bound checks, %d failed' % (len(CHECKS) + 3, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
