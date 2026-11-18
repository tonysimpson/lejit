"""Data pipeline whose shared stage helpers (map, filter, group, window, join, sort) are fed by many pipelines with different record types and key functions: type-profile pollution of shared library code (Click, Fixing the Inlining Problem, 2011; Hoelzle and Ungar, Optimizing Dynamically-Dispatched Calls with Run-Time Type Feedback, PLDI 1994)."""
from collections import namedtuple
from decimal import Decimal
from fractions import Fraction
import itertools
import zlib

N = 130


def pipe(source, *stages):
    for s in stages:
        source = s(source)
    return source


def mapping(f):
    return lambda it: (f(x) for x in it)


def where(pred):
    return lambda it: (x for x in it if pred(x))


def group_by(key, agg):
    def stage(it):
        groups = {}
        order = []
        for x in it:
            k = key(x)
            if k not in groups:
                groups[k] = []
                order.append(k)
            groups[k].append(x)
        return ((k, agg(groups[k])) for k in order)
    return stage


def window(size, f):
    def stage(it):
        buf = []
        for x in it:
            buf.append(x)
            if len(buf) > size:
                del buf[0]
            if len(buf) == size:
                yield f(buf)
    return stage


def sort_by(key, reverse=False):
    return lambda it: iter(sorted(it, key=key, reverse=reverse))


def join(right, lkey, rkey):
    def stage(it):
        index = {}
        for r in right:
            index.setdefault(rkey(r), []).append(r)
        for x in it:
            for r in index.get(lkey(x), ()):
                yield (x, r)
    return stage


def take(n):
    return lambda it: itertools.islice(it, n)


def total(xs, field=None):
    acc = 0
    for x in xs:
        v = x if field is None else field(x)
        if v is not None:
            acc = acc + v
    return acc


def mean(xs):
    vals = [x for x in xs if x is not None]
    return total(vals) / len(vals) if vals else None


def to_number(v):
    if isinstance(v, str):
        v = v.strip()
        if not v:
            return None
        if '/' in v:
            return Fraction(v)
        if '.' in v:
            return Fraction(Decimal(v))
        return int(v)
    return v


Sale = namedtuple('Sale', 'region product qty price')


class Reading:
    __slots__ = ('sensor', 'ts', 'value')

    def __init__(self, sensor, ts, value):
        self.sensor, self.ts, self.value = sensor, ts, value


def sales_dicts(i, k):
    return [{'region': 'r%d' % (j % 5), 'product': 'p%d' % (j * 7 % 11), 'qty': j % 9 + 1,
             'price': (j * 13 + i) % 97 + 1} for j in range(k)]


def sales_mixed(i, k):
    out = []
    for j in range(k):
        price = [(j * 13 + i) % 97 + 1, '%d.25' % (j % 50), '%d/3' % (j % 40 + 1),
                 ' %d ' % j, ''][j % 5]
        out.append(Sale('r%d' % (j % 5), 'p%d' % (j * 7 % 11), j % 9 + 1, price))
    return out


def readings(i, k, kind):
    vals = []
    for j in range(k):
        v = (j * 37 + i) % 101
        if kind == 1:
            v = [v, float(v) / 4, Fraction(v, 7), None, v << 61][j % 5]
        vals.append(Reading('s%d' % (j % 4), j, v))
    return vals


def run_dicts(i, k):
    rows = sales_dicts(i, k)
    by_region = list(pipe(rows, where(lambda r: r['qty'] > 2),
                          mapping(lambda r: dict(r, amount=r['qty'] * r['price'])),
                          group_by(lambda r: r['region'], lambda g: total(g, lambda r: r['amount'])),
                          sort_by(lambda kv: kv[1], True)))
    smooth = list(pipe(rows, mapping(lambda r: r['price']), window(4, mean), take(10)))
    return by_region, smooth


def run_tuples(i, k):
    rows = sales_mixed(i, k)
    by_product = list(pipe(rows, mapping(lambda s: s._replace(price=to_number(s.price))),
                           where(lambda s: s.price is not None),
                           group_by(lambda s: s.product, lambda g: total(g, lambda s: s.price * s.qty)),
                           sort_by(lambda kv: (str(kv[1]), kv[0]))))
    smooth = list(pipe(rows, mapping(lambda s: to_number(s.price)), window(3, mean), take(10)))
    return by_product, smooth


def run_objects(i, k, kind):
    rs = readings(i, k, kind)
    sensors = [('s%d' % s, 'room %d' % (s * 3)) for s in range(4)]
    per_sensor = list(pipe(rs, group_by(lambda r: r.sensor, lambda g: mean([r.value for r in g])),
                           sort_by(lambda kv: kv[0])))
    joined = list(pipe(rs, where(lambda r: r.value is not None and r.value > 50),
                       join(sensors, lambda r: r.sensor, lambda s: s[0]),
                       mapping(lambda xr: (xr[1][1], xr[0].ts, xr[0].value)), take(8)))
    peaks = list(pipe(rs, mapping(lambda r: r.value), window(5, lambda b: max(
        (v for v in b if v is not None), default=None)), take(12)))
    return per_sensor, joined, peaks


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    out = []
    for i in range(n * 6):
        k = 300 + 7 * (i % 30)
        if variant == 0:
            out.append(run_dicts(i, k))
            out.append(run_objects(i, k, 0))
        else:
            out.append(run_tuples(i, k))
            out.append(run_objects(i, k, 1))
            out.append(run_dicts(i, k))
    return digest(out, 4)
