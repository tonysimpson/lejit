"""Guards that keep failing: class attributes bumped on every call as id counters, statistics and caches. Variant 1 has other names and whole-number prices."""


class Entity:
    next_id = 0
    TAX = 0.2
    CURRENCY = 'GBP'

    def __init__(self, name):
        Entity.next_id += 1
        self.id = Entity.next_id
        self.name = name

    def label(self):
        return '%s-%d-%s' % (self.name, self.id, self.CURRENCY)


class Order(Entity):
    open_orders = 0
    closed_orders = 0
    largest = 0.0

    def __init__(self, name, price, qty):
        super().__init__(name)
        self.price = price
        self.qty = qty
        Order.open_orders += 1

    def total(self):
        t = self.price * self.qty * (1 + self.TAX)
        if t > Order.largest:
            Order.largest = t
        return t

    def close(self):
        Order.open_orders -= 1
        Order.closed_orders += 1


class Cache:
    hits = 0
    misses = 0
    store = {}

    @classmethod
    def lookup(cls, key):
        if key in cls.store:
            cls.hits += 1
            return cls.store[key]
        cls.misses += 1
        value = cls.store[key] = len(key) * 3
        return value


def reset():
    Entity.next_id = 0
    Order.open_orders = Order.closed_orders = 0
    Order.largest = 0.0
    Cache.hits = Cache.misses = 0
    Cache.store = {}


def main(n, variant=0):
    reset()
    revenue = 0.0
    labels = 0
    for i in range(n):
        if variant == 0:
            o = Order('item%d' % (i % 40), 1.5 + i % 9, 1 + i % 4)
        else:
            o = Order('sku-%d' % (i % 55), 2 + i % 7, 1 + i % 5)
        revenue += o.total()
        labels += len(o.label()) + Cache.lookup(o.name)
        if i % 3 == 0:
            o.close()
    return (round(revenue, 2), labels, Entity.next_id, Order.open_orders,
            Order.closed_orders, Order.largest, Cache.hits, Cache.misses)


N = 100000

if __name__ == '__main__':
    print(main(N))
