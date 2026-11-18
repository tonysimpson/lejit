# The opcodes step 8 generates from ceval.c (D88): STORE_DEREF and
# DELETE_DEREF (the cell's old value released at run time), IMPORT_FROM,
# BUILD_MAP_UNPACK(_WITH_CALL), BUILD_STRING and FORMAT_VALUE, with their
# error exits and messages; and the hand BUILD_TUPLE_UNPACK* and
# IMPORT_NAME around them.
import sys
import types

freed = []


class Tracked:
    def __init__(self, n):
        self.n = n

    def __del__(self):
        freed.append(self.n)


def cell_stores(k):
    # STORE_DEREF into the frame's own cell: each store releases the old
    # value (a __del__ runs at once)
    del freed[:]
    x = Tracked(k)
    def get():
        return x.n
    a = get()
    x = Tracked(k + 1)
    b = get()
    x = None
    return a, b, sorted(freed)


def cell_deletes(k):
    del freed[:]
    x = Tracked(k)
    def get():
        return x
    del x
    try:
        get()
    except NameError as e:
        msg = str(e)
    return sorted(freed), msg


def cell_unbound(k):
    # DELETE_DEREF of an unbound cell, and a read of one
    x = k
    def get():
        return x
    del x
    out = []
    try:
        del x
    except NameError as e:
        out.append((type(e).__name__, str(e)))
    try:
        x
    except NameError as e:
        out.append((type(e).__name__, str(e)))
    x = k * 2
    out.append(get())
    return out


def nonlocals(k):
    # STORE_DEREF and DELETE_DEREF of a free variable, from inner functions
    x = Tracked(k)
    def store(v):
        nonlocal x
        x = v
    def delete():
        nonlocal x
        del x
    def read():
        return x
    del freed[:]
    store(Tracked(k + 1))
    r = [read().n, list(freed)]
    delete()
    r.append(sorted(freed))
    try:
        read()
    except NameError as e:
        r.append(str(e))
    try:
        delete()
    except NameError as e:
        r.append(str(e))
    store(k)
    r.append(read())
    return r


def counter(n):
    total = 0
    def add(v):
        nonlocal total
        total += v
    for i in range(n):
        add(i)
    return total


# a module whose attribute is missing but whose submodule is in sys.modules
# (import_from's fallback), and one without either
_pkg = types.ModuleType('ni_test_pkg')
_pkg.__path__ = []
_sub = types.ModuleType('ni_test_pkg.sub')
_sub.value = 42
sys.modules['ni_test_pkg'] = _pkg
sys.modules['ni_test_pkg.sub'] = _sub


def from_imports(k):
    from os import path, sep
    from collections import OrderedDict as od
    from ni_test_pkg import sub
    return path.join('a', 'b') == 'a' + sep + 'b', od.__name__, sub.value + k


def from_import_errors(k):
    out = []
    try:
        from os import no_such_name_xyz
    except ImportError as e:
        out.append((type(e).__name__, str(e).split(' (')[0], e.name))
    try:
        from ni_test_pkg import missing
    except ImportError as e:
        out.append((type(e).__name__, str(e).split(' (')[0]))
    try:
        from . import nothing
    except ImportError as e:
        out.append((type(e).__name__, str(e)))
    try:
        import no_such_module_xyz
    except ImportError as e:
        out.append((type(e).__name__, str(e)))
    return out


def imports(k):
    import os
    import os.path as op
    import collections.abc
    return os.sep, op.basename('x/y'), collections.abc.Sized.__name__, k


def map_unpacks(a, b):
    d = {**a, **b}
    e = {'z': 0, **a, 'w': 1, **b}
    return sorted(d.items()), sorted(e.items())


class NotMapping:
    pass


class Keys:
    def keys(self):
        return ['p']

    def __getitem__(self, k):
        return k * 2


