# Inline attribute stores (pobject.c:inline_store, STORES.md "Option 4:
# design"): a set value overwritten, or the next empty slot filled in order,
# checked against a (class, name) cache that holds no keys. Every way the
# dict, its keys, the class or GC tracking can differ from the cache must
# give CPython's result. Run under a checked build, the replay check covers
# each condition's respawn.
import gc
import sys
import types


def _stats(key):
    ni = sys.modules.get('ni')
    return ni._stats()[key] if ni is not None else 0


class Ov:
    def __init__(self):
        self.a = 0
        self.b = None
        self.c = 'c'


def overwrite_loop(n):
    o = Ov()
    for i in range(n):
        o.a = i
        o.b = [i]
        o.c = (i, 'x')
        o.a = o.a + 1
    return o.__dict__


class Init:
    def __init__(self, i):
        self.x = i
        self.y = i * 2.5
        self.z = 'z'
        self.w = None


def init_fills(n):
    # from the second instance, x creates the dict (helper) and y, z, w
    # fill their slots inline: about one helper call per instance
    m = _stats('store_misses')
    objs = [Init(i) for i in range(n)]
    m = _stats('store_misses') - m
    return ([list(o.__dict__.items()) for o in objs[:3]], len(objs),
            m <= n + 8)


def guard_holds(n):
    # a class that doesn't change: once compiled, the stores' type guards
    # pass (a guard that always failed would still give the right result)
    o = Ov()
    overwrite_loop(n)
    t = _stats('store_type_changes')
    for i in range(n):
        o.a = i
        o.b = i
    return o.a, o.b, _stats('store_type_changes') - t


class Many:
    def __init__(self, i):
        self.a = i
        self.b = i + 1
        self.c = i + 2


def many_instances(n):
    objs = [Many(i) for i in range(n)]
    for o in objs:
        o.b = o.a + o.c
    return sum(o.a + o.b + o.c for o in objs), list(objs[-1].__dict__)


class Ord:
    pass


def _store_r(o, v):
    o.r = v


def _store_q(o, v):
    o.q = v


def out_of_order(n):
    out = []
    first = Ord()
    first.p = 1                 # the class's keys: p, q, r
    first.q = 2
    first.r = 3
    for i in range(n):
        o = Ord()
        if i % 3 == 0:
            o.q = i             # q before p: CPython converts the dict
            o.p = i
        elif i % 3 == 1:
            o.p = i
            o.r = i             # skips q: converts too
            o.q = i
        else:
            o.p = i
            o.q = i
            o.r = i
        out.append(list(o.__dict__.items()))
    return out


def skip_a_slot(n):
    # a split dict missing q, on keys p, q, r, gets r: its empty slot isn't
    # at ma_used, so CPython converts it (the first conversion also drops
    # the class's keys, hence a fresh class per call)
    C = type('Skip', (), {})
    a, b = C(), C()
    a.p = 0
    b.p = b.q = b.r = 0
    _store_r(a, n)
    _store_q(a, n + 1)
    return list(a.__dict__.items()), list(b.__dict__.items())


class Big:
    def __init__(self):
        self.k = 0              # 12 names: the class's keys grow to 32 slots
        self.a1 = 1
        self.a2 = 2
        self.a3 = 3
        self.a4 = 4
        self.a5 = 5
        self.a6 = 6
        self.a7 = 7
        self.a8 = 8
        self.a9 = 9
        self.a10 = 10
        self.a11 = 11


class Small:
    def __init__(self):
        self.z = 0
        self.k = 0              # 8 slots, k at index 1


def _store_kk(o, v):
    o.k = v


def size_mismatch(n):
    # at the cached entry offset for 32-slot keys, 8-slot keys hold entry
    # ix + 1's key: only the size check stops a store into the wrong slot
    out = []
    for i in range(n):
        big = Big()
        _store_kk(big, i)
        o = Big()
        o.__dict__ = Small().__dict__
        _store_kk(o, -i)
        out.append((sorted(o.__dict__.items()), big.k))
    return out


class Same:
    def __init__(self):
        self.x = object()


