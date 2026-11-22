# Dict probes with a key not built (pdictobject.c): d[k], k in d,
# d.get(k[, x]) and d[k] = v where k is a view (a slice, strip or split
# item) or a case map (lower, upper) probe with its characters, and the
# key is built only where it must exist. The key a store inserts, a
# KeyError's key and a key an __eq__ is given are the str the variable
# holds; a store over a key found keeps the dict's own key. Hashes equal
# to CPython's at every length, the empty key, many keys and deleted ones,
# str subclass keys and keys with their own __eq__, non-ASCII roots and
# keys, case maps too long for the probe's buffer, instance dicts, dicts
# mutated by an __eq__, an operator or a __del__ between a get and its
# store, and errors.

TEXT = ('It was the best of times, it was the worst of times, it was the age '
        'of wisdom, it was the age of foolishness, it was the epoch of belief.')


class S(str):
    pass


class Eq:
    # equal to the str it holds, with its hash; records what it was
    # compared with, and may mutate the dict while compared
    def __init__(self, s, d=None, mode=None):
        self.s, self.d, self.mode, self.seen = s, d, mode, []

    def __hash__(self):
        return hash(self.s)

    def __eq__(self, other):
        self.seen.append(other)
        if self.mode == 'clear':
            self.d.clear()
        elif self.mode == 'grow':
            for i in range(50):
                self.d['g%d' % i] = i
        elif self.mode == 'del':
            self.d.pop('x', None)
        return self.s == other

    def __repr__(self):
        return 'Eq(%r)' % self.s


class Obj:
    pass


def err(f, *a):
    try:
        return f(*a)
    except Exception as e:
        return type(e).__name__, e.args


def word_counts(text, n):
    counts, views = {}, {}
    for r in range(n):
        for word in text.split():
            v = word.strip('.,;')
            word = v.lower()
            counts[word] = counts.get(word, 0) + 1
            views[v] = views.get(v, 0) + 1
            if v in views:
                views[v] += 1
            views[v] = views[v] * 1
    return sorted(counts.items()), sorted(views.items())


def identity(text):
    out = []
    d = {}
    for w in text.split():
        v = w.strip('.,')
        d[v] = d.get(v, 0) + 1
        key = [x for x in d if x == v][0]
        out.append((key is v, v is w, d[v], v in d))
    for w in text.split():
        c = w.upper()
        d[c] = 1
        out.append([x for x in d if x == c][0] is c)
    e = {'abc': 0}
    orig = next(iter(e))
    k = 'xabc'[1:]
    e[k] = e.get(k, 0) + 1
    e[k] += 1
    out.append((next(iter(e)) is orig, k is orig, k == orig, e))
    m = {'ABC': 0}
    orig = next(iter(m))
    c = 'abc'.upper()
    m[c] = m[c] + 1
    out.append((next(iter(m)) is orig, c is orig, m))
    s = 'whole'
    f = {}
    f[s.strip()] = 1
    out.append(next(iter(f)) is s)
    return out


def hashes(n):
    out = []
    base = ''.join(chr(65 + (k * 7) % 58) for k in range(n + 3))
    for i in range(n + 1):
        v = base[1:1 + i]
        c = v.lower()
        d = {''.join(list(v)): 'v', ''.join(list(c)): 'c'}
        out.append((i, d.get(v), d.get(c), v in d, c in d, d[v], d[c],
                    hash(v) == hash(''.join(list(v))),
                    hash(c) == hash(''.join(list(c)))))
    e = {'': 'empty'}
    out.append((e.get('abc'[1:1]), ''.upper() in e, e['x'.strip('x')]))
    return out


def many_keys(n):
    d = {'k%d' % i: i for i in range(n)}
    t = ' '.join('k%d' % i for i in range(n + 20))
    out = []
    for w in t.split():
        out.append((d.get(w), w in d, w.upper().lower() in d))
    for i in range(0, n, 2):
        del d['k%d' % i]   # lookdict_unicode from here, with dummies
    for w in t.split():
        d[w] = d.get(w, -1) + 1
        out.append((d[w], w in d))
    return out, sorted(d.items())


def subclass_keys(text):
    out = []
    for w in text.split()[:6]:
        v = w.strip('.,')
        d = {S(v): 'sub'}
        out.append((d.get(v), v in d, err(lambda: d[v]),
                    [type(x).__name__ for x in d]))
        d[v] = d.get(v, '') + '!'
        out.append([(type(x).__name__, x, y) for x, y in d.items()])
        e = Eq(v.upper())
        f = {e: 'eq', 'other': 1}
        c = v.upper()
        out.append((f.get(c), c in f, f[c]))
        out.append(all(x is c for x in e.seen) and len(e.seen) == 3)
        f[c] = 'stored'
        out.append((sorted(map(repr, f)), e.seen[-1] is c))
    return out


