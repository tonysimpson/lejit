# Stores into a dict that a watcher watches (PyDict_Watch) must send the
# watcher its events, as CPython's own stores do, and must leave the dict
# watched. _testcapi's watchers record the events; where CPython has no dict
# watchers, only the stores' results are compared.
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


CASES = [
    ('store_existing', [(0,), (3,)], {}),
    ('store_unbuilt_keys', [('a b, a. c a',), ('x',)], {}),
    ('store_before_and_after_watch', [(2,), (3,)], {}),
    ('store_module_global', [(3,)], {}),
    ('store_attribute', [(3,)], {}),
]