def same_object(n):
    o = Same()
    v = o.x
    before = sys.getrefcount(v)
    for i in range(n):
        o.x = o.x
    return sys.getrefcount(v) - before, o.x is v


class Logged:
    def __init__(self, o, log):
        self.o = o
        self.log = log

    def __del__(self):
        self.log.append(self.o.x if isinstance(self.o.x, int) else 'obj')


class Holder:
    pass


def old_value_freed(n):
    log = []
    o = Holder()
    o.x = 0
    for i in range(n):
        o.x = Logged(o, log)
        o.x = i                 # frees the Logged: its __del__ sees i
    return log, o.x


class Mutable:
    pass


class Changer:
    def __init__(self, what):
        self.what = what

    def __del__(self):
        if self.what == 'setattr':
            Mutable.__setattr__ = lambda self, k, v: object.__setattr__(
                self, k, ('hooked', v))
        elif self.what == 'property':
            Mutable.y = property(lambda self: 'prop',
                                 lambda self, v: None)


def _store_y(o, v):
    o.y = v


def del_changes_class(n):
    out = []
    for what in ('setattr', 'property'):
        for k in ('__setattr__', 'y'):
            if k in Mutable.__dict__:
                delattr(Mutable, k)
        o = Mutable()
        o.y = 0
        for i in range(n):
            _store_y(o, i)
        _store_y(o, Changer(what))
        _store_y(o, 'gone')     # the Changer's __del__ changes the class
        _store_y(o, 'next')     # must use the new __setattr__ or property
        out.append((o.y, sorted(o.__dict__.items())))
    for k in ('__setattr__', 'y'):
        if k in Mutable.__dict__:
            delattr(Mutable, k)
    return out


class Replacer:
    def __init__(self, o, how):
        self.o = o
        self.how = how

    def __del__(self):
        if self.how == 'dict':
            self.o.__dict__ = {'fresh': 1}
        else:
            del self.o.other


class Victim:
    pass


def _store_v(o, v):
    o.v = v


def del_replaces_dict(n):
    out = []
    for how in ('dict', 'delete'):
        for i in range(n):
            o = Victim()
            o.v = 0
            o.other = 1
            _store_v(o, Replacer(o, how))
            _store_v(o, i)      # the Replacer's __del__ runs here
            _store_v(o, i + 1)
            out.append((o.v, sorted(o.__dict__.items())))
    return out


class Floats:
    def __init__(self):
        self.a = 1.0
        self.b = 2.0


def _store_b(o, v):
    o.b = v


def gc_tracking(n):
    out = []
    for i in range(n):
        o = Floats()
        gc.collect()
        out.append(gc.is_tracked(o.__dict__))
        _store_b(o, 3.0)        # not a GC value: stays untracked
        out.append(gc.is_tracked(o.__dict__))
        _store_b(o, (1, 2))     # an untracked tuple
        out.append(gc.is_tracked(o.__dict__))
        gc.collect()
        _store_b(o, [i])        # a list: CPython tracks the dict
        out.append(gc.is_tracked(o.__dict__))
        gc.collect()
        _store_b(o, o.__dict__)
        out.append(gc.is_tracked(o.__dict__))
        del o.__dict__['b']
    return out


class Cycle:
    def __init__(self):
        self.a = 1
        self.b = 2


def self_cycle(n):
    import weakref
    out = []
    for i in range(n):
        o = Cycle()
        gc.collect()            # untracks the dict of ints
        o.b = o                 # a cycle: the dict must be tracked
        r = weakref.ref(o)
        del o
        gc.collect()
        out.append(r() is None)
    return out


class G:
    def __init__(self):
        self.x = 1
        self.y = 2


_gns = {}
exec(compile('def get():\n    return x + y\n', '<g>', 'exec'), _gns)
_GET = _gns['get'].__code__


def globals_is_instance_dict(n):
    o = G()
    get = types.FunctionType(_GET, o.__dict__)
    out = []
    for i in range(n):
        out.append(get())
        o.x = i * 10            # an inline store must change the tag
        out.append(get())
    return out


class MG:
    def __init__(self):
        self.x = 0


_mod_inst = MG()


