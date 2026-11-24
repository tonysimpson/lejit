# Freeing an object the GC tracks, compiled code calls its tp_dealloc
# itself only where _Py_Dealloc would neither defer the free to the
# trashcan nor hold back the chain the trashcan empties after
# (Python/pyruntime.c:le_tracked_dealloc). Each case frees a chain of
# lists ending in an instance, from a variable (a list, whose type compiled
# code knows) and taken off a list (whose type it doesn't), and says when
# the instance is freed.
import operator
import sys
import weakref


def stack_margin():
    # the C stack's margin decides whether _Py_Dealloc defers a free
    import lejit
    return dict(lejit.build_features()).get('LE_HAS_STACK_MARGIN')


class Node:
    pass


def chain(n):
    # n lists, each holding the next, and an instance at the end
    last = Node()
    seen = weakref.ref(last)
    head = [last]
    last = None
    for _ in range(n):
        head = [head]
    return head, seen


def deep_chain_freed(n):
    # deeper than the C stack could free recursively: the trashcan frees
    # the rest of the chain before the free returns
    out = []
    for i in range(n):
        head, last = chain(100000)
        head = None
        out.append(last() is None)
        head, last = chain(100000)
        pool = [head]
        head = None
        x = pool.pop()
        x = None
        out.append(last() is None)
    return out


def _down(box):
    # box: [the chain, its last instance's weakref, levels back up from
    # the deepest, what was seen]
    try:
        # a call through C, which frees nothing on the way back up
        operator.call(_down, box)
    except RecursionError:
        box[2] = 0
        return
    box[2] += 1
    if box[2] == 4:
        # a few calls above the C stack's limit: _Py_Dealloc defers the
        # free to the trashcan, and the chain lives on until a free with
        # room to spare empties it
        pool = [box[0]]
        box[0] = None
        x = pool.pop()
        x = None
        box[3].append(box[1]() is None)
        node = Node()
        last = weakref.ref(node)
        node = None
        box[3].append(last() is None)
        box[3].append(last)


def near_stack_limit(n):
    if not stack_margin():
        return True
    out = []
    old = sys.getrecursionlimit()
    sys.setrecursionlimit(10 ** 6)
    try:
        for i in range(n):
            head, last = chain(10)
            box = [head, last, 0, []]
            head = None
            _down(box)
            seen = box[3]
            # by the next free, with room to spare, of an object the GC
            # tracks, the trashcan is empty
            node = Node()
            node = None
            out.append((seen[0], seen[1], last() is None, seen[2]() is None))
    finally:
        sys.setrecursionlimit(old)
    return out


CASES = [
    ('deep_chain_freed', [(1,)], {}),
    ('near_stack_limit', [(3,)], {}),
]
