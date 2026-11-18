# Differences other projects' test suites found (tools/thirdparty), each cut
# down to the smallest code that shows it, with the project it came from.
import builtins
import gc
import sys
import traceback
import tracemalloc
import weakref


# pyparsing's _trim_arity finds the line of its own call with
# extract_stack and compares it with the traceback of a TypeError, to tell
# a parse action's wrong arity from a TypeError raised inside it
def own_line():
    x = 1
    return traceback.extract_stack(limit=1)[-1].lineno - own_line.__code__.co_firstlineno + x - 1


# pyparsing's autoname_elements names parser elements from its caller's
# f_locals
def caller_locals_callee():
    return sorted(sys._getframe(1).f_locals)


def caller_locals():
    element = 'a'
    other = 'b'
    return caller_locals_callee(), element + other


class Box:
    def __init__(self):
        self.x = 1


# sortedcontainers' test_ref_counts: the GC objects around the first call
def gc_count_first_call():
    start = len(gc.get_objects())
    b = Box()
    del b
    return len(gc.get_objects()) - start


def _strings():
    while True:
        yield 'X' * 5000


def _drain(it):
    for x in it:
        pass


# more-itertools' IchunkedTests.test_memory_in_order: the peak traced while
# a function compiled for the first time runs is two strings and a little
# more (about 250 bytes) plainly, 4 kB more with Ni
def traced_peak():
    g = _strings()
    next(g)
    tracemalloc.start()
    _drain(x for _, x in zip(range(50), g))
    peak = tracemalloc.get_traced_memory()[1]
    tracemalloc.stop()
    return peak < 2 * sys.getsizeof('X' * 5000) + 2000


# sympy's lambdify fills a namespace with exec('from mpmath import *', {},
# namespace). Code run with locals other than its globals must not be
# compiled: the compiler's IMPORT_STAR and DELETE_NAME use the globals.
# exec is reached through a variable so that these functions compile (C15)
_exec = builtins.exec


def exec_import_star():
    g, ns = {}, {}
    _exec('from math import *', g, ns)
    return 'fabs' in ns, 'fabs' in g


def exec_delete_name():
    g, ns = {'x': 1}, {'x': 2}
    _exec('del x', g, ns)
    return sorted(g), sorted(ns)


# sympy's preorder_traversal: __init__ stores a bool, then a generator whose
# frame holds self. The dict, untracked while it holds only the bool, must
# be tracked once it holds the generator, or the cycle is never collected
# and every traversal leaks. The inline store tests the value's type for GC
# when it isn't known, as here.
class Two:
    def __init__(self, v):
        self.flag = False
        self.v = v


def store_tracks_dict(v):
    return gc.is_tracked(Two(v).__dict__)


def store_cycle_collected():
    v = []
    t = Two(v)
    v.append(t)
    w = weakref.ref(t)
    del t, v
    gc.collect()
    return w() is None


CASES = [
    ('store_tracks_dict', "[([1],), (1,), ([2],), ((x for x in ()),)]", {}),
    ('store_cycle_collected', "[(), (), ()]", {}),
    ('exec_import_star', "[()]", {}),
    ('exec_delete_name', "[()]", {}),
    ('own_line', "[()]",
     {'xfail': 'C11: a real frame running compiled code reports its first line (pyparsing parse actions)'}),
    ('caller_locals', "[()]",
     {'xfail': "C11: a compiled caller's f_locals is empty (pyparsing autoname_elements)"}),
    ('gc_count_first_call', "[()]",
     {'xfail': 'C20: compiling makes a GC-tracked object (sortedcontainers test_ref_counts)'}),
    ('traced_peak', "[()]",
     {'xfail': "C60: tracemalloc counts the compiler's allocations (more-itertools ichunked)"}),
]