def module_instance(n):
    out = []
    for i in range(n):
        _mod_inst.x = i
        out.append(_mod_inst.x)
    return out


class Other:
    def __init__(self):
        self.y = 'other y'
        self.x = 'other x'


class DSub(dict):
    def __setitem__(self, k, v):
        raise RuntimeError('not called by attribute stores')


class Rep:
    def __init__(self):
        self.x = 0
        self.y = 0


def _store_xy(o, i):
    o.x = i
    o.y = -i


def dict_replaced(n):
    out = []
    for i in range(n):
        o = Rep()
        o.__dict__ = {'z': 1}
        _store_xy(o, i)
        out.append(sorted(o.__dict__.items()))
        o = Rep()
        o.__dict__.clear()
        _store_xy(o, i)
        out.append(sorted(o.__dict__.items()))
        o = Rep()
        p = Other()
        o.__dict__ = p.__dict__     # a split dict on another class's keys
        _store_xy(o, i)
        out.append((sorted(o.__dict__.items()), p.__dict__ is o.__dict__))
        o = Rep()
        o.__dict__ = DSub(x=5)
        _store_xy(o, i)
        out.append((type(o.__dict__).__name__, sorted(o.__dict__.items())))
    return out


class AfterDel:
    def __init__(self, i):
        self.x = i
        self.y = i


def after_delete(n):
    old = [AfterDel(i) for i in range(3)]
    del old[0].x                # converts that dict; the class drops its keys
    out = []
    for i in range(n):
        for o in old:
            _store_xy(o, i)
        new = AfterDel(i)
        _store_xy(new, i + 1)
        out.append([sorted(o.__dict__.items()) for o in old + [new]])
    return out


def reshare(n):
    C = type('Grow', (), {})
    first = C()
    first.x = 0
    first.y = 0
    old = C()                   # on the class's first keys
    old.x = old.y = 1
    grow = C()
    for k in 'abcdefgh':        # grows its own keys past 8 slots
        setattr(grow, k, k)
    newer = C()
    out = []
    for i in range(n):
        for o in (first, old, newer):
            _store_xy(o, i)
        out.append([sorted(o.__dict__.items()) for o in (first, old, newer)])
    return out


class Joins:
    pass


def _store_late(o, v):
    o.late = v


def name_joins_keys(n):
    out = []
    for i in range(n):
        o = Joins()
        o.early = i
        _store_late(o, i)       # compiled before any instance had 'late'
        out.append(list(o.__dict__.items()))
    return out


class EqKey:
    pass


def equal_not_identical_key(n):
    out = []
    for i in range(n):
        o = EqKey()
        o.__dict__[''.join(['x', 'y'])] = 1
        o.xy = i
        out.append(list(o.__dict__.items()))
    return out


class Guarded:
    def __init__(self):
        self.s = 0


class HeapD:
    pass


def type_guard(n):
    out = []
    for k in ('__setattr__', 's', 'h'):
        if k in Guarded.__dict__:
            delattr(Guarded, k)
    if '__set__' in HeapD.__dict__:
        del HeapD.__set__
    g = Guarded()
    Guarded.h = HeapD()
    for i in range(n):
        g.s = i
        g.h = i
        if i == 1:
            Guarded.__setattr__ = lambda self, k, v: object.__setattr__(
                self, k, -v)
        if i == 2:
            del Guarded.__setattr__
            HeapD.__set__ = lambda self, obj, v: obj.__dict__.__setitem__(
                'via_set', v)
        if i == 3:
            Guarded.s = property(lambda self: 'p', lambda self, v: None)
        out.append(sorted(g.__dict__.items()))
    for k in ('s', 'h'):
        delattr(Guarded, k)
    del HeapD.__set__
    try:
        object().s = 1
    except AttributeError as e:
        out.append(str(e))
    return out


def _store_k(o, v):
    o.k = v


def two_classes_one_site(n):
    A = type('A', (), {})
    B = type('B', (), {})
    a, b = A(), B()
    b.other = 0                 # 'k' at another index in B's keys
    out = []
    for i in range(n):
        _store_k(a, i)
        _store_k(b, -i)
        out.append((a.__dict__, list(b.__dict__.items())))
    return out


