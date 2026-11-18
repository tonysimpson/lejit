# Iterators and the builtins over them, with generated metas:
# PyObject_SelfIter (tp_iter of sequence and range iterators, object.c).


def selfiter(n):
    it = iter([1, 2, 3][:n])
    r = iter(range(n))
    out = [iter(it) is it, iter(r) is r]
    for x in iter(it):
        out.append(x)
    for x in iter(r):
        out.append(x)
    return out


CASES = [
    ('selfiter', [(0,), (2,), (3,)], {}),
]
