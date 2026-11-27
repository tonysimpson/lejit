# Methods and attributes of instances whose class keeps their dict where
# the interpreter manages it: a C type (asyncio's Future, which has no
# values on some versions and values in the object on others) and a class
# big enough that its values are far into the object. An attribute set on
# the instance hides the class's method, before and after the dict is made.
import asyncio


def future_methods(n):
    loop = asyncio.new_event_loop()
    out = []
    try:
        for i in range(n):
            f = loop.create_future()
            out.append((f.done(), f.cancelled()))
            f.set_result(i)
            out.append((f.done(), f.result(), f.exception()))
            if i == 1:
                f.done = lambda: 'shadowed'
            out.append(f.done())
    finally:
        loop.close()
    return out


class Big:
    __slots__ = tuple('s%d' % i for i in range(20)) + ('__dict__',)

    def __init__(self, i):
        self.x = i
        self.y = -i

    def method(self):
        return 'method'


def big_values(n):
    out = []
    for i in range(n):
        b = Big(i)
        b.x += 1
        out.append((b.x, b.y, b.method()))
        if i == 1:
            b.method = lambda: 'shadowed'
            out.append(b.method())
        if i == 2:
            b.__dict__['z'] = 0
            out.append((b.x, b.z, b.method()))
    return out


CASES = [
    ('future_methods', [(3,)], {}),
    ('big_values', [(4,)], {}),
]