def map_unpack_errors(x):
    out = []
    try:
        {**x}
    except TypeError as e:
        out.append(str(e))
    try:
        {'a': 1, **{}, **x}
    except TypeError as e:
        out.append(str(e))
    return out


def kw(**kw):
    return sorted(kw.items())


def call_unpacks(a, b):
    return kw(**a, **b), kw(x=1, **a)


def call_unpack_errors(a, b):
    try:
        return kw(**a, **b)
    except TypeError as e:
        return str(e)


def pos(*args):
    return args


def star_calls(a, b):
    return pos(*a, *b), pos(1, *a, 2, *b), (*a, *b), [*a, *b]


def star_call_errors(a, b):
    try:
        return pos(*a, *b)
    except TypeError as e:
        return str(e)


class R:
    def __repr__(self):
        return 'R()'

    def __str__(self):
        return 'r'

    def __format__(self, spec):
        return 'R:' + spec


class SubStr(str):
    pass


class OddRepr:
    def __repr__(self):
        return SubStr('odd')

    def __str__(self):
        return SubStr('odd-str')

    def __format__(self, spec):
        return SubStr('fmt' + spec)


class BadFormat:
    def __format__(self, spec):
        return 5


def fstrings(v, w):
    n = 3
    return [f'{v}', f'{v!r}', f'{v!s}', f'{v!a}', f'{v:>8}', f'{v!r:>8}',
            f'{w:.{n}f}', f'{v}-{w}', f'a{v}b{w}c', f'{v}{v}{v}{v}{v}{w}',
            f'{w:{n}}', f'{v!s:^9}']


def fstring_types(v):
    # a single part is FORMAT_VALUE's result itself (KNOWN_BUGS F98, F110)
    return [type(f'{v}').__name__, type(f'{v!r}').__name__,
            type(f'{v!s}').__name__, type(f'{v:>3}').__name__,
            type(f'{v!a}').__name__]


def fstring_errors(v):
    try:
        return f'{v}'
    except TypeError as e:
        return str(e)


def join_strings(a, b, c):
    # BUILD_STRING: up to ARRAY_ARGS_MAX parts as words, more as a tuple
    return [f'{a}{b}', f'{a}{b}{c}', f'{a}-{b}', f'<{a}|{b}|{c}>',
            f'{a}{b}{c}{a}{b}{c}{a}{b}{c}']


CASES = [
    ('cell_stores', [(1,), (10,)], {}),
    ('cell_deletes', [(1,), (5,)], {}),
    ('cell_unbound', [(1,), ('s',)], {}),
    ('nonlocals', [(1,), (7,)], {}),
    ('counter', [(10,), (0,)], {}),
    ('from_imports', [(1,), (2,)], {}),
    ('from_import_errors', [(0,)], {}),
    ('imports', [(1,)], {}),
    ('map_unpacks', [({'a': 1}, {'b': 2}), ({'a': 1}, {'a': 3, 'w': 9}), ({}, {})], {}),
    ('map_unpack_errors', "[(1,), (NotMapping(),), (Keys(),), ([('a', 1)],)]", {}),
    ('call_unpacks', [({'a': 1}, {'b': 2}), ({}, {'c': 3})], {}),
    ('call_unpack_errors', "[({'a': 1}, {'a': 2}), ({'a': 1}, 5), (NotMapping(), {}),"
                           " ({1: 2}, {}), (Keys(), {'p': 1})]", {}),
    ('star_calls', [((1, 2), [3]), ([], ()), ('ab', range(2))], {}),
    ('star_call_errors', [((1,), 5), (None, ())], {}),
    ('fstrings', "[(1, 2.5), ('s', 0.1), (R(), 3.0), ('\\xe9', 1.0)]", {}),
    ('fstring_types', "[('s',), (5,), (OddRepr(),), (SubStr('q'),)]", {}),
    ('fstring_errors', "[(BadFormat(),), (1,)]", {}),
    ('join_strings', [(1, 'b', 2.5), ('', '', '')], {}),
]
