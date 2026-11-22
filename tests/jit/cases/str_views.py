# Views (punicodeobject.c): slices, strip, lstrip, rstrip, partition and
# rpartition of an exact str, and the items of a for loop over s.split(),
# which are made only when something other than len, ==, != or 'in' reads
# them. The identities CPython's PyUnicode_Substring gives (the string
# itself when nothing is taken off, the shared '' and cached characters),
# 'is' between a view and what it came from, kinds other than ASCII, str
# subclasses, errors, and the list and iterator made when they escape.
# An index into a view and split of a view read the view's root.
import sys


class S(str):
    pass


class Eq(str):
    # its own ==, which compares with a view as with any str
    def __eq__(self, other):
        return str.__eq__(self, other.upper())

    __hash__ = str.__hash__


TEXTS = ['It was the best of times, it was the worst of times.',
         '  leading and trailing  ', 'one', '', '   ', 'a\tb\nc\x1cd',
         'café crème brûlée', 'x\xa0y\x85z', 'Ωμέγα  λ', '日本 語　テキスト',
         'emoji 😀 and 😀😀 ', 'mixed é€😀 widths']
SEPS = [',', ' ', 'é', 'ab', '€', '😀', 'xyz']


def split_items(texts):
    out = []
    for s in texts:
        for w in s.split():
            out.append((w, len(w), w == 'was', w != 'one', 'e' in w, w[1:],
                        w[:-1], w[::-1]))
    return out


def split_sep(texts, seps):
    out = []
    for s in texts:
        for sep in seps:
            row = []
            for w in s.split(sep):
                row.append(w)
            out.append(row)
    return out


def split_const_sep(s):
    a = [w for w in s.split(',')]
    b = []
    for w in s.split('ab'):
        b.append(len(w))
    c = []
    for w in s.split(None):
        c.append(w)
    return a, b, c


def split_identity(s):
    # one item holding no separator is s itself; short items are cached
    out = []
    for w in s.split():
        out.append((w is s, len(w) == 1 and w is w[0], w is ''))
    for w in s.split(','):
        out.append((w is s, w is ''))
    return out


def split_items_kept(s):
    # each item forced once: the same object every time it is read
    kept = []
    for w in s.split():
        kept.append(w)
        kept.append(w)
    return [a is b for a, b in zip(kept[::2], kept[1::2])], kept


def split_list_escapes(s):
    words = s.split()
    n = 0
    for w in words:
        n += len(w)
    words.append('extra')
    return n, words, len(s.split()), s.split()[0] if s.split() else None


def split_list_mutated(s):
    # iterating a list that is also a local: the real list, as CPython's
    words = s.split()
    out = []
    for w in words:
        out.append(w)
        if len(words) < 8:
            words.append(w.upper())
    return out


def split_break(s):
    for w in s.split():
        if w == 'the':
            break
    else:
        w = None
    return w


def split_nested(a, b):
    out = []
    for x in a.split():
        for y in b.split(','):
            if x[0] == y[:1]:
                out.append(x + y)
    return out


def split_receivers(s):
    out = []
    for w in S(s).split():          # a subclass: CPython's split
        out.append((type(w).__name__, w))
    for w in s.strip().split():     # a view: forced first
        out.append(w)
    for w in (s + ' tail').split(' '):
        out.append(w)
    return out


def split_errors(s):
    out = []
    for sep in ['', 1, None, b',']:
        try:
            out.append([w for w in s.split(sep)])
        except (TypeError, ValueError) as e:
            out.append((type(e).__name__, str(e)))
    try:
        for w in s.split(','):
            out.append(w)
            if w == 'b':
                raise KeyError(w)
    except KeyError as e:
        out.append(('KeyError', e.args))
    return out


def split_maxsplit(s):
    return [w for w in s.split(None, 1)], [w for w in s.split(',', 2)]


def split_generator(s):
    return sum(len(w) for w in s.split()), list(w for w in s.split(','))


