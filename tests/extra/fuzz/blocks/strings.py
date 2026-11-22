from . import Block

TEXTS = ["'  It was, the best,of times  '", "'one'", "''", "'   '", "'a\\tb\\nc,d'",
         "'  caf\\u00e9, cr\\u00e8me ,br\\u00fbl\\u00e9e '", "'\\u03a9\\u03bc, \\u03bb\\u00a0x '",
         "'\\u65e5\\u672c, \\u8a9e\\u3000\\u30c6 '", "' emoji \\U0001f600,\\U0001f600 '",
         "P_S('  sub, class  ')", "P_E('  eq, sub  ')"]

BLOCKS = [
    Block('str_view_escape', 'strings',
          'slices, strip and partition of an exact str kept as views, read by len, ==, '
          '!= and in without being built',
          'the text changes kind or becomes a str subclass, and the views escape after the '
          'break: stored, a dict key, compared by identity, hashed, operated on',
          '''
class P_S(str):
    pass
class P_E(str):
    def __eq__(self, o):
        return str.__eq__(self.strip(), o)
    __hash__ = str.__hash__
P_text = S_text
P_route = 0
P_keep = []
P_d = {}
def P_break():
    global P_text, P_route
    P_text = S_text2
    P_route = S_route
def P_run(r):
    s = P_text
    a = s[1:S_cut]
    b = s.strip()
    c, sep, d = s.partition(S_sep)
    e = b.rstrip(S_chars)
    f = a.lstrip()
    n = len(a) + len(b) + len(c) + len(d) + len(e) + len(f)
    eq = (a == b, c != d, 'e' in e, sep == S_sep, f == e, s == b)
    inner = NEXT(r)
    if P_route == 1:
        P_keep.append(e)
        P_keep.append(f)
        del P_keep[:-4]
        e = list(P_keep)
    elif P_route == 2:
        if len(P_d) > 8:
            P_d.clear()
        P_d[b] = r
        P_d[c] = P_d.get(d, 0) + 1
        e = (P_d.get(e), b in P_d, sorted(P_d.items())[:4])
    elif P_route == 3:
        e = (b is s, e is b, a is s[1:S_cut], c is s, f is a, d is s, type(b).__name__)
    elif P_route == 4:
        e = (hash(e) == hash(str(e)), hash(b) == hash(s.strip()))
    elif P_route == 5:
        e = (e.upper(), b + c, a * 2, f.split(','), d.partition(S_sep))
    elif P_route == 6:
        e = [x[::-1] for x in (a, b, c, d, e, f)]
    return n, eq, e, inner
''', slots={'text': TEXTS[:9], 'text2': TEXTS, 'cut': ['5', '-1', '1000', '0'],
            'sep': ["','", "' '", "'\\u00e9'", "'\\U0001f600'", "', '"],
            'chars': ["None", "' ,'", "'s'", "'\\u00e9e'"],
            'route': ['1', '2', '3', '4', '5', '6']}),

    Block('split_loop_escape', 'strings',
          'a for loop over s.split() scanning one item per FOR_ITER, each item a view',
          'after the break the items escape (kept, dict keys, compared by identity), the loop '
          'breaks early, nests another split, or the text changes kind or class',
          '''
class P_S(str):
    pass
class P_E(str):
    def __eq__(self, o):
        return str.__eq__(self.strip(), o)
    __hash__ = str.__hash__
P_text = S_text
P_route = 0
P_hold = []
def P_break():
    global P_text, P_route
    P_text = S_text2
    P_route = S_route
def P_run(r):
    total = 0
    k = 0
    seen = []
    counts = {}
    inner = None
    for w in P_text.split(S_sep):
        k += 1
        total += len(w)
        if w == 'of' or w != 'x' and 'e' in w:
            total += 100
        if k == 1:
            inner = NEXT(r)
        if P_route == 1 and k == 2:
            P_hold.append(w)
            del P_hold[:-3]
        elif P_route == 2:
            seen.append(w)
        elif P_route == 3:
            counts[w] = counts.get(w, 0) + 1
        elif P_route == 4 and k == 2:
            break
        elif P_route == 5:
            seen.append(w is P_text)
        elif P_route == 6:
            for x in w.split(S_sep2):
                seen.append(x.strip())
        elif P_route == 7:
            seen.append(w.strip(' ,') is w)
    return k, total, seen[:12], sorted(counts.items())[:6], list(P_hold), inner
''', slots={'text': TEXTS[:9], 'text2': TEXTS, 'sep': ['None', "','", "' '", "', '"],
            'sep2': ["'e'", 'None', "'\\u00e9'"], 'route': ['1', '2', '3', '4', '5', '6', '7']}),

    Block('split_forced', 'strings',
          "the split loop's virtual list and its iterator, forced mid-loop when the "
          'frame must go on interpreted or be seen',
          'tracing switched on mid-loop, an exception through the loop, or the frame read by '
          'sys._getframe, after the break',
          '''
import sys
class P_S(str):
    pass
class P_E(str):
    def __eq__(self, o):
        return str.__eq__(self.strip(), o)
    __hash__ = str.__hash__
P_text = S_text
P_how = 0
P_events = []
def P_tracer(frame, event, arg):
    if event == 'call':
        P_events.append(frame.f_code.co_name.split('_')[-1])
        del P_events[:-6]
    return None
def P_h(w):
    return len(w)
def P_break():
    global P_text, P_how
    P_text = S_text2
    P_how = S_how
def P_run(r):
    del P_events[:]
    out = []
    k = 0
    inner = None
    try:
        for w in P_text.split(S_sep):
            k += 1
            if k == 2:
                if P_how == 1:
                    sys.settrace(P_tracer)
                elif P_how == 2:
                    raise KeyError(w)
                elif P_how == 3:
                    out.append(sorted(sys._getframe().f_locals)[:3])
                inner = NEXT(r)
            out.append((P_h(w), w == 'one'))
    except KeyError as e:
        out.append(('raised', e.args[0]))
    finally:
        if P_how == 1:
            sys.settrace(None)
    return k, out[:12], list(P_events), inner
''', slots={'text': TEXTS[:9], 'text2': TEXTS, 'sep': ['None', "','", "' '"],
            'how': ['1', '2', '3']}),

    Block('str_iadd', 'strings',
          's += t on an exact str appending in place when only the frame holds s',
          'after the break s gains another holder before the append (an alias, a list, a '
          'cell, f_locals), the right operand is a str subclass or widens the kind',
          '''
import sys
class P_T(str):
    def __radd__(self, o):
        return 'R' + o[:3]
P_mode = 0
P_hold = []
def P_break():
    global P_mode
    P_mode = S_mode
def P_run(r):
    del P_hold[:]
    s = 'ab' + str(r)
    alias = None
    inner = None
    for i in range(S_n):
        if i == 2:
            if P_mode == 1:
                alias = s
            elif P_mode == 2:
                P_hold.append(s)
            elif P_mode == 3:
                s += S_wide
            elif P_mode == 4:
                s += P_T('t')
            elif P_mode == 5:
                alias = sys._getframe().f_locals['s']
            elif P_mode == 6:
                s += s
            inner = NEXT(r)
        s += 'xy'
        s = s + str(i % 3)
    c = 'q' + str(r)
    def P_get():
        return c
    c += S_wide
    g = P_get()
    c = c + 'z'
    return len(s), s[-8:], alias, list(P_hold), g, P_get(), inner
''', slots={'mode': ['1', '2', '3', '4', '5', '6'], 'n': ['3', '8', '30'],
            'wide': ["'\\u00e9'", "'\\u20ac'", "'\\U0001f600'", "'.'"]}),
]