class Long:
    def __init__(self):
        self.x = 'long'


def class_address_reuse(n):
    # classes whose keys hold the same names in other orders are made and
    # freed, so their keys' memory is reused; stores through compiled sites
    # into a long-lived class must still land in its own slots
    out = []
    keep = Long()
    for i in range(n):
        for j in range(30):
            T = type('T%d' % (j % 2), (), {})
            t = T()
            if j % 2:
                t.__dict__['y'] = j
                t.__dict__['x'] = j
            else:
                t.__dict__['x'] = j
                t.__dict__['y'] = j
            del t, T
        gc.collect()
        o = Long()
        _store_xy(o, i)
        _store_xy(keep, i)
        out.append((list(o.__dict__.items()), list(keep.__dict__.items())))
    return out


class Wide:
    pass


_WIDE = compile('def fill(o, i):\n' +
                ''.join('    o.a%d = i + %d\n' % (k, k) for k in range(200)),
                '<wide>', 'exec')
_wns = {}
exec(_WIDE, _wns)
_fill_wide = _wns['fill']


def _store_a199(o, v):
    o.a199 = v


def wide_keys(n):
    out = []
    for i in range(n):
        o = Wide()
        _fill_wide(o, i)
        _store_a199(o, -i)
        out.append((len(o.__dict__), o.a0, o.a199, list(o.__dict__)[-1]))
    return out


class Thrash:
    def __init__(self):
        self.x = 0
        self.y = 0


class Rev:
    def __init__(self):
        self.y = 0
        self.x = 0


def _store_x(o, v):
    o.x = v


def miss_cap(n):
    # o shares a Rev instance's dict, where x is at another index: stores
    # to o and to a real Thrash alternate between two key sets, so the
    # (Thrash, x) cache would be refreshed on every miss without the cap
    r0 = _stats('store_refreshes')
    t = Thrash()
    o = Thrash()
    o.__dict__ = Rev().__dict__
    for i in range(n):
        _store_x(t, i)
        _store_x(o, -i)
    r = _stats('store_refreshes') - r0
    return t.__dict__, o.__dict__, r <= 70


class SL:
    def __init__(self):
        self.a = 1


def store_then_load(n):
    out = []
    for i in range(n):
        o = SL()
        o.a = i                 # A
        o.b = i * 2             # B (after the first instance)
        out.append((o.a, o.b, o.a + o.b))
    return out


# Stores in sequence to one object, in one function: what a check shared
# between them (STORES.md, "Shrinking the store sites") would have to get
# right. The old value of the first store frees an object whose __del__
# changes what the next stores must do.
class Seq:
    def __init__(self):
        self.a = 0
        self.b = 0
        self.c = 0


class SeqOther:
    pass


class SeqDel:
    def __init__(self, o, how):
        self.o = o
        self.how = how

    def __del__(self):
        o, how = self.o, self.how
        if how == 'setattr':
            Seq.__setattr__ = lambda self, k, v: object.__setattr__(
                self, k, ('hooked', v))
        elif how == 'property':
            Seq.b = property(lambda self: 'prop', lambda self, v: None)
        elif how == 'dict':
            o.__dict__ = {'fresh': 1}
        elif how == 'delete':
            del o.b
        elif how == 'class':
            o.__class__ = SeqOther
        elif how == 'grow':
            for k in range(20):
                setattr(o, 'g%d' % k, k)


def _seq_reset():
    for k in ('__setattr__', 'b'):
        if k in Seq.__dict__:
            delattr(Seq, k)


def _seq3(o, i):
    o.a = i
    o.b = i + 1
    o.c = i + 2


def run_del_mid_sequence(n, hows):
    out = []
    for how in hows:
        _seq_reset()
        o = Seq()
        for i in range(n):
            _seq3(o, i)
        o.a = SeqDel(o, how)
        _seq3(o, 100)           # o.a's old value runs __del__ first
        _seq3(o, 200)
        out.append((how, sorted(o.__dict__.items())))
    _seq_reset()
    return out