def non_ascii(n):
    roots = ['é abc def', 'abc é def', 'Ω abc', '😀 abc', 'abc def']
    out = []
    for r in roots:
        d = {'abc': 1, 'é': 2, 'Ω': 3, '😀': 4, 'ABC': 5, 'def': 6}
        for i in range(n):
            for w in r.split():
                d[w] = d.get(w, 0) + 1
                c = w.upper()
                out.append((w, d[w], c in d, d.get(c), w.lower() in d))
        out.append(sorted(d.items()))
    return out


def long_maps(n):
    out = []
    for size in (1, 127, 128, 129, 300):
        s = ('aB' * size)[:size]
        d = {s.lower(): 'l', s.upper(): 'u'}
        for i in range(n):
            out.append((size, d.get(s.lower()), s.upper() in d,
                        d[(' ' + s + ' ').strip().lower()]))
            c = s.lower()
            d[c] = d[c] + '!'
            k = (s + 'x').upper()
            d[k] = 1
            out.append((len(d), [x for x in d if x == k][0] is k))
    return out


def instance_dicts(text):
    a = Obj()
    for w in text.split()[:5]:
        setattr(a, w.strip('.,').lower(), 0)
    d = a.__dict__
    out = []
    for w in text.split():
        v = w.strip('.,').lower()
        d[v] = d.get(v, 0) + 1
        out.append((d[v], v in d))
    return out, sorted(d.items())


def eq_mutates(n):
    out = []
    for mode in ('clear', 'grow', 'del', None):
        for i in range(n):
            d = {'x': 1}
            e = Eq('ab', d, mode)
            d[e] = 'eq'
            k = 'xab'[1:]
            out.append((mode, d.get(k), err(lambda: d[k]), k in d,
                        sorted(map(repr, d))[:3]))
            d[k] = 'v'
            out.append((mode, len(d), d.get(k), all(x is k for x in e.seen)))
    return out


class Grow:
    # its + mutates the dict between a get and its store
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
        elif self.mode == 'other':
            self.d.get('kk'[1:])     # another probe of the same root
        return Grow(self.d, self.mode)


def mutated_between(n):
    out = []
    for mode in ('grow', 'del', 'clear', 'replace', 'other', None):
        for i in range(n):
            d = {'k': None, 'j': 1}
            k = 'xk'[1:]
            d[k] = Grow(d, mode)
            d[k] = d.get(k, 0) + 1
            out.append((mode, len(d), sorted(d)[:3], type(d.get(k)).__name__))
            c = 'K'.lower()
            d[c] = Grow(d, mode)
            d[c] = d[c] + 1
            out.append((mode, len(d), sorted(d)[:3], type(d.get(c)).__name__))
            d[c] = Grow(d, mode)
            d[c] += 1
            out.append((mode, len(d), [x for x in d if x == 'k'][:1]))
    return out


log = []


class Del:
    # its __del__, run by the store's release of the old value
    def __init__(self, d):
        self.d = d

    def __add__(self, other):
        return other

    def __del__(self):
        log.append(len(self.d))
        self.d['from del'] = 1


def del_runs(n):
    out = []
    for i in range(n):
        del log[:]
        d = {'k': 0}
        k = ' k '.strip()
        d[k] = Del(d)
        d[k] = d.get(k, 0) + i
        out.append((list(log), sorted(d.items())))
    return out


def getitem(d, k):
    return d[k]


def delitem(d, k):
    del d[k]


def errors(text):
    out = []
    d = {'it': 1}
    for w in text.split()[:6]:
        v = w.strip('.,')
        c = v.upper()
        out.append((err(getitem, d, v), err(getitem, d, c),
                    err(delitem, d, c), err(lambda: d.get(v, 1, 2)),
                    err(lambda: d.get()), err(getitem, [1], v),
                    err(getitem, {}, c[1:])))
        try:
            d[c]
        except KeyError as e:
            out.append(e.args[0] is c)
    return out


CASES = [
    ('word_counts', [(TEXT, 3)], {}),
    ('identity', [(TEXT,)], {}),
    ('hashes', [(40,)], {}),
    ('many_keys', [(200,)], {}),
    ('subclass_keys', [(TEXT,)], {}),
    ('non_ascii', [(2,)], {}),
    ('long_maps', [(2,)], {}),
    ('instance_dicts', [(TEXT,)], {}),
    ('eq_mutates', [(2,)], {}),
    ('mutated_between', [(2,)], {}),
    ('del_runs', [(3,)], {}),
    ('errors', [(TEXT,)], {}),
]
