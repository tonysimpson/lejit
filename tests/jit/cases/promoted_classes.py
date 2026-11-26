# A class made per call and promoted by long-lived code dies once no frame
# runs in the code compiled for it: what the code compiled for it is held by a group of
# its own, which the owner scan frees (codemanager.h, le_group_t).
import gc
import sys
import weakref


def make(k):
    class C:
        def __init__(self, v):
            self.v = v

        def get(self):
            return self.v

        def __len__(self):
            return self.v

        def __add__(self, other):
            return self.v + other

    C.__name__ = 'C%d' % k
    return C


# each has a loop, so none is inlined into its caller
def by_attribute(objs):
    t = 0
    for o in objs:
        t += o.v
    return t


def by_method(objs):
    t = 0
    for o in objs:
        t += o.get()
    return t


def by_len(objs):
    t = 0
    for o in objs:
        t += len(o)
    return t


def by_add(objs):
    t = 0
    for o in objs:
        t += o + 1
    return t


def by_type(objs):
    t = 0
    for o in objs:
        t += len(type(o).__name__)
    return t


def by_genexpr(objs):
    return sum(len(o) for o in objs)


USERS = [by_attribute, by_method, by_len, by_add, by_type, by_genexpr]


def one_round(k, users):
    C = make(k)
    objs = [C(1), C(k)]
    t = 0
    for f in users:
        t += f(objs)
    return t, weakref.ref(C)


def dying_classes(users):
    refs = []
    total = 0
    for k in range(20):
        t, r = one_round(k, users)
        total += t
        refs.append(r)
    gc.collect()
    return total, sum(r() is None for r in refs)


def classes_die():
    return dying_classes([by_len, by_type, by_genexpr])


def classes_die_at_loop_heads():
    return dying_classes([by_attribute]), dying_classes([by_method])


def classes_die_through_slots():
    return dying_classes([by_add])


def collected_while_running(n):
    # the classes are promoted by this frame's own code, which is running
    # when it collects, but not in the code compiled for any of them
    refs = []
    for i in range(n):
        class D:
            pass
        o = D()
        o.x = i
        refs.append(weakref.ref(D))
        del o, D
    gc.collect()
    return sum(r() is None for r in refs)


def code_made():
    lejit = sys.modules.get('lejit')
    if lejit is None:
        return 0
    return lejit._core._stats()['code_buffers']


def megamorphic_stays():
    # past MEGAMORPHIC_MAX classes each site goes to its megamorphic
    # version; once their groups are freed, a new class must go there too,
    # not compile again. A site in a freed group's code goes with it and
    # counts again, so the sites inside go megamorphic later. The genexpr's
    # sites have no limit.
    total = 0
    grown = []
    for f in USERS[:-1]:
        made = []
        for i in range(50):
            total += one_round(i, [f])[0]
            gc.collect()
            made.append(code_made())
        grown.append(made[-1] - made[29] < 3)
    return total, grown


CASES = [
    ('classes_die', [()], {}),
    ('classes_die_at_loop_heads', [()], {}),
    ('classes_die_through_slots', [()],
     {'xfail': "__add__ called through the slot is compiled for each "
               "class under its own code object"}),
    ('collected_while_running', [(20,)], {}),
    ('megamorphic_stays', [()], {'checked': 'reads lejit._core._stats'}),
]
