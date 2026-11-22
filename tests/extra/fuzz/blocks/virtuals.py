from . import Block

BLOCKS = [
    Block('vtuple_escape', 'virtual',
          'a tuple kept virtual, its items known separately',
          'the tuple escapes by another route after the break: stored, raised, rebuilt, hashed',
          '''
P_store = []
P_route = 0
def P_break():
    global P_route
    P_route = S_route
def P_run(r):
    t = (r, r + 1, 'x')
    u = t + (r,)
    a, b, c = t
    inner = NEXT(r)
    if P_route == 1:
        P_store.append(u)
        del P_store[:-3]
        u = P_store[-1]
    elif P_route == 2:
        try:
            raise ValueError(u)
        except ValueError as e:
            u = e.args[0]
    elif P_route == 3:
        u = tuple(reversed(u))
    elif P_route == 4:
        u = hash(u) == hash((r, r + 1, 'x', r))
    elif P_route == 5:
        u = u[1:] + u[:1] * 2
    return a + b, u, len(t), inner
''', slots={'route': ['1', '2', '3', '4', '5']}),

    Block('vlist_alias', 'virtual',
          'a list built and appended to while virtual',
          'the list escapes into a global, then is mutated through another name',
          '''
P_saved = []
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    l = [r, r * 2]
    l.append(r + S_k)
    m = l
    if P_broken:
        P_saved[:] = [l]
    inner = NEXT(r)
    m.append(1)
    m[0] = S_v
    return l[-1], len(l), l[0], [len(x) for x in P_saved], inner
''', slots={'k': ['1', '2**63', '0.5'], 'v': ["'s'", 'None', '-1']}),

    Block('vstr_concat', 'virtual',
          'strings concatenated, sliced and indexed while virtual',
          'size past a limit, and a str subclass with __radd__ reaching the concatenation',
          '''
class P_Str(str):
    def __radd__(self, o):
        return str.__add__(o, self.upper())
P_piece = S_piece
P_n = S_n
def P_break():
    global P_piece, P_n
    P_piece = S_piece2
    P_n = S_n2
def P_run(r):
    s = S_base
    for i in range(P_n):
        s += P_piece if i % 2 else str(i)
    c = s[r % len(s)] if s else ''
    sl = s[1:S_cut]
    d = {sl: 1}
    inner = NEXT(r)
    return len(s), c, sl[:20], sl in d, s.count('1'), s[-3:], type(s).__name__, inner
''', slots={'piece': ["'ab'", "'\\u00e9'", "''"], 'n': ['3', '6'],
            'piece2': ["P_Str('zz')", "'x' * 100", "'\\U0001f600'"], 'n2': ['40', '300'],
            'base': ["'ab'", "''", "'\\u20ac'"], 'cut': ['5', '-1', '1000']}),

    Block('vrange', 'virtual',
          'a range and its iterator kept virtual in a for loop',
          'size past a limit: bounds near 2**63 and huge lengths after the break',
          '''
P_args = S_args
def P_break():
    global P_args
    P_args = S_args2
def P_run(r):
    try:
        rg = range(*P_args)
        acc = 0
        n = 0
        for i in rg:
            acc += i
            n += 1
            if n > 50:
                break
        ln = len(rg)
    except (OverflowError, ValueError) as e:
        acc, n, ln = type(e).__name__, 0, -1
    return acc, n, ln, NEXT(r)
''', slots={'args': ['(0, 5)', '(5, 0, -1)', '(-3, 3, 2)', '(0, 10**6, 333333)'],
            'args2': ['(2**63 - 2, 2**63 + 2)', '(0, 2**64)', '(-2**63, -2**63 + 3)', '(3, 3)',
                      '(0, 5, 0)', '(2**31 - 1, 2**31 + 1)']}),

    Block('nested_tuples', 'virtual',
          'nested virtual tuples, until the nested weight forces them out',
          'size past a limit: the nesting deepens after the break',
          '''
P_depth = S_d
def P_break():
    global P_depth
    P_depth = S_d2
def P_run(r):
    t = ()
    for i in range(P_depth):
        t = (t, i + r)
    x = t
    d = 0
    while x:
        x = x[0]
        d += 1
    inner = NEXT(r)
    return d, t[1] if t else None, inner
''', slots={'d': ['1', '2', '5'], 'd2': ['30', '200', '0']}),

    Block('bound_method', 'virtual',
          'a bound method kept virtual and called',
          'method replacement on the class; the method escapes into a list',
          '''
class P_C:
    def m(self, a):
        return a + 1
P_o = P_C()
def P_break():
    P_C.m = S_new
def P_run(r):
    m = P_o.m
    a = m(r)
    lst = [m]
    b = lst[0](r)
    inner = NEXT(r)
    return a, b, m == P_o.m, m.__name__, inner
''', slots={'new': ['lambda self, a: a - 1', 'staticmethod(lambda a: -a)',
                    'classmethod(lambda c, a: c.__name__)']}),

    Block('star_args', 'virtual',
          'a call with *args and **kwargs built from known parts',
          'type switch of the starred global: another iterable, or not one',
          '''
P_L = S_l
def P_f(*a, **k):
    return len(a), a[-1:], sorted(k.items())
def P_break():
    global P_L
    P_L = S_l2
def P_run(r):
    try:
        x = P_f(r, *P_L)
        y = P_f(*P_L, k=r)
        z = P_f(**{'z': r}, **{'w': 1})
    except TypeError as e:
        x = y = z = 'TypeError'
    return x, y, z, NEXT(r)
''', slots={'l': ['[1, 2]', '(1,)', '[]'],
            'l2': ["'ab'", 'range(3)', '{1: 2}', '7', 'iter([5])', '(None,) * 40']}),

    Block('unpack_size', 'virtual',
          'unpacking a sequence of known length',
          'size past a limit: the sequence changes length or kind',
          '''
P_seq = S_seq
def P_break():
    global P_seq
    P_seq = S_seq2
def P_run(r):
    try:
        a, b, c = P_seq
        x = (a, c)
    except (ValueError, TypeError) as e:
        x = type(e).__name__
    try:
        h, *t = P_seq
        y = (h, len(t))
    except (ValueError, TypeError) as e:
        y = type(e).__name__
    return x, y, NEXT(r)
''', slots={'seq': ['(1, 2, 3)', '[1, 2, 3]', "'abc'"],
            'seq2': ['(1, 2)', '[1, 2, 3, 4]', 'range(3)', "{'a': 1, 'b': 2, 'c': 3}", 'None', '()']}),
]
