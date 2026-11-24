# Errors that name a type, or a function, the way CPython names them: from
# 3.15 a type outside builtins and __main__ by its module too, and a
# function whose name can't be made as "function".
import fractions
import math


class Plain:
    pass


class FloatGivesStr:
    def __float__(self):
        return 'no'


class FloatGivesSubclass:
    def __float__(self):
        return Subfloat(1.5)


class Subfloat(float):
    pass


class Nameless:
    def __call__(self, *args, **kwargs):
        return args, kwargs

    def __getattribute__(self, name):
        if name == '__qualname__':
            raise RuntimeError('no name')
        return object.__getattribute__(self, name)


def target(*args, **kwargs):
    return args, kwargs


def dict_star(x):
    try:
        return {**x}
    except TypeError as e:
        return str(e)


def sine(x):
    try:
        return math.sin(x)
    except TypeError as e:
        return str(e)


def repeated_keyword(f, x):
    try:
        return f(**x, **{'a': 2})
    except (TypeError, RuntimeError) as e:
        return '%s: %s' % (type(e).__name__, e)


CASES = [
    ('dict_star', '[(1,), (Plain(),), (fractions.Fraction(1),), ({"a": 1},)]', {}),
    ('sine', '[(FloatGivesStr(),), (FloatGivesSubclass(),), (0.5,)]', {}),
    ('repeated_keyword', '[(target, {"a": 1}), (Nameless(), {"a": 1}), (target, {"b": 1})]', {}),
]