def strips(texts):
    out = []
    for s in texts:
        out.append((s.strip(), s.lstrip(), s.rstrip(), s.strip(None),
                    s.strip('. '), s.lstrip('Ia'), s.rstrip('s.'),
                    s.strip('é'), s.strip('😀 ')))
    return out


def strip_identity(texts):
    # nothing stripped: the string itself, as Substring's unchanged rule
    out = []
    for s in texts:
        t = s.strip()
        u = s.strip('#')
        out.append((t is s, u is s, s.lstrip() is s, t.strip() is t,
                    t == s, len(t)))
    return out


def strip_of_split(s):
    out = []
    for w in s.split():
        t = w.strip('.,;')
        u = t.lower()
        out.append((t is w, w.strip('.,;') == t, u, w, t))
    return out


def strip_of_split_unused_parent(s):
    # the parent view held only by the child
    out = []
    for w in s.split():
        out.append(w.strip('.,;'))
    return out


def strip_chain(s):
    a = s.strip()
    b = a.strip('-')
    c = b.strip('ab')
    d = c[1:]
    return (a, b, c, d, b is a, c is b, d is c, a is s, len(d),
            d == c[1:], d.strip())


def strip_subclass(s):
    t = S(s)
    a = t.strip()
    b = t.strip('x')
    return type(a).__name__, a, type(b).__name__, b, a is t


def strip_errors(s):
    out = []
    for chars in [1, b'x', ['a']]:
        try:
            out.append(s.strip(chars))
        except TypeError as e:
            out.append(str(e))
    try:
        out.append(s.strip('a', 'b'))
    except TypeError as e:
        out.append(str(e))
    out.append(s.strip(S('ab')))
    return out


def slices(texts):
    out = []
    for s in texts:
        a = s[1:]
        b = s[:3]
        c = s[2:-2]
        d = a[1:4]
        out.append((a, b, c, d, s[5:2], s[-100:100], len(c), a == s[1:],
                    d.strip(), 'a' in c))
    return out


def slice_identity(s):
    a = s[:]
    b = s[0:len(s)]
    c = s[0:0]
    d = s[1:2]
    e = a[:]
    return a is s, b is s, c is '', len(s) > 1 and d is s[1], e is a


def slice_bounds(s, i, j):
    v = s.strip()
    return v[i:j], s[i:j], v[i:j] == s.strip()[i:j], len(v[i:j])


def slice_subclass(s):
    t = S(s)
    return type(t[1:]).__name__, t[1:], t[:] is t


def partitions(texts, seps):
    out = []
    for s in texts:
        for sep in seps:
            out.append((s.partition(sep), s.rpartition(sep)))
    return out


def partition_identity(s):
    a, b, c = s.partition('=')
    x, y, z = s.rpartition('=')
    return (a is s, b is '', c is '', z is s, x is '', b, y,
            len(a), len(z), a == x, c.strip())


def partition_of_view(s):
    line = s.strip()
    k, eq, v = line.partition('=')
    return k.strip(), v.strip(), k is line, eq, len(v)


def partition_errors(s):
    out = []
    for sep in ['', 1, None]:
        try:
            out.append(s.partition(sep))
        except (TypeError, ValueError) as e:
            out.append((type(e).__name__, str(e)))
    out.append(S(s).partition('='))
    out.append(s.partition(S('=')))
    return out


def compares(texts):
    out = []
    for s in texts:
        v = s.strip()
        out.append((v == s.strip(), v == s, v != 'one', v == Eq('ONE'),
                    v < 'm', v >= s, v[1:] == v[1:], v == 1, v != None,
                    s.strip() is v, v.strip() is v))
    return out


def contains(texts):
    out = []
    for s in texts:
        v = s.strip()
        out.append(('a' in v, '' in v, 'é' in v, '😀' in v, v in s,
                    S('t') in v, v[2:] in v))
    try:
        out.append(1 in 'abc'.strip())
    except TypeError as e:
        out.append(str(e))
    return out


def as_keys(texts):
    # views as dict keys and set members, hashed when forced
    d = {}
    for s in texts:
        for w in s.split():
            d[w.strip('.,')] = d.get(w.strip('.,'), 0) + 1
    return sorted(d.items()), {w for w in ' '.join(texts).split()} == set(
        ' '.join(texts).split())


