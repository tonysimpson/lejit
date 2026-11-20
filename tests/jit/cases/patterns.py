# match statements: class patterns (MATCH_CLASS, through __match_args__,
# keywords, a type matching itself, and the TypeErrors match_class
# raises), mapping patterns (MATCH_MAPPING, MATCH_KEYS, **rest's
# COPY_DICT_WITHOUT_KEYS), sequence patterns (MATCH_SEQUENCE, GET_LEN),
# and patterns capturing many names, which rotate the stack (ROT_N). A
# source without the match statement has no cases here.
try:
    compile('match 0:\n case _: pass', '<probe>', 'exec')
    HAS_MATCH = True
except SyntaxError:
    HAS_MATCH = False

SRC = '''
class Point:
    __match_args__ = ('x', 'y')

    def __init__(self, x, y):
        self.x, self.y = x, y


class Point3(Point):
    __match_args__ = ('x', 'y', 'z')

    def __init__(self, x, y, z):
        Point.__init__(self, x, y)
        self.z = z


NOT_A_TYPE = 3


class NoArgs:
    pass


class BadArgs:
    __match_args__ = ['x']


class NotStr:
    __match_args__ = (1,)


class Many:
    __match_args__ = ('a', 'b', 'c', 'd', 'e', 'f')

    def __init__(self, *v):
        self.a, self.b, self.c, self.d, self.e, self.f = v


def classify(v):
    match v:
        case Point3(x, y, z):
            return ('p3', x, y, z)
        case Point(0, y):
            return ('on y', y)
        case Point(x=x, y=0):
            return ('on x', x)
        case Point(x, y) if x == y:
            return ('diagonal', x)
        case Point():
            return ('point', v.x, v.y)
        case int(n) | float(n):
            return ('number', n)
        case str() as s:
            return ('str', s)
        case NoArgs():
            return 'noargs'
        case _:
            return 'other'


def class_errors(kind):
    try:
        if kind == 0:
            match 5:
                case NOT_A_TYPE():   # not a type
                    return 'matched'
        elif kind == 1:
            match NoArgs():
                case NoArgs(1):   # no __match_args__
                    return 'matched'
        elif kind == 2:
            match BadArgs():
                case BadArgs(1):  # not a tuple
                    return 'matched'
        elif kind == 3:
            match Point(1, 2):
                case Point(1, 2, 3):   # too many
                    return 'matched'
        elif kind == 4:
            match Point(1, 2):
                case Point(1, x=1):    # x twice
                    return 'matched'
        elif kind == 5:
            match NotStr():
                case NotStr(1):   # not a string
                    return 'matched'
        elif kind == 6:
            match 7:
                case int(1, 2):   # a type matching itself takes one
                    return 'matched'
        elif kind == 7:
            match Point(1, 2):
                case Point(1, q=2):    # no such attribute: no match
                    return 'matched'
        return 'none'
    except TypeError as e:
        return ('TypeError', str(e))


def many(v):
    match v:
        case Many(a, b, c, d, e, f):
            return a + b + c + d + e + f
        case [a, b, c, d, e, f, *rest]:
            return (a, b, c, d, e, f, rest)
        case {'a': a, 'b': b, 'c': c, 'd': d, 'e': e}:
            return (a, b, c, d, e)
        case (Point(a, b), Point(c, d), Point(e, f)):
            return (a, b, c, d, e, f)
        case _:
            return None


many_items = many_points = many


def mapping(v):
    match v:
        case {'k': 1, **rest}:
            return ('k1', sorted(rest.items()))
        case {'k': k, 'j': j}:
            return ('kj', k, j)
        case {}:
            return ('empty or other', len(v))
        case _:
            return 'not a mapping'


def sequence(v):
    match v:
        case []:
            return 'empty'
        case [x]:
            return ('one', x)
        case [x, y]:
            return ('two', x, y)
        case [first, *mid, last]:
            return ('more', first, mid, last)
        case _:
            return 'not a sequence'


class BadKeys(dict):
    def get(self, key, default=None):
        raise KeyError(key)


def mapping_errors(v):
    try:
        match v:
            case {'a': a}:
                return ('a', a)
            case _:
                return 'none'
    except Exception as e:
        return (type(e).__name__, str(e))
'''

# growth that stops, plain as well, after a few hundred rounds (the free
# lists filling, at a pace that differs by the hash seed)
WARM = {'warmup': (700, 'growth lasting a few hundred rounds, plain too')}

if HAS_MATCH:
    exec(SRC)
    _VALUES = ('[(Point(0, 1),), (Point(2, 0),), (Point(3, 3),), (Point(1, 2),),'
               ' (Point3(1, 2, 3),), (4,), (1.5,), ("s",), (NoArgs(),), (None,),'
               ' ([1],), ((Point(1, 1),),)]')
    CASES = [
        ('classify', _VALUES, WARM),
        ('class_errors', [(k,) for k in range(8)], {}),
        # (in three processes: together their block counts wander further
        # than the leak check's slack, plain as well)
        ('many', '[(Many(1, 2, 3, 4, 5, 6),), (3,)]', WARM),
        ('many_points', '[((Point(1, 2), Point(3, 4), Point(5, 6)),)]', WARM),
        ('many_items', '[([1, 2, 3, 4, 5, 6, 7, 8],),'
                       ' ({"a": 1, "b": 2, "c": 3, "d": 4, "e": 5, "x": 6},), ([1, 2],)]', WARM),
        ('mapping', [({'k': 1, 'a': 2, 'b': 3},), ({'k': 2, 'j': 3},), ({'k': 1},), ({},),
                     ({'x': 1},), ([1],), (5,)], {}),
        ('sequence', [([],), ([1],), ((1, 2),), ([1, 2, 3, 4],), ('ab',), (range(5),),
                      ({1: 2},), (5,)], {}),
        ('mapping_errors', '[({"a": 1},), ({"b": 1},), (BadKeys(a=1),), ([("a", 1)],)]', {}),
    ]
else:
    CASES = []
