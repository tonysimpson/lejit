"""ORM-style layer of metaclass-built descriptors, __getattr__ fallbacks, lazy relations, operator-overloaded query expressions and fields added to a model class at run time: hidden-class transitions and map deprecation (Bruni, Fast Properties in V8, 2017; Hoelzle, Chambers and Ungar, ECOOP 1991)."""
from decimal import Decimal
import zlib

N = 350


class Field:
    creation = 0

    def __init__(self, default=None):
        self.default = default
        Field.creation += 1
        self.order = Field.creation

    def __set_name__(self, owner, name):
        self.name = name

    def __get__(self, obj, cls):
        if obj is None:
            return Col(self.name)
        return obj.__dict__.get(self.name, self.default)

    def __set__(self, obj, value):
        obj.__dict__[self.name] = self.clean(value)

    def clean(self, value):
        return value


class IntField(Field):
    def clean(self, value):
        return int(value)


class StrField(Field):
    def clean(self, value):
        return str(value).strip()


class DecimalField(Field):
    def clean(self, value):
        return Decimal(value).quantize(Decimal('0.01'))


class ForeignKey(Field):
    def __init__(self, target):
        super().__init__()
        self.target = target

    def __get__(self, obj, cls):
        if obj is None:
            return Col(self.name)
        cache = obj.__dict__.setdefault('_fk_cache', {})
        if self.name not in cache:
            key = obj.__dict__.get(self.name + '_id')
            cache[self.name] = obj.db.get(REGISTRY[self.target], key)
        return cache[self.name]

    def __set__(self, obj, value):
        obj.__dict__[self.name + '_id'] = value
        obj.__dict__.pop('_fk_cache', None)


REGISTRY = {}


class ModelMeta(type):
    def __new__(mcls, name, bases, ns):
        cls = super().__new__(mcls, name, bases, ns)
        fields = {}
        for b in reversed(cls.__mro__):
            for k, v in vars(b).items():
                if isinstance(v, Field):
                    fields[k] = v
        cls._fields = dict(sorted(fields.items(), key=lambda kv: kv[1].order))
        REGISTRY[name] = cls
        return cls

    def add_field(cls, name, field):
        field.__set_name__(cls, name)
        setattr(cls, name, field)
        cls._fields = dict(cls._fields, **{name: field})

    def remove_field(cls, name):
        delattr(cls, name)
        cls._fields = {k: v for k, v in cls._fields.items() if k != name}


class Model(metaclass=ModelMeta):
    id = IntField()

    def __init__(self, db, **kw):
        self.db = db
        for k, f in self._fields.items():
            if k in kw:
                setattr(self, k, kw[k])

    def __getattr__(self, name):
        if name.startswith('get_') and name.endswith('_display'):
            v = getattr(self, name[4:-8])
            return lambda: ('%s' % v).replace('_', ' ').title()
        raise AttributeError(name)

    def as_row(self):
        return tuple((k, getattr(self, k)) for k in self._fields if not isinstance(
            self._fields[k], ForeignKey))


class Customer(Model):
    name = StrField()
    tier = StrField('basic')
    credit = DecimalField(Decimal('0'))


class Order(Model):
    customer = ForeignKey('Customer')
    total = DecimalField(Decimal('0'))
    status = StrField('new')
    lines = IntField(1)


class Col:
    def __init__(self, name):
        self.name = name

    def ev(self, obj):
        return getattr(obj, self.name)

    def _op(self, other, fn, sym):
        return Expr(self, other, fn, sym)

    def __eq__(self, o):
        return self._op(o, lambda a, b: a == b, '==')

    def __ne__(self, o):
        return self._op(o, lambda a, b: a != b, '!=')

    def __lt__(self, o):
        return self._op(o, lambda a, b: a < b, '<')

    def __gt__(self, o):
        return self._op(o, lambda a, b: a > b, '>')

    def __add__(self, o):
        return self._op(o, lambda a, b: a + b, '+')

    def __mul__(self, o):
        return self._op(o, lambda a, b: a * b, '*')

    __hash__ = None

    def __repr__(self):
        return self.name


