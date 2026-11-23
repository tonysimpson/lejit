# Another frame-evaluation hook (torch.compile's, a debugger's) must see
# every frame while it is installed: a compiled call, an inlined frame or a
# fused generator skips the hook, so compiled code tests that LeJIT's is
# the installed one first and makes the real call when it isn't
# (mechanisms/tracing.h:le_direct_call_test). _testinternalcapi's recording
# hook stands in for the other one; where it is missing, each case gives
# None. LeJIT's hook goes back in afterwards, as dynamo restores the one it
# found.
import sys

try:
    from _testinternalcapi import set_eval_frame_default, set_eval_frame_record
except ImportError:
    set_eval_frame_record = None

SEEN = ('leaf', 'middle', '<genexpr>')


def leaf(x):
    return x + 1


def middle(x):
    return leaf(x) * 2


def restore():
    set_eval_frame_default()
    lejit = sys.modules.get('lejit')
    if lejit is not None:
        lejit._core._start()


def recorded(run, n):
    if set_eval_frame_record is None:
        return None
    run(n)                            # compiled before the hook goes in
    names = []
    set_eval_frame_record(names)
    try:
        r = run(n)
    finally:
        restore()
    return r, [name for name in names if name in SEEN], run(n)


def calls(n):
    return middle(n) + leaf(n)


def generator(n):
    return sum(x for x in range(n))


def direct(n):
    return recorded(calls, n)


def fused(n):
    return recorded(generator, n)


def own_calls(n):
    # the case's own frame, compiled, calls with the other hook in place
    if set_eval_frame_record is None:
        return None
    first = middle(n) + leaf(n)
    names = []
    set_eval_frame_record(names)
    try:
        then = middle(n) + leaf(n)
    finally:
        restore()
    return first, then, [name for name in names if name in SEEN]


CASES = [
    ('direct', [(3,)], {}),
    ('fused', [(4,)], {}),
    ('own_calls', [(5,)], {}),
]
