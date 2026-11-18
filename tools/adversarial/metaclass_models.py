"""Classes with metaclasses (no type version tag on 3.7), descriptors, properties and enums: an ORM-style model layer."""
import enum


class Field:
    def __init__(self, kind, default=None):
        self.kind = kind
        self.default = default

    def __set_name__(self, owner, name):
        self.name = name
        self.slot = '_' + name

    def __get__(self, obj, cls):
        if obj is None:
            return self
        return obj.__dict__.get(self.slot, self.default)

    def __set__(self, obj, value):
        if not isinstance(value, self.kind):
            raise TypeError('%s must be %s' % (self.name, self.kind.__name__))
        obj.__dict__[self.slot] = value


class ModelMeta(type):
    registry = {}

    def __new__(mcls, name, bases, ns):
        cls = super().__new__(mcls, name, bases, ns)
        fields = {}
        for base in reversed(cls.__mro__):
            fields.update((k, v) for k, v in vars(base).items() if isinstance(v, Field))
        cls._fields = fields
        mcls.registry[name] = cls
        return cls

    def __call__(cls, **kw):
        obj = super().__call__(**kw)
        cls.created += 1
        return obj


class Status(enum.Enum):
    DRAFT = 1
    SENT = 2
    PAID = 3
    VOID = 4


class Model(metaclass=ModelMeta):
    created = 0
    id = Field(int, 0)

    def __init__(self, **kw):
        for name, field in self._fields.items():
            setattr(self, name, kw.get(name, field.default))

    def to_dict(self):
        return {name: getattr(self, name) for name in self._fields}

    @property
    def key(self):
        return '%s:%d' % (type(self).__name__.lower(), self.id)


class Customer(Model):
    name = Field(str, '')
    city = Field(str, 'London')
    credit = Field(float, 0.0)


class Line(Model):
    sku = Field(str, '')
    qty = Field(int, 1)
    price = Field(float, 0.0)

    @property
    def amount(self):
        return self.qty * self.price


class Invoice(Model):
    customer = Field(str, '')
    status = Field(Status, Status.DRAFT)
    lines = Field(list, None)

    @property
    def total(self):
        return sum(line.amount for line in self.lines)

    def advance(self):
        order = list(Status)
        self.status = order[(order.index(self.status) + 1) % len(order)]


def main(n):
    for cls in ModelMeta.registry.values():
        cls.created = 0
    revenue = 0.0
    keys = 0
    paid = 0
    for i in range(n):
        c = Customer(id=i, name='cust%d' % (i % 30), credit=float(i % 500))
        lines = [Line(id=k, sku='sku%d' % k, qty=1 + (i + k) % 3, price=2.5 * (k + 1))
                 for k in range(i % 4 + 1)]
        inv = Invoice(id=i, customer=c.key, lines=lines)
        for _ in range(i % 4):
            inv.advance()
        if inv.status is Status.PAID:
            paid += 1
            revenue += inv.total
        keys += len(inv.key) + len(c.to_dict()) + len(inv.to_dict()['customer'])
    return (round(revenue, 2), keys, paid, Customer.created, Line.created,
            Invoice.created, sorted(ModelMeta.registry))


N = 6000

if __name__ == '__main__':
    print(main(N))
