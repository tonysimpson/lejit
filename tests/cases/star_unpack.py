# BUILD_TUPLE_UNPACK_WITH_CALL, BUILD_TUPLE_UNPACK, BUILD_LIST_UNPACK:
# known tuples (a virtual result) and anything else (ceval's code), with
# the "argument after *" message.


def add3(a, b, c):
    return a + b + c


def call_known(x):
    t = (x, 2)
    return add3(1, *t), add3(*(x,), *t[1:] if False else (2, 3))


def call_unknown(xs):
    return add3(xs[0], *xs[1:]), add3(*xs)


def literals(a, b):
    return (*a, *b), [*a, *b], (*a, 1, *b)


def bad_star(x):
    try:
        return add3(1, *x)
    except TypeError as e:
        return str(e)


def deep(d):
    import copy
    return copy.deepcopy(d)


# a global list is a compile-time constant (D25), and PsycoTuple_Load read
# it as a tuple: f(x, *LIST) crashed the compiler (KNOWN_BUGS F45, CPython's
# test_cmd_line_script)
LIST = ['b', 'c']
STR = 'yz'


def star_constant_list(x):
    return add3(x, *LIST), (x, *LIST), [x, *LIST], add3(x, *STR)


CASES = [
    ('call_known', [(5,), (1.5,)], {}),
    ('call_unknown', [([1, 2, 3],), ((4, 5, 6),)], {}),
    ('literals', [((1, 2), [3]), ('ab', range(2))], {}),
    ('bad_star', [(5,), (None,), ((2, 3),)], {}),
    ('deep', [({'a': [1, (2, 3)], 'b': {'c': 1.5}},)], {}),
    ('star_constant_list', [('a',)], {}),
]
