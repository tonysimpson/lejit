# Loops whose paths reach a merge point knowing promoted types that the
# block already there was compiled without (SPECIALISATION.md rule 1):
# branches compiled late (coding pauses, respawns) and joins in the middle
# of an expression, where the instruction at the merge point itself promotes.
# Dropping facts that the current block used, without the guard on a
# promotion's own compare (psyco_compatible_keep_facts), loops forever in
# compiled code on every one of these. Results must not change.


class P:
    def __init__(self, a, b, v):
        self.a = a
        self.b = b
        self.v = v


class Q(P):
    pass


def objs(n):
    out = []
    for i in range(n):
        cls = Q if i % 7 == 3 else P
        a = i % 3 == 0
        b = [] if i % 5 else [i]
        v = i * 0.5 if i % 11 == 4 else i
        out.append(cls(a, b, v))
    return out


def or_join(items):
    # the join after 'or' is the POP_JUMP_IF_FALSE, which promotes the
    # type of the value it tests
    n = 0
    for o in items:
        if o.a or o.b:
            n += 1
    return n


def and_join(items):
    n = 0
    t = 0
    for o in items:
        if o.a and o.b and o.v:
            n += 1
        t += 1 if (o.b or o.a) else 2
    return n, t


def ifexp_attr(items):
    # the join after a conditional expression is the LOAD_ATTR on its value
    t = 0
    for o in items:
        t += (o if o.a else items[0]).v
    return t


def pause_paths(items):
    # a rare branch, compiled when first taken, promotes the type of 'v'
    # and reaches the loop head knowing it
    t = 0
    s = 0
    for o in items:
        v = o.v
        if o.b:
            s += v * 2
            if type(o) is Q:
                s -= 1
        t += v
    return t, s


def nested_loops(items, k):
    t = 0
    for j in range(k):
        for o in items:
            x = o.v
            if o.a:
                x = x + j
            elif o.b:
                x = -x
            t = t + x
    return t


def while_chain(items):
    # a while loop whose head tests an object's attribute
    i = 0
    t = 0
    o = items[0]
    while o.v is not None and i < len(items):
        if o.a:
            t += o.v
        else:
            t -= 1
        i += 1
        o = items[i] if i < len(items) else P(0, 0, None)
    return i, t


CASES = [
    ('or_join', 'map(lambda n: (objs(n),), (5, 40, 200))', {}),
    ('and_join', 'map(lambda n: (objs(n),), (5, 40, 200))', {}),
    ('ifexp_attr', 'map(lambda n: (objs(n),), (5, 40, 200))', {}),
    ('pause_paths', 'map(lambda n: (objs(n),), (3, 40, 200))', {}),
    ('nested_loops', 'map(lambda n: (objs(n), 3), (3, 40, 120))', {}),
    ('while_chain', 'map(lambda n: (objs(n),), (2, 40, 200))', {}),
]