class Expr(Col):
    def __init__(self, left, right, fn, sym):
        self.left, self.right, self.fn, self.sym = left, right, fn, sym

    def ev(self, obj):
        r = self.right.ev(obj) if isinstance(self.right, Col) else self.right
        return self.fn(self.left.ev(obj), r)

    def __and__(self, o):
        return Expr(self, o, lambda a, b: a and b, '&')

    def __or__(self, o):
        return Expr(self, o, lambda a, b: a or b, '|')

    def __invert__(self):
        return Expr(self, None, lambda a, b: not a, '~')

    def __repr__(self):
        return '(%r %s %r)' % (self.left, self.sym, self.right)


class DB:
    def __init__(self):
        self.tables = {}

    def insert(self, cls, **kw):
        obj = cls(self, **kw)
        self.tables.setdefault(cls.__name__, {})[obj.id] = obj
        return obj

    def get(self, cls, key):
        return self.tables[cls.__name__].get(key)

    def query(self, cls):
        return Query(self, cls)


class Query:
    def __init__(self, db, cls, where=(), order=()):
        self.db, self.cls, self.where, self.order = db, cls, where, order

    def filter(self, expr):
        return Query(self.db, self.cls, self.where + (expr,), self.order)

    def order_by(self, *cols):
        return Query(self.db, self.cls, self.where, cols)

    def __iter__(self):
        rows = [o for o in self.db.tables[self.cls.__name__].values()
                if all(e.ev(o) for e in self.where)]
        for c in reversed(self.order):
            desc = c.startswith('-')
            rows.sort(key=lambda o: getattr(o, c.lstrip('-')), reverse=desc)
        return iter(rows)

    def values(self, *cols):
        return [tuple(getattr(o, c) for c in cols) for o in self]

    def group_count(self, col):
        out = {}
        for o in self:
            k = getattr(o, col)
            out[k] = out.get(k, 0) + 1
        return sorted(out.items(), key=lambda kv: str(kv[0]))


def run(db, i, variant):
    report = []
    tiers = ['basic', 'gold', 'staff_member'] if variant == 0 else ['basic', ' gold ', 7]
    for c in range(30):
        db.insert(Customer, id=i * 1000 + c, name='cust %d' % c,
                  tier=tiers[c % 3], credit=c * 11 if variant == 0 else '%d.5' % c)
    for o in range(120):
        kw = dict(id=i * 10000 + o, customer=i * 1000 + o % 30,
                  total=(o * 37) % 500 if variant == 0 else Decimal(o * 37 % 500) / Decimal('1.7'),
                  status=['new', 'paid', 'shipped', 'refunded'][o % 4], lines=o % 7 + 1)
        if variant == 1 and o % 5 == 0:
            kw['discount'] = o % 30
        db.insert(Order, **kw)
    big = Order.total > 200
    q = db.query(Order).filter(big & ~(Order.status == 'refunded')).order_by('-total', 'id')
    report.append(repr(big & ~(Order.status == 'refunded')))
    report.append(q.values('id', 'total', 'status')[:12])
    report.append(db.query(Order).group_count('status'))
    gold = [o.id for o in db.query(Order).filter(Order.lines * 2 + 1 > 9)
            if o.customer is not None and 'gold' in o.customer.tier]
    report.append(gold[:15])
    report.append([o.customer.get_tier_display() for o in list(db.query(Order))[:9]])
    report.append([c.as_row() for c in list(db.query(Customer).order_by('credit', 'id'))[:4]])
    if variant == 1:
        report.append(db.query(Order).filter(Order.discount > 10).values('id', 'discount')[:10])
    return report


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    added = False
    if variant == 1:
        Order.add_field('discount', DecimalField(Decimal('0')))
        added = True
    try:
        out = []
        for i in range(n):
            db = DB()
            out.append(run(db, i, variant))
        return digest(out, 2)
    finally:
        if added:
            Order.remove_field('discount')
