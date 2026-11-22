# d[k], k in d, d.get(k[, x]) and d[k] = v on exact dicts with str keys
# (pdictobject.c's exact-str probe), and the get then store of one key:
# str subclass keys and keys with their own __hash__ and __eq__ in the same
# dict, hash collisions, deleted keys, hashes not yet computed, split
# instance dicts, missing keys and their KeyError, a dict resized or
# mutated by an operator or by a __del__ between the get and the store, GC
# tracking, and dict subclasses.
import gc


class S(str):
    pass


class LoudS(str):
    # equal to a str, with its own hash and eq: the dict leaves lookdict_unicode
    def __hash__(self):
        return str.__hash__(self)

    def __eq__(self, other):
        return str.__eq__(self, other)


class Collide:
    # the hash of a str key, equal to nothing else
    def __init__(self, s):
        self.s = s

    def __hash__(self):
        return hash(self.s)

    def __eq__(self, other):
        return self is other

    def __repr__(self):
        return 'Collide(%r)' % self.s


class D(dict):
    pass


class Missing(dict):
    def __missing__(self, key):
        return 'missing:' + key


class Obj:
    pass


def err(f, *a):
    try:
        return f(*a)
    except Exception as e:
        return type(e).__name__, str(e)


def getitem(d, k):
    return d[k]


def fresh(s):
    # a str whose hash is not computed yet
    return ''.join(list(s))


def lookups(d, keys):
    out = []
    for k in keys:
        out.append((err(getitem, d, k), k in d, k not in d, d.get(k),
                    d.get(k, 'dflt')))
    return out


def word_counts(words):
    d = {}
    e = {}
    for w in words:
        d[w] = d.get(w, 0) + 1
        if w in e:
            e[w] = e[w] + 1
        else:
            e[w] = 1
    return sorted(d.items()), sorted(e.items())


def str_keys(n):
    d = {'a': 1, 'b': 2, 'é': 3, '€uro': 4}
    out = [lookups(d, ['a', 'b', 'c', 'é', '€uro', fresh('a'), fresh('zz')])]
    for i in range(n):
        d[fresh('k%d' % i)] = i
        d['a'] = d['a'] + i
        out.append((d['a'], d.get(fresh('k%d' % i)), fresh('k0') in d))
    del d['b']  # lookdict_unicode from now on
    out.append(lookups(d, ['a', 'b', 'k1', fresh('k1')]))
    d['b'] = 'back'
    out.append(sorted(d.items()))
    return out


def mixed_keys(n):
    d = {'a': 1, 'b': 2}
    d[S('c')] = 3
    d[LoudS('d')] = 4
    d[1] = 'one'
    out = [lookups(d, ['a', 'c', S('c'), 'd', LoudS('d'), S('x'), 1, 2])]
    e = {'a': 1}
    e[Collide('a')] = 'collide'
    e[Collide('b')] = 'collide b'
    for i in range(n):
        e['a'] = e.get('a', 0) + 1
        e['b'] = e.get('b', 0) + 1
        out.append((e['a'], e['b'], len(e), 'a' in e))
    out.append(lookups(e, ['a', 'b', 'c']))
    f = {S('s'): 1}
    for i in range(n):
        f['s'] = f.get('s', 0) + 1
        f[S('s')] = f[S('s')] + 1
    out.append(sorted((type(k).__name__, k, v) for k, v in f.items()))
    return out


def subclass_keys(n):
    out = []
    for i in range(n):
        d = {'a': 1}
        k = S('a') if i % 2 else 'a'
        d[k] = d.get(k, 0) + 1
        d[k] = d[k] * 10
        out.append((sorted(d.items()), [type(x).__name__ for x in d]))
    return out


def missing(n):
    d = {'a': 1}
    out = []
    for i in range(n):
        out.append(err(getitem, d, 'b'))
        out.append(err(getitem, d, fresh('long missing key %d' % i)))
        out.append(err(getitem, d, ('t',)))
        out.append(d.get('b'))
    m = Missing(a=1)
    s = D(a=1)
    for i in range(n):
        m['b'] = m['b'] + '!'
        s['a'] = s.get('a', 0) + 1
        out.append((m['a'], m['c'], 'c' in m, sorted(m.items()), s['a']))
    out.append(err(getitem, s, 'zz'))
    return out


def instance_dicts(n):
    out = []
    a, b = Obj(), Obj()
    a.x = b.x = 0
    a.y = b.y = 'y'
    for i in range(n):
        d = a.__dict__
        d['x'] = d.get('x', 0) + 1
        d['x'] = d['x'] + 1
        k = 'x'
        d[k] = d.get(k, 0) + 1
        d[k] += 1
        b.__dict__['z'] = i
        out.append((a.x, 'z' in d, sorted(b.__dict__.items())))
    return out


