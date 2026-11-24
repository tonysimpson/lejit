# Forwarding calls f(*args, **kwargs): while kwargs is an empty dict the call
# is bound as f(*args); anything else (a non-empty dict, a dict subclass, a
# mapping, a non-mapping) makes the call CPython makes.
import collections


class Mod:
    # nn.Module's shape
    def __call__(self, *args, **kwargs):
        return self._call_impl(*args, **kwargs)

    def _call_impl(self, *args, **kwargs):
        return self.forward(*args, **kwargs)

    def forward(self, x, scale=1, **extra):
        return x * scale, sorted(extra.items())


def forward(f, *args, **kwargs):
    return f(*args, **kwargs)


def leaf(a, b=2):
    return a, b


def kwonly(a, *, k=3):
    return a, k


def gen(a, b=0):
    yield a
    yield b


class Point:
    def __init__(self, x, y=0, **more):
        self.v = (x, y, sorted(more))


class Sub(dict):
    def keys(self):
        return ['z']

    def __getitem__(self, key):
        return 9


class Mapping:
    def keys(self):
        return ['b']

    def __getitem__(self, key):
        return 7


def chain(n):
    m = Mod()
    return [(m(i), m(i, scale=2), m(i, q=1), m.forward(i)) for i in range(n)]


def callees(n):
    def inner(a, b=5):
        return a + b
    out = []
    for i in range(n):
        out.append((forward(leaf, i), forward(leaf, i, b=1), forward(kwonly, i),
                    forward(kwonly, i, k=4), list(forward(gen, i)),
                    forward(inner, i), forward(Point, i).v,
                    forward(Point, i, w=1).v, forward(max, i, 3),
                    forward(max, [i, 1], key=None), forward(dict, a=i),
                    forward(dict), forward(Mod().forward, i)))
    return out


def take(a, **k):
    return a, sorted(k.items())


def other_kwargs(n):
    out = []
    for i in range(n):
        for k in ({}, {'b': i}, Sub(), Sub(a=1), Mapping(),
                  collections.OrderedDict()):
            out.append(forward_dict(take, (i,), k))
    return out


def forward_dict(f, a, k):
    return f(*a, **k)


def changed_between(n):
    # the same site called with an empty dict, then not
    out = []
    for i in range(n):
        out.append(forward_dict(leaf, (i,), {}))
        out.append(forward_dict(leaf, (i,), {'b': 0}))
        out.append(forward_dict(leaf, [i], {}))
    return out


def errors(n):
    out = []
    for i in range(n):
        for f, a, k in ((leaf, (), {}), (leaf, (1, 2, 3), {}),
                        (leaf, (1,), {'zz': 1}), (leaf, (1,), {'a': 1}),
                        (leaf, (1,), 5), (leaf, 5, {}), (gen, (), {}),
                        (Point, (), {}), (len, (), {}),
                        (leaf, (1,), {1: 2})):
            try:
                out.append(forward_dict(f, a, k))
            except TypeError as e:
                out.append(str(e))
    return out


CASES = [
    ('chain', [(0,), (3,)], {}),
    ('callees', [(3,)], {}),
    ('other_kwargs', [(2,)], {}),
    ('changed_between', [(3,)], {}),
    ('errors', [(2,)], {}),
]
