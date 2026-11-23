# Attributes of instances whose attributes are in a dict of their own
# rather than their class's shared keys: on 3.11 and later an instance
# gets one when the class's shared keys are full (30 names). Compiled loads and stores
# find the name in a combined dict with str keys by the dict's own probe
# (x64/ipyencoding.c), and anything else goes to the helper.
import gc
import weakref


class Many:
    k = 'class attribute'

    def __init__(self, i):
        for n in range(32):
            setattr(self, 'a%d' % n, i + n)
        self.x = i
        self.lst = None

    def meth(self):
        return self.x


def _many(n):
    return [Many(i) for i in range(n)]


def loads_and_stores(n):
    objs = _many(n)
    t = 0
    for o in objs:
        o.x = o.x + 1
        o.a3 = o.a3 * 2
        t += o.a3 + o.x + o.meth() + len(o.k)
    return t, sorted(objs[-1].__dict__.items(), key=repr)


def gc_value_into_dict(n):
    o = Many(n)
    o.lst = [o]
    r = weakref.ref(o)
    tracked = gc.is_tracked(o.__dict__)
    del o
    gc.collect()
    return tracked, r() is None


def delete_then_load(n):
    o = Many(n)
    del o.x
    try:
        o.x
    except AttributeError as e:
        gone = str(e)
    o.x = 'back'
    return gone, o.x, o.meth()


def keys_not_all_str(n):
    o = Many(n)
    o.__dict__[1] = 'int key'
    o.x = o.x + 1
    return o.x, o.a2, o.__dict__[1]


def wide_dict(n):
    o = Many(n)
    for i in range(150):
        setattr(o, 'w%d' % i, i)
    o.x = o.x + o.w149
    return o.x, o.w7, o.meth()


def equal_key_not_the_same(n):
    # 'xy' built at run time is a different object from the interned name
    o = Many(n)
    o.__dict__[''.join(['x', 'y'])] = n
    o.xy = o.xy + 1
    return o.xy, sorted(k for k in o.__dict__ if k.startswith('x'))


def dict_replaced(n):
    o = Many(n)
    o.__dict__ = {'x': 'replaced', 'a1': n}
    o.x = o.x + '!'
    return o.x, o.a1, o.meth(), o.k


CASES = [
    ('loads_and_stores', [(5,)], {}),
    ('gc_value_into_dict', [(3,)], {}),
    ('delete_then_load', [(3,)], {}),
    ('keys_not_all_str', [(3,)], {}),
    ('wide_dict', [(3,)], {}),
    ('equal_key_not_the_same', [(3,)], {}),
    ('dict_replaced', [(3,)], {}),
]
