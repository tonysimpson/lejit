# A function's __defaults__ can change after a compiled call to it (as
# unittest.mock's patch.object(f, '__defaults__', ...) does): the call
# must use the defaults the function has then (KNOWN_BUGS F52).


def foo(x=0):
    return x


def foo2(x=0):
    return x


def call2():
    return foo2()


def via_callee():
    r = [call2()]
    foo2.__defaults__ = (5,)
    r.append(call2())
    foo2.__defaults__ = (0,)
    return r


def direct():
    r = [foo()]
    foo.__defaults__ = (1,)
    r.append(foo())
    foo.__defaults__ = (0,)
    r.append(foo())
    foo.__defaults__ = None
    try:
        foo()
    except TypeError as e:
        r.append(str(e))
    foo.__defaults__ = (0,)
    r.append(foo(7))
    return r


def loop(n):
    r = []
    for i in range(n):
        r.append(foo())
        if i == 1:
            foo.__defaults__ = (i,)
    foo.__defaults__ = (0,)
    return r


CASES = [
    ('via_callee', [()], {}),
    ('direct', [()], {}),
    ('loop', [(4,)], {}),
]
