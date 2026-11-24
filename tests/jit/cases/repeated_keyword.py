# A keyword given twice in a call that unpacks **x: CPython's TypeError
# names the function and the key.


def target(*args, **kwargs):
    return args, kwargs


def twice(x):
    try:
        return target(**x, **{'a': 2})
    except TypeError as e:
        return str(e)


def given_and_unpacked(x):
    try:
        return target(a=1, **x)
    except TypeError as e:
        return str(e)


def not_a_string(x):
    try:
        return target(**x, **{1: 2})
    except TypeError as e:
        return str(e)


CALLS = '[({"a": 1},), ({"b": 1},), ({1: 1},)]'
CASES = [
    ('twice', CALLS, {}),
    ('given_and_unpacked', CALLS, {}),
    ('not_a_string', CALLS, {}),
]