class Grow:
    # its + resizes the dict, or removes the key, before the store
    def __init__(self, d, mode):
        self.d, self.mode = d, mode

    def __add__(self, other):
        if self.mode == 'grow':
            for i in range(100):
                self.d['g%d' % i] = i
        elif self.mode == 'del':
            self.d.pop('k', None)
        elif self.mode == 'clear':
            self.d.clear()
        elif self.mode == 'replace':
            self.d['k'] = 'replaced'
        return Grow(self.d, self.mode)


def mutated_between(n):
    out = []
    for mode in ['grow', 'del', 'clear', 'replace', 'none']:
        for i in range(n):
            d = {'k': None, 'j': 1}
            k = fresh('k')
            d[k] = Grow(d, mode)
            d[k] = d.get(k, 0) + 1
            out.append((mode, len(d), sorted(d)[:3], type(d.get(k)).__name__))
            d[k] = Grow(d, mode)
            d[k] = d[k] + 1
            out.append((mode, len(d), sorted(d)[:3], type(d.get(k)).__name__))
            d[k] = Grow(d, mode)
            d[k] += 1
            out.append((mode, len(d), sorted(d)[:3], type(d.get(k)).__name__))
    return out


log = []


class Del:
    # its __del__, run by the store's decref of the old value, mutates d
    def __init__(self, d, mode):
        self.d, self.mode = d, mode

    def __add__(self, other):
        return other

    def __del__(self):
        log.append(self.mode)
        if self.mode == 'grow':
            for i in range(100):
                self.d['h%d' % i] = i
        elif self.mode == 'del':
            self.d.pop('k', None)
        elif self.mode == 'store':
            self.d['k'] = 'from del'


def del_mutates(n):
    out = []
    for mode in ['grow', 'del', 'store']:
        for i in range(n):
            del log[:]
            d = {'k': 0}
            k = 'k'
            d[k] = Del(d, mode)
            d[k] = d.get(k, 0) + i
            out.append((mode, list(log), d.get(k), len(d)))
            d[k] = Del(d, mode)
            d[k] = d[k] + i
            out.append((mode, list(log), d.get(k), len(d)))
            d[k] = Del(d, mode)
            d[k] += i
            out.append((mode, list(log), d.get(k), len(d)))
    return out


def tracking(n):
    out = []
    for i in range(n):
        d = {'a': 1, 'b': 'x'}
        a, b, y = 'a', 'b', [i]
        before = gc.is_tracked(d)
        d[a] = d.get(a, 0) + 1
        mid = gc.is_tracked(d)
        d[b] = d[b] * 0 + y
        e = {'b': (1,)}
        e[b] += (i,)
        out.append((before, mid, gc.is_tracked(d), d, gc.is_tracked(e), e))
    return out


def add_str(d, k):
    d[k] = d[k] + 'x'


def get_add_str(d, k):
    d[k] = d.get(k) + 'x'


def inplace(words):
    d = dict.fromkeys(words, 0)
    m = {}
    for w in words:
        d[w] += 1
        d[w] = d[w] * 2
        k = w + '!'
        m[k] = m.get(k, '') + w
    del d[words[0]]
    for w in words:
        d[w] = d.get(w, 100) - 1
    return sorted(d.items()), sorted(m.items())


def collisions(n):
    d = {}
    keys = ['k%d' % i for i in range(40)]
    for r in range(n):
        for k in keys:
            d[k] = d.get(k, 0) + 1   # grows through resizes on the first round
        for k in keys[::3]:
            d[k] += 1
        if r == 0:
            d[Collide(keys[0])] = 'c'   # the dict leaves lookdict_unicode
    return sorted((repr(k), v) for k, v in d.items())


def subclass_local(n):
    out = []
    d = {}
    for i in range(n):
        k = S('s') if i % 2 else 's'
        d[k] = d.get(k, 0) + 1
        d[k] += 1
        j = LoudS('t')
        d[j] = d.get(j, 0) + 1
        out.append(sorted((type(x).__name__, x, v) for x, v in d.items()))
    return out


def errors(n):
    out = []
    for i in range(n):
        d = {'a': 1}
        out.append(err(add_str, d, 'a'))
        out.append(err(add_str, d, 'zz'))
        out.append(err(get_add_str, d, 'a'))
        out.append(err(lambda: d.get('a', 0) + 'x'))
        out.append(err(getitem, d, []))
        out.append(err(lambda: [] in d))
        out.append(err(lambda: d.__setitem__([], 1)))
        out.append(sorted(d.items()))
    return out


CASES = [
    ('word_counts', [('the cat and the hat and the bat sat'.split() * 3,)], {}),
    ('str_keys', [(5,)], {}),
    ('mixed_keys', [(4,)], {}),
    ('subclass_keys', [(4,)], {}),
    ('missing', [(3,)], {}),
    ('instance_dicts', [(4,)], {}),
    ('mutated_between', [(2,)], {}),
    ('del_mutates', [(2,)], {}),
    ('tracking', [(3,)], {}),
    ('errors', [(2,)], {}),
    ('inplace', [('a b c a b a'.split(),)], {}),
    ('collisions', [(3,)], {}),
    ('subclass_local', [(4,)], {}),
]