def kinds(texts):
    out = []
    for s in texts:
        for w in s.split():
            t = w[:2]
            out.append((t, max(map(ord, t)) if t else -1, t.isascii(),
                        sys.getsizeof(t) == sys.getsizeof(str(t))))
    return out


def frame_locals(s):
    out = []
    for w in s.split():
        t = w.strip('.')
        loc = sys._getframe().f_locals
        out.append((loc['w'], loc['t'], loc['t'] == t))
    return out


def loop_carried(s):
    # a view of a view each turn: nested until the weights force one
    v = s.strip()
    out = []
    while v:
        out.append((len(v), v[:1]))
        v = v[1:].strip()
    i = 0
    w = s
    for i in range(6):
        w = w.lstrip(w[:1])
    return out, w, i


def ident(x):
    return x


def escapes(s):
    v = s.strip('<>')
    out = [ident(v), v + v, v * 2, str(v), repr(v), '%s|%r' % (v, v),
           '{}'.format(v), f'{v}!', hash(v) == hash(s.strip('<>')), {v: 1},
           v.upper(), v.encode(), v[0] if v else '', [c for c in v],
           v.startswith('a'), v.find('b'), v.split('b'), tuple(v),
           v.join('xy'), ' '.join([v, v])]
    if v:
        out.append(ord(v[0]))
    return out, v


def returned(s):
    return s.strip(), s[1:], s.partition(':')


def in_try(s):
    v = s.strip()
    try:
        if v == 'boom':
            raise ValueError(v)
        return v[1:]
    except ValueError as e:
        return 'caught ' + e.args[0] + ' ' + v
    finally:
        v = None


def generators(s):
    a = list(w.strip('.') for w in s.split())
    b = {w[:2] for w in s.split()}
    c = {w: len(w) for w in s.split(',')}
    return a, sorted(b), sorted(c.items())


def word_counts(text, repeat):
    counts = {}
    for r in range(repeat):
        for word in text.split():
            word = word.strip('.,;').lower()
            counts[word] = counts.get(word, 0) + 1
    return sorted(counts.items())


def view_items(texts):
    # s[i] on a view reads its root's character
    out = []
    for s in texts:
        for w in s.split():
            v = w.strip('.,')
            row = []
            for i in (0, 1, -1, -2, len(v) - 1):
                try:
                    row.append(v[i])
                except IndexError as e:
                    row.append(('IndexError', str(e)))
            if v:
                row.append(v[0] is v[:1])
            out.append(row)
    return out


def view_item_errors(s):
    v = s.strip()
    out = []
    for i in (len(v), -len(v) - 1, 10 ** 30, True, 1.5, None, '0'):
        try:
            out.append(v[i])
        except Exception as e:
            out.append((type(e).__name__, str(e)))
    return out


def view_splits(texts):
    # split of a strip, slice or split item: items are views of the root
    out = []
    for s in texts:
        out.append(list(s.strip().split()))
        for w in s.strip().split():
            out.append(w)
        for w in s[1:-1].split(' '):
            out.append((w, len(w)))
        for w in s.strip(' .').split(', '):
            out.append(w.lower())
        for part in s.split(','):
            for w in part.split():
                out.append(w.upper())
    return out


def view_split_identity(s):
    # an item with no separator in it is its view, as Substring gives
    v = s.strip()
    out = []
    for w in v.split():
        out.append((w is v, w == v))
    for w in v.split(','):
        out.append((w is v, w == ''))
    t = s.strip()
    out.append([w is t for w in t.split()])
    return out, v


def view_split_forced_source(s):
    # the view split is built during the loop: later items still scan its
    # root at the same places
    v = s.strip()
    out = []
    for w in v.split():
        out.append((v, w, w is v, w[0]))
    t = s[1:]
    for w in t.split(' '):
        out.append((w, t[:1]))
    return out


