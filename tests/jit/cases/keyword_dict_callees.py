# Calls of functions taking **kwargs, bound by the binder: the keyword
# arguments no parameter takes go into a new dict, in the call's order, as
# CPython's binder makes it (psyfunc.c:_PsycoEval_EvalCodeWithName).


def kw(a, b=0, **rest):
    return a, b, rest, list(rest)


def kw_only_rest(**rest):
    rest['added'] = 1        # the callee's own dict: changing it is fine
    return rest


def star_both(*args, **kwargs):
    return args, kwargs


class Obj:
    def m(self, x, **opts):
        return x, sorted(opts.items())


def bound(n):
    o = Obj()
    return [(kw(i), kw(i, 1), kw(i, z=1, y=2), kw(b=2, a=i, c=3),
             kw_only_rest(), kw_only_rest(q=i), star_both(i, k=i),
             star_both(), o.m(i, p=1)) for i in range(n)]


def fresh_each_call(n):
    # every call gets its own dict
    out = [kw_only_rest(v=i) for i in range(n)]
    return out, len(set(map(id, out))) == len(out)


# the syntax is 3.8's: made where the code object has the count
POSONLY = {}
if hasattr(kw.__code__, 'co_posonlyargcount'):
    exec('def po(a, /, **kw):\n    return a, kw\n', POSONLY)


def positional_only_names(n):
    if not POSONLY:
        return None
    po = POSONLY['po']
    return [po(i, a=2) for i in range(n)]


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: kw(i, a=1), lambda: kw(), lambda: kw(1, 2, 3)):
            try:
                out.append(f())
            except TypeError as e:
                out.append(str(e))
    return out


def as_cell(n):
    # **kwargs read by an inner function: its slot is a cell
    def outer(**k):
        def get():
            return k
        return get()
    return [outer(x=i) for i in range(n)]


def many(n):
    # more names than the binder takes at once
    return [kw(i, k0=0, k1=1, k2=2, k3=3, k4=4, k5=5, k6=6, k7=7, k8=8,
               k9=9, k10=10, k11=11, k12=12, k13=13, k14=14, k15=15, k16=16)
            for i in range(n)]


CASES = [
    ('bound', [(0,), (3,)], {}),
    ('fresh_each_call', [(3,)], {}),
    ('positional_only_names', [(3,)], {}),
    ('errors', [(2,)], {}),
    ('as_cell', [(3,)], {}),
    ('many', [(2,)], {}),
]
