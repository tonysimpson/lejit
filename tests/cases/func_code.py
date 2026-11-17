# A function's __code__ can change after a compiled call to it (as
# unittest.mock and code reloaders do): the call must run the code the
# function has then (KNOWN_BUGS F57).


def foo(x):
    return ('foo', x)


def bar(x):
    return ('bar', x)


def baz(x, y=2):
    return ('baz', x, y)


foo_code = foo.__code__


def call(x):
    return foo(x)


def via_callee():
    r = [call(1)]
    foo.__code__ = bar.__code__
    r.append(call(2))
    foo.__code__ = foo_code
    r.append(call(3))
    return r


def direct():
    r = [foo(1)]
    foo.__code__ = bar.__code__
    r.append(foo(2))
    foo.__code__ = baz.__code__
    try:
        foo(3)
    except TypeError as e:
        r.append(str(e))
    foo.__defaults__ = (5,)
    r.append(foo(3))
    foo.__code__ = foo_code
    foo.__defaults__ = None
    r.append(foo(4))
    return r


def loop(n):
    r = []
    for i in range(n):
        r.append(foo(i))
        if i == 1:
            foo.__code__ = bar.__code__
    foo.__code__ = foo_code
    return r


def local_alias(n):
    # the same function object in a local across a loop
    f = foo
    r = []
    for i in range(n):
        r.append(f(i))
        if i == 1:
            foo.__code__ = bar.__code__
    foo.__code__ = foo_code
    return r


CASES = [
    ('via_callee', [()], {}),
    ('direct', [()], {}),
    ('loop', [(4,)], {}),
    ('local_alias', [(4,)], {}),
]
