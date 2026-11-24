# Case maps (punicodeobject.c): lower() and upper() of an exact ASCII str,
# of a view or of another case map, built only when something other than
# len, == or != reads them, and built with their hash for a dict key. The
# identities ascii_upper_or_lower gives (the shared '', a new object even
# for one character or an unchanged string), 'is', non-ASCII text taking
# CPython's own case_operation, str subclasses, errors, and hashes equal
# to CPython's for every length round SipHash's 8-byte blocks.
import sys


class S(str):
    pass


class Low(str):
    def lower(self):
        return 'own:' + str.lower(self)


WORDS = ['abc', 'ABC', 'AbC', '', 'x', 'X', 'Hello, World!', '[@`{~]',
         'a' * 7, 'B' * 8, 'cD' * 9, 'Mixed Case Text Of 24 ch', '0123456789',
         'é', 'ÉCOLE', 'ß', 'ﬁ', 'Σίσυφος', 'ÿ', '日本', '😀A']
TEXTS = ['It was the best of times, it was the WORST of times.',
         '  Leading AND trailing  ', 'one', '', 'Café Crème', 'İstanbul x',
         'ΑΒΓ δεζ', 'mixed é€😀 Widths']


def maps(words):
    out = []
    for w in words:
        a = w.lower()
        b = w.upper()
        out.append((a, b, len(a), len(b), bool(a), a.lower().upper(),
                    b.upper().lower(), w.lower().lower(), w.upper().upper()))
    return out


def identity(words):
    out = []
    for w in words:
        a = w.lower()
        out.append((a is w, a is w.lower(), a is '', w.upper() is w,
                    len(a) == 1 and a is a[0], a == w, a is a))
    k = 'k'
    out.append(('K'.lower() is k, ''.upper() is '', 'abc'.lower() is 'abc'))
    return out


def compares(words):
    out = []
    for w in words:
        a = w.lower()
        out.append((a == 'abc', a != 'abc', 'abc' == a, a == w.lower(),
                    w.upper() == w.lower(), a == S('abc'), a == 'é',
                    a < 'b', a == b'abc', a != 1, a == w.lower().strip()))
    return out


def hashes(words):
    out = []
    for w in words:
        a = w.lower()
        b = w.upper()
        out.append((hash(a) == hash(''.join(list(a))),
                    hash(b) == hash(''.join(list(b)))))
    for n in range(41):
        s = ''.join(chr(65 + (k * 7) % 58) for k in range(n))
        d = {s.lower(): n, s.upper(): -n}
        out.append((n, d[s.lower()], d[s.upper()], s.lower() in d,
                    hash(s.lower()) == hash(str(list(s.lower()) and
                                                ''.join(s.lower())))))
    return out


def as_keys(texts):
    # the fused get then store, d[k], k in d, d.get, sets
    counts = {}
    seen = set()
    for s in texts:
        for w in s.split():
            w = w.strip('.,').lower()
            counts[w] = counts.get(w, 0) + 1
            seen.add(w.upper())
    d = {}
    for s in texts:
        for w in s.split():
            d[w.upper()] = len(w)
            d[w.lower()] += 1 if w.lower() in d else 0
    found = [(w.lower() in counts, counts.get(w.lower()), d.get(w.upper(), -1))
             for s in texts for w in s.split()]
    return sorted(counts.items()), sorted(seen), sorted(d.items()), found


def keys_kept(texts):
    # the key built for the probe is the dict's key object afterwards
    d = {}
    for s in texts:
        for w in s.split():
            k = w.lower()
            d[k] = k
    return [(k is v, hash(k) == hash(''.join(list(k)))) for k, v in d.items()]


def missing(words):
    d = {'abc': 1}
    out = []
    for w in words:
        try:
            out.append(d[w.lower()])
        except KeyError as e:
            out.append(('KeyError', e.args, e.args[0] == w.lower()))
    return out


def subclasses(words):
    out = []
    for w in words:
        s = S(w)
        t = Low(w)
        a = s.lower()
        out.append((type(a).__name__, a, s.upper(), t.lower(), t.upper(),
                    a is s, w.lower() == s.lower(), S(w.lower()) == w.lower()))
    return out


