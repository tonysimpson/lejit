"""Symbolic algebra simplifier whose __eq__, __hash__, __lt__, __bool__ and __repr__ run user code that re-enters the simplifier and mutates the memo dict and lists being searched or sorted: side effects inside operations a compiler assumed pure (Gross, Exploiting Logic Bugs in JavaScript JIT Engines, Phrack 70)."""
import reprlib
import zlib

N = 1000


class Ctx:
    def __init__(self):
        self.memo = {}
        self.events = []
        self.forced = 0


CTX = [None]


class Expr:
    __slots__ = ('_h',)

    def __hash__(self):
        h = self._h
        if h is None:
            h = self._h = hash(self.key())
        return h

    def __eq__(self, other):
        return isinstance(other, Expr) and self.key() == other.key()

    def __lt__(self, other):
        return self.key() < other.key()

    def __add__(self, other):
        return Add(self, wrap(other))

    def __mul__(self, other):
        return Mul(self, wrap(other))


def wrap(x):
    return x if isinstance(x, Expr) else Num(x)


class Num(Expr):
    __slots__ = ('v',)

    def __init__(self, v):
        self.v, self._h = v, None

    def key(self):
        return (0, str(type(self.v).__name__), self.v)

    def __repr__(self):
        return repr(self.v)

    def __bool__(self):
        return self.v != 0


class Var(Expr):
    __slots__ = ('name',)

    def __init__(self, name):
        self.name, self._h = name, None

    def key(self):
        return (1, self.name)

    def __repr__(self):
        return self.name


class Bin(Expr):
    __slots__ = ('a', 'b')
    op = '?'

    def __init__(self, a, b):
        self.a, self.b, self._h = a, b, None

    def key(self):
        return (2, self.op, self.a.key(), self.b.key())

    @reprlib.recursive_repr()
    def __repr__(self):
        return '(%r %s %r)' % (self.a, self.op, self.b)


class Add(Bin):
    __slots__ = ()
    op = '+'


class Mul(Bin):
    __slots__ = ()
    op = '*'


class Lazy(Expr):
    __slots__ = ('thunk', 'value')

    def __init__(self, thunk):
        self.thunk, self.value, self._h = thunk, None, None

    def force(self):
        if self.value is None:
            ctx = CTX[0]
            ctx.forced += 1
            self.value = simplify(self.thunk())
            ctx.memo[('forced', ctx.forced)] = self.value
            ctx.events.append('forced %d -> %r' % (ctx.forced, self.value))
        return self.value

    def key(self):
        return self.force().key()

    def __repr__(self):
        return 'lazy<%r>' % (self.force(),)


class Cell(Expr):
    __slots__ = ('items',)

    def __init__(self):
        self.items, self._h = [], None

    def key(self):
        return (3, len(self.items))

    @reprlib.recursive_repr('<cycle>')
    def __repr__(self):
        return 'cell%r' % (self.items,)


def simplify(e):
    ctx = CTX[0]
    if e in ctx.memo:
        return ctx.memo[e]
    r = e
    if isinstance(e, Lazy):
        r = e.force()
    elif isinstance(e, Bin):
        a, b = simplify(e.a), simplify(e.b)
        if isinstance(a, Num) and isinstance(b, Num):
            r = Num(a.v + b.v if e.op == '+' else a.v * b.v)
        elif e.op == '+' and isinstance(a, Num) and not a:
            r = b
        elif e.op == '+' and isinstance(b, Num) and not b:
            r = a
        elif e.op == '*' and ((isinstance(a, Num) and not a) or (isinstance(b, Num) and not b)):
            r = Num(0)
        elif e.op == '*' and isinstance(a, Num) and a.v == 1:
            r = b
        elif e.op == '+' and a == b:
            r = Mul(Num(2), a)
        else:
            r = type(e)(*sorted([a, b])) if e.op in '+*' else type(e)(a, b)
    ctx.memo[e] = r
    return r


class Meddler:
    def __init__(self, target, v):
        self.target, self.v = target, v

    def __lt__(self, other):
        if len(self.target) < 40:
            self.target.append(Meddler(self.target, self.v + 100))
        ov = other.v if isinstance(other, Meddler) else 0
        return self.v < ov


def build(k, variant):
    x, y, z = Var('x'), Var('y'), Var('z')
    terms = [x * k + y * 0, (x + Num(0)) * Num(1), x + x, y * z + z * y, Num(k) + Num(2) * Num(3)]
    if variant == 1:
        terms.append(Lazy(lambda: (x + y) * Num(k % 3)))
        terms.append(Lazy(lambda: Num(k) * Num(0.5)) + Num(1))
        terms.append(Lazy(lambda: Lazy(lambda: z + z)) * x)
    e = terms[0]
    for t in terms[1:]:
        e = e + t
    return e, terms


def run(k, variant):
    ctx = CTX[0] = Ctx()
    e, terms = build(k, variant)
    s = simplify(e)
    parts = [repr(simplify(t)) for t in terms]
    ordered = sorted(set(simplify(t) for t in terms))
    victims = [Meddler(None, j) for j in range(6)]
    for m in victims:
        m.target = victims
    try:
        victims.sort()
        sort_result = 'sorted'
    except ValueError as err:
        sort_result = str(err)
    c = Cell()
    c.items.append(c)
    c.items.append(Num(k))
    if variant == 1:
        c.items.append(Lazy(lambda: Var('w') + Num(k)))
    cell_repr = repr(c)
    truth = [bool(Num(j - 2)) for j in range(4)]
    return repr(s), parts, [repr(o) for o in ordered], sort_result, len(victims), cell_repr, \
        truth, ctx.events, len(ctx.memo), ctx.forced


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    try:
        return digest([run(k, variant) for k in range(n * 5)], 3)
    finally:
        CTX[0] = None
