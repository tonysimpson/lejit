"""Fails if warm code keeps being recompiled on inputs that take paths
already seen.

    python3.X tests/jit/bounds.py [-v]
"""
import os, sys
sys.path.insert(0, os.environ.get('LEJIT_MODULE_DIR') or
                os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import math
import lejit
if not hasattr(lejit._core, '_stats'):
    sys.exit('lejit was not built for tests: build it with LEJIT_ALL_CHECKS=1')
lejit.start()

VERBOSE = '-v' in sys.argv


def buffers():
    return lejit._core._stats()['code_buffers']


# A loop whose condition is a chain of popping jumps back to the loop head
# (POP_JUMP_IF_TRUE to FOR_ITER): the target's merge point must be used, or
# every back-edge compiles a new copy of the body.
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


# Eight counters, each still its first constant or run-time depending on
# the paths taken: every combination is a new state at the loop head. The
# cap per merge point (vcompiler.c:mp_capped) bounds the copies: about
# 75 buffers with it, 440 without.
def counters8(items):
    a = b = c = d = 0
    e = f = g = h = 1
    for x in items:
        if x == 1:
            a += x
        elif x == 2:
            b += x
        elif x == 3:
            c += x
        elif x == 4:
            d += x
        elif x == 5:
            e *= x
        elif x == 6:
            f -= x
        elif x == 7:
            g = x
        elif x == 8:
            h = -x
        else:
            a, h = h, a
    return a + b + c + d + e + f + g + h


def counter_orders(n):
    for i in range(1, 10):
        for j in range(1, 10):
            counters8([i, j] * n)
    counters8(list(range(10)) * n)


CHECKS = [
    # name, function, warm-up arguments, then arguments that must add at
    # most 'allow' code buffers in total. New argument values may compile a
    # little (about 10 here); the bug these guard against gave 150000.
    ('popping back-edges', nqueens, [(6,), (7,)], [(8,), (9,), (10,)], 50),
    ('while/continue', while_continue, [(100,)], [(1000,), (10000,)], 50),
    ('merge-point cap', counter_orders, [], [(1,), (2,), (3,)], 150),
]


# the liveness analysis itself: what is dead at each merge point
def dead_before_loop(n, flag):
    y = 1.5 if flag else 'abc'
    z = len(str(y))
    t = 0
    for i in range(n):
        t += i * z
    return t


def check_dead_locals():
    import dis
    code = dead_before_loop.__code__
    dead = lejit._core._dead_locals(code)
    # at the loop head (FOR_ITER) only t and z are needed; after the loop, t,
    # where a merge point follows it (3.7's SETUP_LOOP target; from 3.8 the
    # loop's exit has one way in and is none)
    head = next(x.offset for x in dis.get_instructions(code)
                if x.opname == 'FOR_ITER')
    after = [set(v) for k, v in dead.items() if k > head]
    ok = set(dead.get(head, ())) == {'n', 'flag', 'y', 'i'} and all(
        s == {'n', 'flag', 'y', 'z', 'i'} for s in after)
    if not ok or VERBOSE:
        print('%-4s dead locals: %r' % ('ok' if ok else 'FAIL', dead))
    return 0 if ok else 1


# which code is inlined: small straight-line code without globals
def small(a, b):
    return a + b


def with_loop(n):
    for i in range(n):
        pass


def uses_global(a):
    return len(a)


def check_inlinable():
    got = (lejit._core._inlinable(small.__code__),
           lejit._core._inlinable(with_loop.__code__),
           lejit._core._inlinable(uses_global.__code__))
    ok = got == (True, False, False)
    if not ok or VERBOSE:
        print('%-4s inlinable small/with_loop/uses_global: %r'
              % ('ok' if ok else 'FAIL', got))
    return 0 if ok else 1


def check_metas_registered():
    # a meta written for other ml_flags isn't registered
    got = lejit._core._unregistered_metas()
    if got or VERBOSE:
        print('%-4s unregistered metas: %r' % ('FAIL' if got else 'ok', got))
    return 1 if got else 0


def fast_calls(xs, s):
    # METH_FASTCALL with 0, 2 and 3 arguments, no meta
    return xs.pop(), math.gcd(len(xs), 6), getattr(s, 'zz', None)


def kw_call(xs):
    return sorted(xs, reverse=True)


def check_fastcall_direct():
    # a METH_FASTCALL builtin without keywords is called with an array,
    # not through PyCFunction_Call; with keywords it still is. Called by eval,
    # so they are compiled here, not inlined into this function earlier
    s0 = lejit._core._stats()
    eval("fast_calls([1, 2], 'a')")
    s1 = lejit._core._stats()
    eval('kw_call([1, 2])')
    s2 = lejit._core._stats()
    got = (s1['cfunc_fast'] - s0['cfunc_fast'],
           s1['cfunc_proxy'] - s0['cfunc_proxy'],
           s2['cfunc_proxy'] - s1['cfunc_proxy'])
    ok = got[0] >= 3 and got[1] == 0 and got[2] >= 1
    if not ok or VERBOSE:
        print('%-4s fastcall direct/residual, keyword residual: %r'
              % ('ok' if ok else 'FAIL', got))
    return 0 if ok else 1


def main():
    failed = (check_dead_locals() + check_inlinable() +
              check_metas_registered() + check_fastcall_direct())
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
    print('%d compile-bound checks, %d failed' % (len(CHECKS) + 4, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