def errors(w):
    out = []
    for f in (lambda: w.lower(1), lambda: w.upper(x=1), lambda: str.lower(1),
              lambda: str.upper(b'a'), lambda: w.lower()[len(w) + 1],
              lambda: {w.lower(): 1}[w.upper() + 'z'],
              lambda: w.lower() + 1):
        try:
            out.append(('ok', f()))
        except Exception as e:
            out.append((type(e).__name__, str(e)))
    return out


def of_views(texts):
    # lower of strip, slice, partition and split items; their roots' kinds
    out = []
    for s in texts:
        for w in s.split():
            out.append((w.lower(), w.strip('.,').upper(), w[1:].lower(),
                        w.partition('t')[2].upper(), w[:3].upper().lower()))
        out.append((s.strip().lower(), s[2:-2].upper()))
    return out


def escapes(s):
    v = s.lower()
    out = [v, v + v, str(v), repr(v), '%s|%r' % (v, v), f'{v}!', [v],
           (v,), {v}, v.encode(), v.split(), tuple(v), v[1:], v.strip('x'),
           v.find('b'), v.startswith('a'), ' '.join([v, v]), v * 2,
           sys.intern(v) == v, [c for c in v]]
    if v:
        out.append((v[0], ord(v[0]), v[-1]))
    return out, v


def frame_locals(s):
    out = []
    for w in s.split():
        t = w.lower()
        loc = sys._getframe().f_locals
        out.append((loc['t'], loc['t'] == t, type(loc['t']).__name__))
    return out


def loop_carried(s):
    # a case map live across a loop's merge point
    v = s.lower()
    out = []
    for i in range(3):
        out.append((len(v), v == 'abc'))
        v = v.upper() if i % 2 == 0 else v.lower()
    return out, v


def returned(s):
    return s.lower(), s.strip().upper(), (s.upper(), len(s.lower()))


def in_try(s):
    v = s.upper()
    try:
        if v == 'BOOM':
            raise ValueError(v)
        return v.lower()
    except ValueError as e:
        return 'caught', e.args, v


def ascii_bytes():
    # every ASCII character's map, alone and inside longer strings
    out = []
    for c in range(128):
        ch = chr(c)
        s = ch * 9 + 'Az'
        out.append((ch.lower(), ch.upper(), s.lower(), s.upper(),
                    {s.lower(): 1}.get(s.lower())))
    return out


class Moves:
    # adding to it takes its key out of the dict and puts it back, so the
    # store of d[k] += x goes to another entry than the get found
    def __init__(self, d, k):
        self.d, self.k = d, k

    def __add__(self, x):
        del self.d[self.k]
        self.d[self.k] = 0
        return x


def store_after_move(n):
    d = {}
    out = []
    for i in range(n):
        k = 'key%d' % (i % 3)
        d['other'] = i
        d[k] = Moves(d, k)
        d[k] += i
        out.append(sorted(d.items()))
    return out


def store_after_move_forms(n):
    # the same for d[k] = d.get(k, 0) + x and d[k] = d[k] + x, whose loads
    # of d and k may be one instruction
    d = {}
    out = []
    for i in range(n):
        k = 'key%d' % (i % 3)
        d['other'] = i
        d[k] = Moves(d, k)
        d[k] = d.get(k, 0) + i
        out.append(sorted(d.items()))
        d[k] = Moves(d, k)
        d[k] = d[k] + i
        out.append(sorted(d.items()))
    return out


CASES = [
    ('store_after_move', [(4,), (7,)], {}),
    ('store_after_move_forms', [(4,), (7,)], {}),
    ('maps', [(WORDS,)], {}),
    ('identity', [(WORDS,)], {}),
    ('compares', [(WORDS,)], {}),
    ('hashes', [(WORDS,)], {}),
    ('as_keys', [(TEXTS,)], {}),
    ('keys_kept', [(TEXTS,)], {}),
    ('missing', [(WORDS,)], {}),
    ('subclasses', [(WORDS,)], {}),
    ('errors', [('Ab',), ('',), ('é',)], {}),
    ('of_views', [(TEXTS,)], {}),
    ('escapes', [('AbC',), ('',), ('X',), ('Émile',)], {}),
    ('frame_locals', [('A b. C',)], {}),
    ('loop_carried', [('aBc',), ('',), ('é',)], {}),
    ('returned', [('Ab',), ('',), (' x ',), ('ß',)], {}),
    ('in_try', [('boom',), ('fine',)], {}),
    ('ascii_bytes', [()], {}),
]
