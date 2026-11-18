"""Service layer reached through transparent proxies: lazy-initialising proxies with __getattr__, recording mocks, and access-checking wrappers overriding __getattribute__ and __setattr__, stacked to different depths per variant over different targets: every attribute access running user code instead of a cached lookup (V8 blog, Optimizing ES2015 Proxies in V8, 2017; Hoelzle, Chambers and Ungar, ECOOP 1991)."""
import zlib

N = 900


class Lazy:
    __slots__ = ('_factory', '_target')

    def __init__(self, factory):
        object.__setattr__(self, '_factory', factory)
        object.__setattr__(self, '_target', None)

    def _get(self):
        t = object.__getattribute__(self, '_target')
        if t is None:
            t = object.__getattribute__(self, '_factory')()
            object.__setattr__(self, '_target', t)
        return t

    def __getattr__(self, name):
        return getattr(self._get(), name)

    def __setattr__(self, name, value):
        setattr(self._get(), name, value)

    def __len__(self):
        return len(self._get())


class Recorder:
    def __init__(self, target, log):
        self.__dict__['_t'] = target
        self.__dict__['_log'] = log

    def __getattr__(self, name):
        value = getattr(self._t, name)
        self._log.append(name)
        if callable(value):
            def call(*a, **k):
                r = value(*a, **k)
                self._log.append('%s->%s' % (name, type(r).__name__))
                return r
            return call
        return value

    def __setattr__(self, name, value):
        self._log.append('set ' + name)
        setattr(self._t, name, value)


class Guard:
    def __init__(self, target, denied):
        object.__setattr__(self, '_t', target)
        object.__setattr__(self, '_denied', denied)

    def __getattribute__(self, name):
        if name in object.__getattribute__(self, '_denied'):
            raise PermissionError(name)
        return getattr(object.__getattribute__(self, '_t'), name)

    def __setattr__(self, name, value):
        if name.startswith('_'):
            raise PermissionError('set ' + name)
        setattr(object.__getattribute__(self, '_t'), name, value)


class Account:
    def __init__(self, owner, balance):
        self.owner = owner
        self.balance = balance
        self.history = []

    def deposit(self, amount):
        self.balance += amount
        self.history.append(amount)
        return self.balance

    def withdraw(self, amount):
        if amount > self.balance:
            raise ValueError('insufficient')
        self.balance -= amount
        self.history.append(-amount)
        return self.balance

    def __len__(self):
        return len(self.history)


class Ledger(dict):
    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name) from None

    def deposit(self, amount):
        self['balance'] = self.get('balance', 0) + amount
        return self['balance']

    def withdraw(self, amount):
        if amount > self.get('balance', 0):
            raise ValueError('insufficient')
        self['balance'] -= amount
        return self['balance']


def wrap(k, j, log, variant):
    if variant == 0:
        return Lazy(lambda: Account('o%d' % j, 100 + j))
    target_kind = (k + j) % 3
    if target_kind == 0:
        base = Account('o%d' % j, 100 + j)
    elif target_kind == 1:
        base = Ledger(owner='o%d' % j, balance=100 + j)
    else:
        base = Lazy(lambda: Account('o%d' % j, 100 + j))
    layers = (k + 2 * j) % 4
    obj = base
    for d in range(layers):
        obj = [lambda o: Recorder(o, log), lambda o: Guard(o, ('history',)),
               lambda o: Lazy(lambda: o)][(d + j) % 3](obj)
    return obj


def run(k, variant):
    log = []
    accounts = [wrap(k, j, log, variant) for j in range(12)]
    results = []
    for step in range(150):
        a = accounts[(step * 5 + k) % len(accounts)]
        try:
            if step % 3 == 2:
                r = a.withdraw((step * 7) % 90)
            else:
                r = a.deposit(step % 23)
            results.append(r)
        except ValueError as e:
            results.append(str(e))
        if step % 25 == 24:
            try:
                results.append(('owner', a.owner, a.balance))
                a.balance = a.balance + 1
                results.append(('hist', len(a.history)))
            except (PermissionError, AttributeError) as e:
                results.append(('denied', type(e).__name__, str(e)))
    return results, log[-30:], len(log)


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    return digest([run(k, variant) for k in range(n * 3)], 3)
