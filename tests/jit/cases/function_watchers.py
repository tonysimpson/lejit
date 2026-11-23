# A function the compiled code makes, with defaults, keyword defaults,
# annotations or a closure, gives a function watcher (PyFunction_AddWatcher)
# only its creation, as ceval's MAKE_FUNCTION does: not a modification for
# each field. Where CPython has no function watchers, only the functions are
# compared.
try:
    import _testcapi
except ImportError:
    _testcapi = None

WATCHERS = hasattr(_testcapi, 'add_func_watcher')
NAMES = {}
if WATCHERS:
    NAMES = {getattr(_testcapi, n): n[len('PYFUNC_EVENT_'):]
             for n in dir(_testcapi) if n.startswith('PYFUNC_EVENT_')}


def made(a):
    def g(x=a, *, y=a + 1) -> int:
        return x + y + a
    return g


def watched(a):
    events = []
    if WATCHERS:
        wid = _testcapi.add_func_watcher(
            lambda e, f, v: events.append(NAMES[e]) if f.__name__ == 'g'
            else None)
    g = made(a)
    if WATCHERS:
        _testcapi.clear_func_watcher(wid)
    return (g(), g.__defaults__, g.__kwdefaults__, g.__annotations__,
            [e for e in events if e != 'DESTROY'])


CASES = [
    ('watched', [(1,), (10,)], {}),
]
