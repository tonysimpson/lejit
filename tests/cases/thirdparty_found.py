# Differences other projects' test suites found (tools/thirdparty), each cut
# down to the smallest code that shows it, with the project it came from.
import builtins
import gc
import weakref


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
# (3 GB in sympy's hyperexpand tests). The inline store tests the value's
# type for GC when it isn't known, as here.
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
    ('exec_import_star', "[()]", {}),
    ('exec_delete_name', "[()]", {}),
    ('store_tracks_dict', "[([1],), (1,), ([2],), ((x for x in ()),)]", {}),
    ('store_cycle_collected', "[(), (), ()]", {}),
]
