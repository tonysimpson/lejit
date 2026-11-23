# Stores into a dict that a watcher watches (PyDict_Watch) must send the
# watcher its events, as CPython's own stores do, and must leave the dict
# watched. _testcapi's watchers record the events; where CPython has no dict
# watchers, only the stores' results are compared.
from types import FunctionType

try:
    import _testcapi
except ImportError:
    _testcapi = None

WATCHERS = hasattr(_testcapi, 'add_dict_watcher')
G = 0


def watch(d):
    if not WATCHERS:
        return None
    wid = _testcapi.add_dict_watcher(0)
    _testcapi.watch_dict(wid, d)
    return wid


def unwatch(wid, d):
    """the events seen since watch(d)"""
    if wid is None:
        return None
    events = list(_testcapi.get_dict_watcher_events())
    _testcapi.unwatch_dict(wid, d)
    _testcapi.clear_dict_watcher(wid)
    return events


def store_existing(n):
    # a str key the dict already has, then a get and a store of one key
    d = {'a': 0, 'b': 0}
    wid = watch(d)
    for i in range(n):
        d['a'] = i
        d['b'] = d['b'] + i
    return sorted(d.items()), unwatch(wid, d)


def store_unbuilt_keys(text):
    # keys from str methods, which LeJIT probes without building
    d = dict.fromkeys(text.replace(',', '').replace('.', '').split(), 0)
    wid = watch(d)
    for w in text.split():
        d[w.strip('.,')] = d.get(w.strip('.,'), 0) + 1
    return sorted(d.items()), unwatch(wid, d)


def store_before_and_after_watch(n):
    # stores before the dict is watched leave it watchable
    d = {'a': 0}
    for i in range(n):
        d['a'] = i
    wid = watch(d)
    for i in range(n):
        d['a'] = -i
    return d, unwatch(wid, d)


def read_g():
    return G


def store_module_global(n):
    # the module's dict through globals(): a global read sees each store
    g = globals()
    wid = watch(g)
    seen = []
    for i in range(n):
        g['G'] = i
        seen.append(read_g())
    g['G'] = 0
    return seen, unwatch(wid, g)


class Point:
    def __init__(self, x):
        self.x = x


def store_attribute(n):
    # an instance's attributes, once its __dict__ is watched
    p = Point(0)
    wid = watch(p.__dict__)
    for i in range(n):
        p.x = i
    return p.x, unwatch(wid, p.__dict__)


_len = len


def read_len():
    return len


def builtin_under_watch(n):
    # a builtin shadowed and unshadowed in a module dict another watcher
    # watches as well
    g = globals()
    wid = watch(g)
    seen = []
    for i in range(n):
        g['len'] = i
        seen.append(read_len())
        del g['len']
        seen.append(read_len() is _len)
    return seen, unwatch(wid, g)


def take_all_watchers():
    # every dict watcher id taken before anything is compiled: compiled code
    # finds no watcher of its own (a run without LeJIT takes the same)
    while WATCHERS:
        try:
            _testcapi.add_dict_watcher(0)
        except RuntimeError:
            break


def no_watcher_left(n):
    global G
    if n == 0:
        return take_all_watchers()
    seen = []
    for i in range(n):
        G = i
        seen.append(read_g())
        seen.append(read_len() is len)
    G = 0
    return seen


def read_added():
    return lejit_test_added  # noqa: F821


def builtin_deleted(n):
    # a builtin added, read, then deleted: compiled code must raise
    import builtins
    seen = []
    for i in range(n):
        builtins.lejit_test_added = i
        seen.append(read_added())
        del builtins.lejit_test_added
        try:
            read_added()
        except NameError as e:
            seen.append(str(e))
    return seen


def read_x():
    return X  # noqa: F821


def fresh_globals(n):
    # one function's code run with a new globals dict each time, each made
    # as the last is freed, so often at its address
    seen = []
    for i in range(n):
        f = FunctionType(read_x.__code__, {'X': i})
        got = 0
        for j in range(50):
            got += f()
        seen.append(got)
        del f
    return seen


CASES = [
    ('builtin_deleted', [(3,)], {}),
    ('fresh_globals', [(20,)], {}),
    ('builtin_under_watch', [(3,)], {}),
    ('no_watcher_left', [(0,), (3,)], {}),
    ('store_existing', [(0,), (3,)], {}),
    ('store_unbuilt_keys', [('a b, a. c a',), ('x',)], {}),
    ('store_before_and_after_watch', [(2,), (3,)], {}),
    ('store_module_global', [(3,)], {}),
    ('store_attribute', [(3,)], {}),
]
