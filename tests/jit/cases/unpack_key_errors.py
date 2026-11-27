# A KeyError while unpacking f(**x): a key given twice is a TypeError, and a
# KeyError the mapping raises itself, from keys() or __getitem__, is its own
# (from 3.15; before, CPython takes that too for a key given twice).


class KeysRaise:
    def keys(self):
        raise KeyError('from keys')


class ItemRaises:
    def keys(self):
        return ['b']

    def __getitem__(self, key):
        raise KeyError('from getitem')


class Mapping:
    def keys(self):
        return ['a']

    def __getitem__(self, key):
        return 2


def target(*args, **kwargs):
    return kwargs


def call_star_star(x):
    try:
        return target(**x)
    except (TypeError, KeyError) as e:
        return '%s: %s' % (type(e).__name__, e)


def call_twice(x):
    try:
        return target(a=1, **x)
    except (TypeError, KeyError) as e:
        return '%s: %s' % (type(e).__name__, e)


def merge_twice(x):
    try:
        return target(**{'a': 1}, **x)
    except (TypeError, KeyError) as e:
        return '%s: %s' % (type(e).__name__, e)


CALLS = '[(KeysRaise(),), (ItemRaises(),), (Mapping(),), ({"a": 3},)]'

CASES = [
    ('call_star_star', CALLS, {}),
    ('call_twice', CALLS, {}),
    ('merge_twice', CALLS, {}),
]
