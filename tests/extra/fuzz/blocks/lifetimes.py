from . import Block

BLOCKS = [
    Block('finaliser_effects', 'lifetimes',
          "when a local's last reference goes, which compiled code decides by its own "
          'reference counting and virtual objects',
          'global rebinding from a finaliser, run at a point inside the compiled frame',
          '''
P_log = []
P_g = 1
class P_D:
    def __init__(self, n):
        self.n = n
    def __del__(self):
        global P_g
        P_log.append(self.n)
        del P_log[:-6]
        P_g = S_g
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    del P_log[:]
    a = P_g
    o = P_D(r)
    if P_broken:
        o = None
    b = P_g
    t = (P_D(-1),)
    t = 0
    inner = NEXT(r)
    c = P_g
    o = None
    d = P_g
    return a, b, c, d, list(P_log), inner
''', slots={'g': ["'fin'", '2.5', 'None']}),

    Block('weakref_callback', 'lifetimes',
          'a weak reference whose callback runs when compiled code drops the object',
          'attribute replacement on a class from the callback, between two uses',
          '''
import weakref
class P_C:
    def m(self):
        return 'm'
class P_O:
    pass
P_log = []
def P_cb(w):
    P_log.append('cb')
    del P_log[:-4]
    P_C.m = S_new
P_keep = []
def P_break():
    P_keep.append(1)
def P_run(r):
    c = P_C()
    a = c.m()
    o = P_O()
    w = weakref.ref(o, P_cb)
    if P_keep:
        del o
    b = c.m()
    inner = NEXT(r)
    return a, b, w() is None, len(P_log), inner
''', slots={'new': ["lambda self: 'cb'", 'lambda self: 1 // 0']}),

    Block('descriptor_side_effect', 'attributes',
          "a property read compiled as a call of its getter, with the class's guards "
          'kept across it',
          'method replacement from inside the attribute load itself',
          '''
P_broken = False
class P_C:
    def m(self):
        return 'orig'
    @property
    def p(self):
        if P_broken:
            P_C.m = S_new
        return 'p'
P_o = P_C()
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    a = P_o.m()
    b = P_o.p
    try:
        c = P_o.m()
    except Exception as e:
        c = type(e).__name__
    inner = NEXT(r)
    return a, b, c, inner
''', slots={'new': ["lambda self: 'new'", 'property(lambda self: 1)', 'None']}),

    Block('dict_mutating_eq', 'attributes',
          'a dict lookup with a key whose __eq__ mutates the dict',
          'the key starts mutating the dict mid-lookup after the break',
          '''
P_broken = False
P_busy = []
P_d = {}
class P_K:
    def __init__(self, v):
        self.v = v
    def __hash__(self):
        return self.v % 4
    def __eq__(self, o):
        if P_broken and not P_busy and len(P_d) < 20:
            P_busy.append(1)
            P_d[P_K(len(P_d) * 4 + 100)] = 'grown'
            P_busy.pop()
        return isinstance(o, P_K) and o.v == self.v
P_d[P_K(1)] = 'one'
P_d[P_K(5)] = 'five'
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    out = [P_d.get(P_K(5)), P_K(1) in P_d, P_d.get(P_K(9), 'none')]
    inner = NEXT(r)
    out.append(len(P_d))
    return out, inner
'''),

    Block('index_protocol', 'arith',
          'operations that convert through __index__',
          'size past a limit: __index__ returns edge values after the break',
          '''
P_v = 2
class P_I:
    def __index__(self):
        return P_v
P_i = P_I()
def P_break():
    global P_v
    P_v = S_v
def P_run(r):
    out = []
    for f in range(5):
        try:
            if f == 0:
                out.append([1, 2, 3][P_i])
            elif f == 1:
                out.append(len(range(P_i)))
            elif f == 2:
                out.append(len('ab' * P_i))
            elif f == 3:
                out.append(bin(P_i))
            else:
                out.append((1 << P_i) if P_v < 200 else 'big')
        except (IndexError, OverflowError, TypeError, ValueError, MemoryError) as e:
            out.append(type(e).__name__)
    return out, NEXT(r)
''', slots={'v': ['-1', '2**63', '-2**63 - 1', '2**40', 'True', "'s'"]}),
    Block('dead_locals', 'lifetimes',
          'a dead local keeps its value until rebound or deleted, only what is known '
          'about it dropped at a merge point',
          'after the break the dead locals are read: through sys._getframe().f_locals, a '
          'callee reading its caller\'s frame, a traceback\'s frame, or released by del or '
          'rebinding, with __del__ logging when each goes',
          '''
import sys
P_log = []
class P_D:
    def __init__(self, n):
        self.n = n
    def __del__(self):
        P_log.append(self.n)
        del P_log[:-8]
P_how = 0
def P_break():
    global P_how
    P_how = S_how
def P_view(f):
    return sorted((k, v.n if isinstance(v, P_D) else v) for k, v in f.f_locals.items()
                  if not k.startswith('_'))
def P_peek():
    return P_view(sys._getframe(1))
def P_raiser(n):
    z = P_D(-n)
    dead = n * 2
    q = (z.n, dead)
    raise ValueError(q[0])
def P_run(r):
    del P_log[:]
    a = P_D(r)
    x = a.n + 1
    t = (r, 'k')
    for i in range(S_n):
        v = P_D(i)
        w = i * S_mul
    seen = None
    if P_how == 1:
        seen = P_view(sys._getframe())
    elif P_how == 2:
        seen = P_peek()
    elif P_how == 3:
        try:
            P_raiser(r)
        except ValueError as e:
            seen = P_view(e.__traceback__.tb_next.tb_frame)
    elif P_how == 4:
        del a
        del v
    elif P_how == 5:
        a = None
        for i in range(2):
            v = i
    elif P_how == 6:
        try:
            raise KeyError(x)
        except KeyError:
            seen = P_view(sys.exc_info()[2].tb_frame)
    logged = list(P_log)
    inner = NEXT(r)
    return x, t, seen, logged, inner
''', slots={'how': ['1', '2', '3', '4', '5', '6'], 'n': ['1', '3'], 'mul': ['2', '0.5']}),
]
