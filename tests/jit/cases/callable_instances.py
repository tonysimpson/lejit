# Calling an instance whose class defines __call__ in Python: the method is
# looked up on the class, never the instance, and called with the instance
# first. Anything else (no __call__, one that isn't a plain function, one
# replaced on the class) is CPython's own call.
import re
import traceback


class Adder:
    def __init__(self, k):
        self.k = k

    def __call__(self, x, y=0, *rest, scale=1, **extra):
        return (x + y + self.k) * scale, rest, sorted(extra.items())


class Forwarder:
    # nn.Module's shape
    def __call__(self, *args, **kwargs):
        return self.impl(*args, **kwargs)

    def impl(self, *args, **kwargs):
        return self.forward(*args, **kwargs)

    def forward(self, x, scale=1):
        return x * scale


class Sub(Adder):
    pass


class Static:
    __call__ = staticmethod(lambda *a: ('static', a))


class ByObject:
    __call__ = Adder(100)


class Plain:
    pass


class Raises:
    def __call__(self, x):
        return 1 // x


def calls(n):
    a, s, f = Adder(1), Sub(2), Forwarder()
    return [(a(i), a(i, 2), a(i, 2, 3, 4), a(i, scale=3), a(i, z=1), s(i),
             f(i), f(i, scale=2), f(x=i), Static()(i), ByObject()(i))
            for i in range(n)]


def on_instance_ignored(n):
    a = Adder(1)
    a.__call__ = lambda *args: 'instance'
    return [a(i) for i in range(n)]


def replaced_on_class(n):
    class C:
        def __call__(self, x):
            return 'first', x
    c = C()
    out = []
    for i in range(n):
        out.append(c(i))
        if i == 1:
            C.__call__ = lambda self, x: ('second', x)
        if i == 3:
            del C.__call__
            try:
                c(i)
            except TypeError as e:
                out.append(str(e))
            C.__call__ = lambda self, x: ('third', x)
    return out


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: Adder(1)(), lambda: Adder(1)(1, q=2, x=3),
                  lambda: Plain()(i), lambda: Raises()(0),
                  lambda: Adder(1)(1, scale=1, **{'scale': 2})):
            try:
                out.append(f())
            except (TypeError, ZeroDivisionError) as e:
                # an object's repr shows its address
                out.append((type(e).__name__,
                            re.sub('0x[0-9a-f]+', 'ADDR', str(e))))
    return out


def tracebacks(n):
    out = []
    for i in range(n):
        try:
            Raises()(0)
        except ZeroDivisionError:
            out.append([(fs.name, fs.line) for fs in
                        traceback.extract_tb(__import__('sys').exc_info()[2])])
    return out


CASES = [
    ('calls', [(0,), (3,)], {}),
    ('on_instance_ignored', [(3,)], {}),
    ('replaced_on_class', [(6,)], {}),
    ('errors', [(2,)], {}),
    ('tracebacks', [(2,)], {}),
]
