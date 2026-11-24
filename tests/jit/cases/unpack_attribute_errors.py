# An AttributeError raised while unpacking {**x} or f(**x): CPython says x
# is not a mapping (a TypeError), or, from 3.15, keeps the AttributeError
# when x has keys.


class KeysRaise:
    def keys(self):
        raise AttributeError('from keys')


class ItemRaises:
    def keys(self):
        return ['a']

    def __getitem__(self, key):
        raise AttributeError('from getitem')


class NoKeys:
    pass


def target(**kwargs):
    return kwargs


def dict_star(x):
    try:
        return {**x}
    except (TypeError, AttributeError) as e:
        return '%s: %s' % (type(e).__name__, e)


def call_star_star(x):
    try:
        return target(**x)
    except (TypeError, AttributeError) as e:
        return '%s: %s' % (type(e).__name__, e)


CALLS = '[(KeysRaise(),), (ItemRaises(),), (NoKeys(),), ({"a": 1},)]'
CASES = [
    ('dict_star', CALLS, {}),
    ('call_star_star', CALLS, {}),
]
