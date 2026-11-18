"""Serialiser whose output order is insertion order everywhere: vars() of instances built along different paths, attributes deleted and re-added, setattr with computed names, **kwargs forwarded through layers, class bodies seen by __init_subclass__, dict pop-and-reinsert and OrderedDict.move_to_end: object layouts (maps, hidden classes) that must keep property order (Bolz et al., Runtime Feedback in a Meta-Tracing JIT, ICOOOLPS 2011; Bruni, Fast Properties in V8, 2017)."""
import zlib
from collections import OrderedDict

N = 1100


def dump(x, depth=0):
    if depth > 6:
        return '...'
    if isinstance(x, dict):
        return '{' + ','.join('%s:%s' % (k, dump(v, depth + 1)) for k, v in x.items()) + '}'
    if isinstance(x, (list, tuple)):
        return '[' + ','.join(dump(v, depth + 1) for v in x) + ']'
    if hasattr(x, '__dict__') and not isinstance(x, type):
        return type(x).__name__ + dump(vars(x), depth + 1)
    if hasattr(x, '__slots__'):
        return type(x).__name__ + dump({k: getattr(x, k) for k in x.__slots__ if hasattr(x, k)},
                                       depth + 1)
    return repr(x)


REGISTRY = []


class Record:
    def __init_subclass__(cls, **kw):
        super().__init_subclass__()
        REGISTRY.append((cls.__name__, [k for k in vars(cls) if not k.startswith('__')],
                         sorted(kw.items())))

    def __init__(self, **fields):
        for k, v in fields.items():
            setattr(self, k, v)


class Point(Record, tag='geo'):
    z = 0
    x = 0
    y = 0

    def norm(self):
        return abs(self.x) + abs(self.y)


class Slotted:
    __slots__ = ('b', 'a', 'c')

    def __init__(self, a, b):
        self.a = a
        self.b = b


def make_point(k, path):
    if path == 0:
        return Point(x=k, y=k + 1)
    if path == 1:
        p = Point(y=k)
        p.x = -k
        return p
    if path == 2:
        p = Point()
        for name in ('label', 'x', 'y')[k % 3:] + ('x', 'label')[:k % 3]:
            setattr(p, name, '%s%d' % (name, k))
        return p
    p = Point(x=k, y=k, label='tmp')
    del p.x
    p.x = k * 2
    if k % 2:
        del p.label
        p.label = 'back'
    return p


def forward(**kw):
    return layer_two(extra=len(kw), **kw)


def layer_two(**kw):
    kw.setdefault('stamp', 'L2')
    return Record(**kw)


def reorder_dict(k):
    d = {'a': 1, 'b': 2, 'c': 3, 'd': 4}
    d['b'] = d.pop('b') + k
    del d['a']
    d['a'] = k
    d.update({'c': 30, 'e': 5})
    od = OrderedDict(d)
    od.move_to_end('c', last=k % 2 == 0)
    return d, od


def dynamic_class(k):
    ns = {}
    for name in ['gamma', 'alpha', 'beta'][k % 3:] + ['gamma', 'alpha', 'beta'][:k % 3]:
        ns[name] = k
    return type('Dyn%d' % (k % 3), (Record,), ns)


INITIAL = list(REGISTRY)


def main(n, variant=0):
    REGISTRY[:] = INITIAL
    out = []
    for k in range(n * 10):
        path = k % 4 if variant == 0 else (k * 3 + 1) % 4
        p = make_point(k, path)
        if variant == 1 and k % 5 == 0:
            p.__dict__.clear()
            p.__dict__.update(reversed(list(vars(make_point(k, 2)).items())))
        s = Slotted(k, -k)
        if variant == 1 and k % 2:
            del s.a
            s.c = 'c%d' % k
        if variant == 0:
            r = forward(name='n%d' % k, value=k, owner=p)
        else:
            r = forward(value=k, owner=s, name='n%d' % k, **{'z%d' % (k % 3): k})
        d, od = reorder_dict(k + variant)
        cls = dynamic_class(k + variant)
        inst = cls(**{'f%d' % j: j for j in range(k % 4)})
        out.append('|'.join([dump(p), dump(s), dump(r), dump(d), dump(list(od.items())),
                             dump(inst), dump([a for a in dir(p) if not a.startswith('_')])]))
    reg = REGISTRY[:]
    REGISTRY[:] = INITIAL
    crc = 0
    for line in out:
        crc = zlib.crc32(line.encode(), crc)
    return len(out), crc, out[:30], out[-30:], reg[:12], len(reg)
