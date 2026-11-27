# Calls f(*args, **d) of Python functions where args has a known length and
# d is an exact dict known only at run time: the keywords bind as CPython's
# binder binds them, read from d when the call runs (psyfunc.c:
# cimpl_bind_dict), and anything CPython's binder would raise is the call
# CPython makes.
import collections, gc


def sub(self, loop, method, url, **kwargs):
    return loop, method, url, kwargs, list(kwargs)


def named(a, b=2, c=3):
    return a, b, c


def kw_only(a, b=2, *, c, d=4, **rest):
    return a, b, c, d, rest


def star_both(a, *args, **kwargs):
    return a, args, kwargs


class Obj:
    def m(self, x, y=0, **opts):
        return x, y, sorted(opts.items())


# the syntax is 3.8's: made where the code object has the count
POSONLY = {}
if hasattr(named.__code__, 'co_posonlyargcount'):
    exec('def po(a, b=5, /, c=6, **kw):\n    return a, b, c, kw\n'
         'def po_strict(a, /, b=1):\n    return a, b\n', POSONLY)


def submit_shape(n):
    # httpperf's Session._request -> _submit(..., **call_options)
    options = {'cert': None, 'verify': True, 'timeout': None}
    out = []
    for i in range(n):
        out.append(sub(0, 'loop', 'GET', i, headers=None, auth=i,
                       **options))
        out.append(sub(0, 'loop', 'GET', i, **options))
    return out


def fills_parameters(n):
    out = []
    for i in range(n):
        for d in ({}, {'b': i}, {'c': i, 'b': 1}, {'a': 1, 'b': 2}):
            if 'a' in d:
                out.append(named(**d))
            else:
                out.append(named(i, **d))
        out.append(kw_only(i, **{'c': 1}))
        out.append(kw_only(i, **{'d': 0, 'c': 1, 'z': 2, 'b': 3}))
        out.append(star_both(i, 1, 2, **{'k': i}))
        out.append(star_both(**{'a': i, 'k': 1}))
        o = Obj()
        out.append(o.m(**{'x': i, 'q': 1}))
        out.append(o.m(i, **{'y': 1, 'p': 2}))
        out.append(Obj.m(o, i, **{'r': 3}))
    return out


def positional_only(n):
    if not POSONLY:
        return None
    po, po_strict = POSONLY['po'], POSONLY['po_strict']
    out = []
    for i in range(n):
        out.append(po(i, **{'a': 1, 'b': 2, 'c': 3}))
        out.append(po(i, 7, **{'c': 3}))
        try:
            out.append(po_strict(i, **{'a': 1}))
        except TypeError as e:
            out.append(str(e))
    return out


def defaults_at_run_time(n):
    def f(a, b=1, *, k=2, **rest):
        return a, b, k, rest

    def call(d):
        return f(0, **d)
    out = []
    for i in range(n):
        f.__defaults__ = (i,)
        f.__kwdefaults__ = {'k': -i}
        out.append(call({'z': i}))
        f.__defaults__ = None
        out.append(call({'b': i}))
        try:
            out.append(call({}))
        except TypeError as e:
            out.append(str(e))
        f.__kwdefaults__ = None
        try:
            out.append(call({'b': 1}))
        except TypeError as e:
            out.append(str(e))
        out.append(call({'b': 1, 'k': 2}))
    return out


class Key(str):
    pass


class EqKey(str):
    def __eq__(self, other):
        return str.__eq__(self, 'b') or str.__eq__(self, other)

    __hash__ = str.__hash__


def errors(n):
    out = []
    for i in range(n):
        for f, d in ((sub, {'loop': 1}), (sub, {'url': 1, 'x': 2}),
                     (sub, {'self': 1}), (named, {'zz': 1}),
                     (named, {'a': 1}), (kw_only, {}), (kw_only, {'d': 1}),
                     (named, {1: 2}), (named, {Key('b'): 2}),
                     (named, {EqKey('q'): 2}), (sub, {Key('x'): 1})):
            try:
                if f is sub:
                    out.append(f(0, 'l', 'GET', i, **d))
                else:
                    out.append(f(i, **d))
            except TypeError as e:
                out.append(str(e))
        try:
            out.append(named(1, 2, 3, 4, **{'c': 1}))
        except TypeError as e:
            out.append(str(e))
        try:
            out.append(named(i, b=1, **{'b': 2}))
        except TypeError as e:
            out.append(str(e))
        try:
            out.append(named(i, **{'b': 1}, **{'b': 2}))
        except TypeError as e:
            out.append(str(e))
        # a bound method's errors name it as CPython names it
        o = Obj()
        for d in ({'y': 2}, 5, {1: 1}):
            try:
                out.append(o.m(i, y=1, **d))
            except TypeError as e:
                out.append(str(e))
    return out


class Sub(dict):
    def keys(self):
        return ['b']

    def __getitem__(self, key):
        return 9


class Mapping:
    def keys(self):
        return ['c']

    def __getitem__(self, key):
        return 7


def other_mappings(n):
    out = []
    for i in range(n):
        for d in ({'b': i}, Sub(b=1), Mapping(),
                  collections.OrderedDict(c=i)):
            out.append(named(i, **d))
    return out


LOG = []


def note(x):
    LOG.append(x)
    return x


def order(n):
    # what CPython evaluates, and when, is unchanged
    del LOG[:]
    out = []
    for i in range(n):
        out.append(named(note(i), b=note(1), **note({'c': i})))
        out.append(star_both(*note((i, 2)), **note({'k': note(3)})))
    return out, LOG[:]


SHARED = {'a': [1, 2, 3], 'z': [4]}


def drops_its_dict(a, **rest):
    # the dict the caller passed is emptied before the values are used
    SHARED.clear()
    gc.collect()
    return a, rest


def mutation(n):
    out = []
    for i in range(n):
        d = {'q': [i]}
        r = sub(0, 'l', 'GET', i, **d)
        r[3]['q'].append(1)          # the callee's dict is its own
        r[3]['new'] = 1
        out.append((r, d))
        SHARED.update(a=[i], z=[i + 1])
        out.append(drops_its_dict(**SHARED))
        out.append(SHARED)
    return out


def gen(a, b=0, **k):
    yield a
    yield b
    yield k


def generators(n):
    return [list(gen(i, **{'b': 1, 'c': 2})) for i in range(n)]


CASES = [
    ('submit_shape', [(3,)], {}),
    ('fills_parameters', [(3,)], {}),
    ('positional_only', [(3,)], {}),
    ('defaults_at_run_time', [(3,)], {}),
    ('errors', [(2,)], {}),
    ('other_mappings', [(2,)], {}),
    ('order', [(2,)], {}),
    ('mutation', [(2,)], {}),
    ('generators', [(2,)], {}),
]
