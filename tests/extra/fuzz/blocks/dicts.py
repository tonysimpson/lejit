from . import Block

BLOCKS = [
    Block('dict_get_store', 'dicts',
          'd[k] = d.get(k) OP y and d[k] OP= y on an exact dict with an exact-str key '
          "probe once, the store reusing the get's slot if dict, keys and version still"
          ' match',
          'after the break the dict is changed between the get and the store: by the operator\'s '
          '__radd__ (a key deleted, the dict resized or cleared, the key replaced), by a key\'s '
          '__eq__, or by an old value\'s __del__ at the store',
          '''
P_mode = 0
P_inner = []
P_d = {'a': 1, 'b': 2}
P_busy = []
def P_reset():
    P_d.clear()
    P_d.update(a=1, b=2)
def P_mutate():
    if P_busy:
        return
    P_busy.append(1)
    try:
        P_change()
    finally:
        P_busy.pop()
def P_change():
    if P_mode == 1:
        P_d.pop('a', None)
    elif P_mode == 2:
        for i in range(S_grow):
            P_d['g%d' % i] = i
    elif P_mode == 3:
        P_d.clear()
    elif P_mode == 4:
        P_d['a'] = 100
        P_d['c'] = -1
    elif P_mode == 5:
        P_d.pop('c', None)
        P_d['c'] = 50
class P_Y:
    def __radd__(self, o):
        P_mutate()
        return o + 1 if isinstance(o, int) else 0
class P_Old:
    def __add__(self, o):
        return o
    def __del__(self):
        P_mutate()
class P_K(str):
    __hash__ = str.__hash__
    def __eq__(self, o):
        P_mutate()
        return str.__eq__(self, o)
def P_note(v):
    P_inner[:] = [v]
    return 1
def P_break():
    global P_mode
    P_mode = S_mode
def P_run(r):
    d = P_d
    if len(d) > 40:
        P_reset()
    d['a'] = d.get('a', 0) + P_Y()
    d['c'] = d.get('c', 0) + P_note(NEXT(r))
    d['b'] = P_Old()
    d['b'] = d['b'] + 3
    k = S_key
    try:
        d[k] = d.get(k, 0) + 1
        d['a'] += 2
        d['c'] += P_Y()
    except (KeyError, TypeError) as e:
        P_reset()
        d['e'] = type(e).__name__
    out = sorted(d.items())[:8]
    return len(d), out, P_inner[0]
''', slots={'mode': ['1', '2', '3', '4', '5'], 'grow': ['3', '30', '200'],
            'key': ["'a'", "'k' + str(r % 2)", "P_K('a')", "P_K('n')", "'\\u00e9'"]}),

    Block('dict_split_probe', 'dicts',
          "str probes of an instance's key-sharing __dict__ read directly",
          'between get and store the shared keys change: an attribute added out of order to '
          'another instance, deleted from this one, or __dict__ replaced',
          '''
class P_C:
    def __init__(self, a):
        self.x = a
        self.y = a + 1
P_mode = 0
P_objs = [P_C(i) for i in range(3)]
P_inner = []
def P_mutate():
    if P_mode == 1:
        P_objs[1].z = 1
        P_objs[1].w = 2
    elif P_mode == 2:
        P_objs[0].__dict__.pop('y', None)
    elif P_mode == 3:
        P_objs[0].__dict__ = {'y': 7, 'x': 's'}
    elif P_mode == 4:
        P_objs.append(P_C(9))
        del P_objs[3:]
        P_objs[2].__dict__.clear()
class P_Y:
    def __radd__(self, o):
        P_mutate()
        return o if isinstance(o, str) else o + 1
def P_note(v):
    P_inner[:] = [v]
    return 0
def P_break():
    global P_mode
    P_mode = S_mode
def P_run(r):
    out = []
    for i, o in enumerate(P_objs):
        d = o.__dict__
        if len(d) > 10:
            d.clear()
        d['x'] = d.get('x', 0) + P_Y()
        d['y'] = d.get('y', 0) + (P_note(NEXT(r)) if i == 0 else 1)
        try:
            out.append((o.x, d['y'], 'z' in d))
        except (AttributeError, KeyError) as e:
            out.append(type(e).__name__)
    return out, P_inner[0]
''', slots={'mode': ['1', '2', '3', '4']}),
]
