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


def main():
    failed = 0
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
    print('%d compile-bound checks, %d failed' % (len(CHECKS), failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
