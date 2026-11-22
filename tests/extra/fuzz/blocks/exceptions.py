from . import Block

BLOCKS = [
    Block('raise_finally', 'exceptions',
          'a try/finally inside a try/except around a call; the block stack and the '
          'pseudo-exception at compile time',
          'exception through the site after the break, of a kind the handler may not catch',
          '''
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    log = []
    v = None
    try:
        try:
            log.append(1)
            v = NEXT(r)
            if P_broken and r % S_m == 0:
                raise S_exc(r)
            log.append(2)
        finally:
            log.append(3)
    except S_catch as e:
        log.append(type(e).__name__)
    return log, v
''', slots={'m': ['1', '2', '3'], 'exc': ['ValueError', 'KeyError', 'OverflowError', 'StopIteration'],
            'catch': ['Exception', 'ValueError', 'LookupError']}),

    Block('with_suppress', 'exceptions',
          'a with statement around a raise',
          'the context manager starts suppressing exceptions after the break',
          '''
class P_CM:
    def __init__(self):
        self.n = 0
    def __enter__(self):
        self.n += 1
        return self.n % 100
    def __exit__(self, t, v, tb):
        return P_suppress and t is not None
P_cm = P_CM()
P_suppress = False
def P_break():
    global P_suppress
    P_suppress = True
def P_run(r):
    out = []
    for i in range(3):
        with P_cm as n:
            out.append(n)
            if i == 0:
                out.append(NEXT(r))
            if i == S_i:
                raise S_exc('w')
    return out
''', slots={'i': ['1', '2'], 'exc': ['ValueError', 'KeyError', 'ZeroDivisionError']}),

    Block('exc_in_compare', 'exceptions',
          'membership and dict lookups that call a Python __eq__',
          'exception through the site: __eq__ starts raising after the break',
          '''
P_broken = False
class P_K:
    def __init__(self, v):
        self.v = v
    def __eq__(self, o):
        if P_broken:
            raise S_exc('eq')
        return isinstance(o, P_K) and o.v == self.v
    def __hash__(self):
        return hash(self.v)
P_items = [P_K(i) for i in range(3)]
P_d = {P_K(1): 'one'}
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    k = P_K(r % 3)
    out = []
    try:
        out.append(k in P_items)
    except Exception as e:
        out.append(type(e).__name__)
    try:
        out.append(P_d.get(k))
    except Exception as e:
        out.append(type(e).__name__)
    try:
        out.append(P_items.index(k))
    except Exception as e:
        out.append(type(e).__name__)
    out.append(P_items[0] in P_items)
    return out, NEXT(r)
''', slots={'exc': ['ValueError', 'TypeError', 'RecursionError']}),

    Block('custom_iter', 'exceptions',
          'a for loop over a Python iterator; StopIteration ends it',
          'exception through the site: the iterator raises something else mid-iteration',
          '''
P_broken = False
class P_It:
    def __init__(self, n):
        self.i = 0
        self.n = n
    def __iter__(self):
        return self
    def __next__(self):
        self.i += 1
        if self.i > self.n:
            raise StopIteration
        if P_broken and self.i == S_at:
            raise S_exc('it')
        return self.i
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    out = []
    try:
        for v in P_It(4):
            out.append(v)
    except Exception as e:
        out.append(type(e).__name__)
    try:
        s = list(P_It(3))
    except Exception as e:
        s = type(e).__name__
    inner = NEXT(r)
    return out, s, next(P_It(0), 'dflt'), inner
''', slots={'at': ['1', '2', '3'], 'exc': ['ValueError', 'KeyError', 'StopIteration']}),

    Block('exc_state', 'exceptions',
          'sys.exc_info and the exception context across compiled frames',
          'exception through the site: a second exception raised inside the handler',
          '''
import sys
P_raise = False
def P_h():
    t = sys.exc_info()[0]
    return t.__name__ if t else None
def P_break():
    global P_raise
    P_raise = True
def P_run(r):
    ctx = None
    try:
        raise S_exc(r)
    except S_exc:
        a = P_h()
        inner = NEXT(r)
        if P_raise:
            try:
                raise ValueError('in handler')
            except ValueError as e2:
                ctx = type(e2.__context__).__name__
        b = P_h()
    c = P_h()
    return a, b, c, ctx, inner
''', slots={'exc': ['KeyError', 'OSError', 'ArithmeticError']}),
]