def _seq_poke(o, how):
    if how == 'setattr':
        Seq.__setattr__ = lambda self, k, v: object.__setattr__(
            self, k, ('hooked', v))
    elif how == 'dict':
        o.__dict__ = {}
    elif how == 'class':
        o.__class__ = SeqOther
    return 1


def _seq_calls(o, i, how):
    o.a = i
    o.b = _seq_poke(o, how)
    o.c = i
    len(o.__dict__)
    o.a = i + 1


def run_interleaved_calls(n, hows):
    out = []
    for how in hows:
        _seq_reset()
        o = Seq()
        for i in range(n):
            _seq_calls(o, i, 'none')
        _seq_calls(o, 50, how)
        _seq_calls(o, 60, 'none')
        out.append((how, sorted(o.__dict__.items())))
    _seq_reset()
    return out


def _seq_two(p, q, i):
    p.a = i
    q.a = i * 10
    p.b = q.a
    q.b = p.a
    p.c = (i,)
    q.c = None


def run_two_objects(n):
    out = []
    p, q = Seq(), Seq()
    for i in range(n):
        _seq_two(p, q, i)
    out.append((sorted(p.__dict__.items()), sorted(q.__dict__.items())))
    _seq_two(p, p, 7)           # the same object twice
    out.append(sorted(p.__dict__.items()))
    r = Seq()
    r.__dict__ = {'a': 1}       # a combined dict beside a split one
    _seq_two(p, r, 8)
    out.append((sorted(p.__dict__.items()), sorted(r.__dict__.items())))
    q.a = SeqDel(p, 'dict')     # q's store frees it: p's dict is replaced
    _seq_two(p, q, 9)
    out.append((sorted(p.__dict__.items()), sorted(q.__dict__.items())))
    return out


def _to_other(o):
    o.__class__ = SeqOther


def class_assigned_in_call(n):
    # F55: a promoted type is kept after a call assigns __class__
    out = []
    for i in range(n):
        o = Seq()
        len(o.__dict__)
        _to_other(o)
        out.append(type(o).__name__)
    return out


CASES = [
    ('overwrite_loop', [(20,)], {}),
    ('init_fills', [(50,)], {}),
    ('guard_holds', [(50,)], {}),
    ('many_instances', [(500,)], {}),
    ('out_of_order', [(9,)], {}),
    ('skip_a_slot', [(3,)], {'leaks': 'C14 (2): a class per call, held by compiled code'}),
    ('size_mismatch', [(4,)], {}),
    ('same_object', [(10,)], {}),
    ('old_value_freed', [(5,)], {}),
    ('del_changes_class', [(4,)], {}),
    ('del_replaces_dict', [(3,)], {}),
    ('gc_tracking', [(3,)], {}),
    ('self_cycle', [(3,)], {}),
    ('globals_is_instance_dict', [(5,)], {}),
    ('module_instance', [(5,)], {}),
    ('dict_replaced', [(12,)], {}),
    ('after_delete', [(3,)], {}),
    ('reshare', [(3,)], {'leaks': 'C14 (2): a class per call, held by compiled code'}),
    ('name_joins_keys', [(4,)], {}),
    ('equal_not_identical_key', [(3,)], {}),
    ('type_guard', [(5,)], {}),
    ('two_classes_one_site', [(4,)],
     {'leaks': 'C14 (2): classes per call, held by compiled code'}),
    ('class_address_reuse', [(4,)], {}),
    ('wide_keys', [(3,)], {}),
    ('miss_cap', [(200,)], {}),
    ('store_then_load', [(6,)], {}),
    ('run_del_mid_sequence',
     [(4, ('setattr', 'property', 'dict', 'delete', 'grow'))], {}),
    ('run_interleaved_calls', [(4, ('none', 'setattr', 'dict', 'none'))], {}),
    # __class__ assigned in the sequence (F55, D59): these passed only while
    # an earlier round left the load guard of o.__dict__ stale for good (D60)
    ('run_del_mid_sequence', [(4, ('setattr', 'class'))], {}),
    ('run_interleaved_calls', [(4, ('none', 'setattr', 'class'))], {}),
    ('run_two_objects', [(4,)], {}),
    ('class_assigned_in_call', [(3,)], {}),
]
