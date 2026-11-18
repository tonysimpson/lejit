"""Calls through decorator layers, functools.partial, lru_cache, singledispatch, methodcaller and attrgetter."""
import functools
import operator


def timed(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        wrapper.calls += 1
        return func(*args, **kwargs)
    wrapper.calls = 0
    return wrapper


def validated(*types):
    def deco(func):
        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            for a, t in zip(args, types):
                if not isinstance(a, t):
                    raise TypeError(a)
            return func(*args, **kwargs)
        return wrapper
    return deco


@timed
@validated(int, int)
def tax(amount, rate):
    return amount * rate // 100


@functools.lru_cache(maxsize=128)
def shipping(zone, weight):
    return 300 + zone * 50 + weight * 7


@functools.singledispatch
def describe(x):
    return 1


@describe.register(int)
def _(x):
    return x % 10


@describe.register(str)
def _(x):
    return len(x)


@describe.register(list)
def _(x):
    return sum(describe(v) for v in x)


class Item:
    def __init__(self, name, price, zone):
        self.name = name
        self.price = price
        self.zone = zone

    def discounted(self, pct):
        return self.price * (100 - pct) // 100


VAT = functools.partial(tax, rate=20)
by_price = operator.attrgetter('price')
discount10 = operator.methodcaller('discounted', 10)


def main(n):
    shipping.cache_clear()
    tax.calls = 0
    items = [Item('i%d' % k, 100 + k * 37 % 900, k % 5) for k in range(60)]
    total = 0
    for i in range(n):
        for it in items[i % 6::6]:
            total += VAT(discount10(it)) + shipping(it.zone, i % 40)
            total += describe([it.price, it.name, [i]])
        total += by_price(max(items, key=by_price))
    info = shipping.cache_info()
    return total, tax.calls, info.hits, info.misses


N = 3000

if __name__ == '__main__':
    print(main(N))
