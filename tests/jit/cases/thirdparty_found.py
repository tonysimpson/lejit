# Behaviour other projects' test suites depend on, each cut down to the
# smallest code that shows it, with the project it comes from.
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
# more (about 250 bytes) plainly, 4 kB more with LeJIT
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
# exec is reached through a variable so that these functions compile
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


# pydantic's BaseSettings.Config.prepare_field rebuilds a set or a list of
# names with names.__class__(genexpr). The one generator expression,
# fused with set() at the first call, must not be reused for list()
def same_genexpr_two_consumers(x):
    return x.__class__(n for n in x)


# mpmath makes its mpf class with type('mpf', ...) in a method of
# ctx_mp_python and pickles it by that module's name: type() takes
# __module__ from the globals of the frame running, which for a compiled
# frame is the nearest real frame's
_other = {'__name__': 'tp_other'}
_exec('def make_class():\n    return type("X", (), {})\n', _other)


def class_module_from_callee():
    return _other['make_class']().__module__


# trio's outcome.Error.unwrap: 'try: raise err  finally: del err', so that
# the traceback's frame doesn't hold the exception in a cycle and it dies
# by its count. The traceback's frame must see the del.
class Err(ValueError):
    pass


def _unwrap(box):
    err = box.pop()
    try:
        raise err
    finally:
        del err


def deleted_after_raise():
    enabled = gc.isenabled()
    gc.disable()
    e = Err()
    r = weakref.ref(e)
    try:
        _unwrap([e])
    except ValueError:
        pass
    del e
    dead = r() is None
    if enabled:
        gc.enable()
    return dead


# SQLAlchemy's Options.__add__ fills an instance's __dict__ with update();
# its keys, built as '_' + name, are equal to the attribute names but not
# the same objects. A read must find them, not the class attribute.
class Shared:
    mode = 'class'


def _shared(value):
    o = Shared()
    o.__dict__.update({''.join(['mo', 'de']): value})
    return o


_kept = _shared('first')


def shared_keys_equal_name():
    return _shared('instance').mode, _kept.mode


# SQLAlchemy's reconnect tests call a fresh Mock (a class of its own each)
# through do_execute, inside a try: past MEGAMORPHIC_MAX types the call
# site in the inlined do_execute goes on in ceval, which must run that
# frame alone, without its caller's try
class _Callable:
    def __call__(self):
        return 1


def _call(f):
    return f()


def megamorphic_call_in_try():
    t = 0
    for i in range(30):
        try:
            t += _call(type('K', (_Callable,), {})())
        except KeyError:
            pass
    return t


CASES = [
    ('megamorphic_call_in_try', "[()]", {}),
    ('shared_keys_equal_name', "[(), (), ()]", {}),
    ('same_genexpr_two_consumers', "[({1},), ([1],), ((2,),), ({3},)]", {}),
    ('class_module_from_callee', "[()]",
     {'xfail': "type() reads the nearest real frame's globals (mpmath test_pickle)"}),
    ('deleted_after_raise', "[()]", {}),
    ('store_tracks_dict', "[([1],), (1,), ([2],), ((x for x in ()),)]", {}),
    ('store_cycle_collected', "[(), (), ()]", {}),
    ('exec_import_star', "[()]", {}),
    ('exec_delete_name', "[()]", {}),
    ('own_line', "[()]", {}),
    ('caller_locals', "[()]", {}),
    ('gc_count_first_call', "[()]",
     {'xfail': 'compiling makes a GC-tracked object (sortedcontainers test_ref_counts)'}),
    ('traced_peak', "[()]",
     {'xfail': "tracemalloc counts the compiler's allocations (more-itertools ichunked)"}),
]
