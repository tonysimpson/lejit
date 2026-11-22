from . import Block

BLOCKS = [
    Block('attr_shadow', 'attributes',
          'a class attribute read through an instance, with a load guard on the class',
          'attribute addition: the instance or the class changes what the name finds',
          '''
class P_C:
    x = S_v
    def __init__(self):
        self.y = 1
P_o = P_C()
def P_break():
    S_how
def P_run(r):
    try:
        a = P_o.x
    except AttributeError:
        a = 'AttributeError'
    inner = NEXT(r)
    try:
        b = P_o.x
    except AttributeError:
        b = 'AttributeError'
    return a, b, P_o.y, inner
''', slots={'v': ['1', "'s'", 'None'],
            'how': ['P_o.x = 2', 'P_C.x = 3', 'del P_C.x', "P_C.x = property(lambda s: 'prop')",
                    "P_o.__dict__['x'] = 'dict'", 'P_C.y = property(lambda s: -1)']}),

    Block('method_replace', 'attributes',
          'a method looked up on the class and called directly',
          'method replacement on the class or instance, or deletion',
          '''
class P_C:
    def m(self):
        return 'orig'
P_o = P_C()
def P_break():
    S_how
def P_run(r):
    out = []
    for i in range(3):
        try:
            out.append(P_o.m())
        except Exception as e:
            out.append(type(e).__name__)
        if i == 0:
            out.append(NEXT(r))
    return out
''', slots={'how': ["P_C.m = lambda self: 'class'", "P_o.m = lambda: 'inst'", 'del P_C.m',
                    "P_C.m = staticmethod(lambda: 'static')", 'P_C.m = classmethod(lambda c: c.__name__)',
                    'P_C.m = len']}),

    Block('getattr_added', 'attributes',
          'instance attribute loads compiled from the shared keys',
          'the class gains __getattr__, __getattribute__ or a data descriptor after the break',
          '''
class P_C:
    def __init__(self):
        self.a = 1
P_o = P_C()
def P_break():
    S_how
def P_run(r):
    out = [P_o.a]
    try:
        out.append(P_o.b)
    except AttributeError:
        out.append('AttributeError')
    inner = NEXT(r)
    out.append(getattr(P_o, 'a', 'dflt'))
    return out, inner
''', slots={'how': ['P_C.__getattr__ = lambda s, n: n.upper()', "P_C.__getattribute__ = lambda s, n: 'ga:' + n",
                    "P_C.a = property(lambda s: 'prop')", 'P_C.b = 7', 'del P_o.a']}),

    Block('setattr_added', 'attributes',
          'instance stores compiled into the dict with a store guard',
          'the class gains __setattr__, or a data descriptor for the name',
          '''
class P_C:
    pass
P_o = P_C()
P_log = []
def P_logset(s, n, v):
    P_log.append(n)
    del P_log[:-4]
def P_break():
    S_how
def P_run(r):
    try:
        P_o.y = r
        P_o.z = r + 1
    except AttributeError:
        pass
    inner = NEXT(r)
    return getattr(P_o, 'y', None), P_o.__dict__.get('z'), len(P_log), inner
''', slots={'how': ['P_C.__setattr__ = P_logset', 'P_C.y = property(lambda s: 1, lambda s, v: P_log.append(v))',
                    'P_C.y = property(lambda s: 1)', 'P_C.__delattr__ = P_logset',
                    "P_C.__slots__ = ('q',)"]}),

    Block('shared_keys', 'attributes',
          'instances with the same keys share their layout; loads read a known slot',
          'attribute deletion or a replaced __dict__ breaks the shared layout',
          '''
class P_C:
    def __init__(self, a):
        self.a = a
        self.b = a + 1
P_objs = [P_C(i) for i in range(4)]
def P_break():
    S_how
def P_run(r):
    out = []
    inner = None
    for i, o in enumerate(P_objs):
        try:
            out.append(o.a + o.b)
        except (AttributeError, TypeError) as e:
            out.append(type(e).__name__)
        if i == 1:
            inner = NEXT(r)
    return out, inner
''', slots={'how': ['del P_objs[1].a', "P_objs[0].__dict__ = {'b': 's', 'a': 't'}",
                    'P_objs.append(P_C.__new__(P_C))', 'P_objs[2].c = 5', "P_objs[3].__dict__.clear()"]}),

    Block('slots_class', 'attributes',
          'member descriptors of __slots__ read and written',
          'the slot is deleted or set through its descriptor after the break',
          '''
class P_S:
    __slots__ = ('a', 'b')
    def __init__(self):
        self.a = 1
P_o = P_S()
def P_break():
    S_how
def P_run(r):
    try:
        a = P_o.a
    except AttributeError:
        a = 'AttributeError'
    inner = NEXT(r)
    try:
        b = P_o.b
    except AttributeError:
        b = 'AttributeError'
    return a, b, inner
''', slots={'how': ['del P_o.a', 'P_o.b = 2**64', "P_S.a.__set__(P_o, 'set')", 'P_S.b = 5']}),

    Block('dict_ops', 'attributes',
          'dict reads and writes with known keys',
          'key type switch, deletion and reinsertion after the break',
          '''
P_d = {'a': 1, 'b': 2}
P_keys = ('a', 'b')
def P_break():
    global P_keys
    S_how
def P_run(r):
    out = []
    for k in P_keys:
        try:
            out.append(P_d[k])
        except (KeyError, TypeError) as e:
            out.append(type(e).__name__)
    P_d['c'] = r
    inner = NEXT(r)
    out.append(list(P_d)[-2:])
    out.append(P_d.get('a', 'none'))
    return out, inner
''', slots={'how': ["P_keys = (1, 'a', 1.0)", "del P_d['a']", "P_keys = ([],)", "P_d.pop('b'); P_d['b'] = 'again'",
                    "P_keys = (('a',), True)"]}),

    Block('list_index', 'virtual',
          'list and tuple indexing with constant and edge indices',
          'type switch of the container, or an index past its end',
          '''
P_seq = S_seq
P_idx = S_idx
def P_break():
    global P_seq
    P_seq = S_seq2
def P_run(r):
    out = []
    for i in P_idx + (r,):
        try:
            out.append(P_seq[i])
        except (IndexError, TypeError) as e:
            out.append(type(e).__name__)
    try:
        out.append(P_seq[1:S_hi:S_step])
    except (TypeError, ValueError) as e:
        out.append(type(e).__name__)
    return out, NEXT(r)
''', slots={'seq': ['[1, 2, 3]', '(4, 5, 6, 7)'], 'seq2': ["'abcdef'", 'range(10)', '[]', "b'xyz'", '{0: 1}'],
            'idx': ['(0, -1)', '(2**31, -2**63)', '(3, -4)', '(True, 2**28)'],
            'hi': ['None', '-1', '2**63'], 'step': ['None', '-1', '2']}),
]
