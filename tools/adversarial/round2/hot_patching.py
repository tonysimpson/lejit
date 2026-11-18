"""Pricing engine patched live from inside its own loop: a hot function's __code__ and __defaults__ replaced, instances' __class__ reassigned, methods shadowed per instance and a module global rebound, all while callers' frames are running: code and type assumptions invalidated under active frames (Hoelzle, Chambers and Ungar, PLDI 1992; HotSpot class redefinition through JVMTI RedefineClasses)."""
from decimal import Decimal
import types
import zlib

N = 2500

RATE = 20


def price(item, markup=10):
    return item.base * (100 + markup) // 100


def price_v2(item, markup=10):
    return item.base + markup + item.weight


def price_decimal(item, markup=10):
    return (Decimal(item.base) * (100 + markup) / 100).quantize(Decimal('0.01'))


ORIGINAL_CODE = price.__code__
ORIGINAL_DEFAULTS = price.__defaults__


class Item:
    __slots__ = ('name', 'base', 'weight', 'shipping')

    def __init__(self, name, base, weight):
        self.name, self.base, self.weight = name, base, weight

    def ship(self):
        return self.weight * 2

    def tax(self, amount):
        return amount * RATE // 100


class Fragile(Item):
    __slots__ = ()

    def ship(self):
        return self.weight * 5 + 7


class Exempt(Item):
    __slots__ = ()

    def tax(self, amount):
        return 0


class Bulky:
    def __init__(self, name, base, weight):
        self.name, self.base, self.weight = name, base, weight

    def ship(self):
        return self.weight * 3

    def tax(self, amount):
        return amount * RATE // 100


def quote(item):
    p = price(item)
    s = item.ship()
    t = item.tax(p + s)
    return p + s + t


def run_batch(items, k, variant, log):
    global RATE
    totals = []
    for i, item in enumerate(items):
        totals.append(quote(item))
        step = (i + k) % 50
        if step == 10:
            price.__code__ = price_v2.__code__
            log.append('code v2 at %d' % i)
        elif step == 20:
            price.__defaults__ = (25 + k % 3,)
            log.append('defaults at %d' % i)
        elif step == 30:
            price.__code__ = (price_decimal if variant == 1 else price_v2).__code__
            log.append('code %s at %d' % ('decimal' if variant == 1 else 'v2', i))
        elif step == 40:
            price.__code__ = ORIGINAL_CODE
            price.__defaults__ = ORIGINAL_DEFAULTS
            log.append('restored at %d' % i)
        if variant == 1 and i % 13 == 6 and i + 1 < len(items):
            nxt = items[i + 1]
            if type(nxt) is Item:
                nxt.__class__ = Fragile if i % 2 else Exempt
                log.append('reclassed %s' % nxt.name)
        if variant == 1 and i % 17 == 8:
            b = items[(i + 2) % len(items)]
            if isinstance(b, Bulky):
                b.ship = types.MethodType(lambda self: -self.weight, b)
                log.append('shadowed ship on %s' % b.name)
        if i % 29 == 14:
            RATE = 5 + (RATE + 7) % 30
            log.append('rate %d' % RATE)
    return totals


def make_items(k, variant):
    items = []
    for j in range(200):
        base, weight = (j * 37 + k) % 400 + 20, j % 7 + 1
        cls = Item if variant == 0 else [Item, Fragile, Bulky, Item, Exempt][j % 5]
        items.append(cls('i%d' % j, base, weight))
    return items


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    global RATE
    out = []
    try:
        for k in range(n):
            RATE = 20
            log = []
            items = make_items(k, variant)
            totals = run_batch(items, k, variant, log)
            out.append((totals, log, [type(x).__name__ for x in items[:30]]))
    finally:
        price.__code__ = ORIGINAL_CODE
        price.__defaults__ = ORIGINAL_DEFAULTS
        RATE = 20
    return digest(out, 2)