def view_split_escapes(s):
    v = s.strip()
    items = v.split()
    it = iter(v.split(','))
    first = next(it, None)
    rest = list(it)
    out = []
    for w in v.split():
        out.append(w)
        if len(out) == 2:
            break
    return items, first, rest, out, v


CASES = [
    ('split_items', [(TEXTS,)], {}),
    ('split_sep', [(TEXTS, SEPS)], {}),
    ('split_const_sep', [('a,b,,cab,dabab',), (',',), ('',), ('é,€ab😀,',)],
     {}),
    ('split_identity', [('one',), ('a b',), ('',), ('é',), ('a,b',),
                        ('x',)], {}),
    ('split_items_kept', [('a bb ccc a',), ('é €',)], {}),
    ('split_list_escapes', [('a b c',), ('',)], {}),
    ('split_list_mutated', [('a b',), ('x',)], {}),
    ('split_break', [('it was the best',), ('none here',)], {}),
    ('split_nested', [('ab cd ae', 'a1,c2,,e'), ('', 'x')], {}),
    ('split_receivers', [('  a b  ',), ('é x',)], {}),
    ('split_errors', [('a,b,c',)], {}),
    ('split_maxsplit', [('a b c',), ('a,b,c,d',)], {}),
    ('split_generator', [('a bb ccc',), ('x,y',)], {}),
    ('strips', [(TEXTS,)], {}),
    ('strip_identity', [(TEXTS,)], {}),
    ('strip_of_split', [(TEXTS[0],), ('a. ;b, c',), ('é. €,',)], {}),
    ('strip_of_split_unused_parent', [(TEXTS[0],), ('..,; x',)], {}),
    ('strip_chain', [('  -ab-xyz-ba-  ',), ('abc',), ('',), (' -é- ',)], {}),
    ('strip_subclass', [('  ab  ',), ('x',)], {}),
    ('strip_errors', [('abcba',)], {}),
    ('slices', [(TEXTS,)], {}),
    ('slice_identity', [('hello',), ('h',), ('',), ('héllo',), ('😀x',)], {}),
    ('slice_bounds', [('  hello  ', 1, 3), (' héllo ', -3, None),
                      ('abc', None, -9), ('abc', 5, 1), ('€', 0, 1)], {}),
    ('slice_subclass', [('abc',)], {}),
    ('partitions', [(TEXTS, SEPS + ['=', 'the', 'times'])], {}),
    ('partition_identity', [('k=v',), ('kv',), ('=',), ('a=b=c',), ('',)],
     {}),
    ('partition_of_view', [('  key = value  ',), ('novalue',), (' é=€ ',)],
     {}),
    ('partition_errors', [('a=b',)], {}),
    ('compares', [(TEXTS + [' one ', 'ONE'],)], {}),
    ('contains', [(TEXTS,)], {}),
    ('as_keys', [(TEXTS,)], {}),
    ('kinds', [(TEXTS,)], {}),
    ('frame_locals', [('a. b. c',)], {}),
    ('loop_carried', [('  abcdefgh  ',), ('aaab',), ('',), ('é€😀x',)], {}),
    ('escapes', [('<abc>',), ('<>',), ('<é€😀>',)], {}),
    ('returned', [('a:b',), ('',), (' x ',)], {}),
    ('in_try', [(' boom ',), (' fine ',)], {}),
    ('generators', [('a. b.. c',), ('x,y,x',)], {}),
    ('word_counts', [(TEXTS[0], 3), ('', 2), ('é. é, €;', 2)], {}),
    ('view_items', [(TEXTS,)], {}),
    ('view_item_errors', [(' abc ',), ('  ',), (' é€😀 ',)], {}),
    ('view_splits', [(TEXTS + [' a, b, c. ', ',,', ' x '],)], {}),
    ('view_split_identity', [(' one ',), (' a b ',), ('  ',), (' a,b ',),
                             (' é ',)], {}),
    ('view_split_forced_source', [(' ab cd ef ',), (' one ',), ('  ',),
                                  ('xa b  c',)], {}),
    ('view_split_escapes', [(' a b c ',), ('',), (' x,y ',)], {}),
]
