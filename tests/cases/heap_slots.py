# A class's slots change when a special method is set or deleted on it,
# after code using them is compiled (KNOWN_BUGS F56, the rest of F50):
# __bool__ or __len__ in a truth test, the number slots, __contains__,
# __getitem__/__setitem__, len() and __call__. Compiled code must see
# the class as it is when it runs.


def truth(x):
    if x:
        return 'true'
    return 'false'


class T:
    pass


def added_bool():
    t = T()
    r = [truth(t)]
    T.__bool__ = lambda self: False
    r.append(truth(t))
    del T.__bool__
    r.append(truth(t))
    T.__len__ = lambda self: 0
    r.append(truth(t))
    del T.__len__
    r.append(truth(t))
    return r


class TL:
    def __len__(self):
        return 0


def deleted_len():
    t = TL()
    r = [truth(t)]
    f = TL.__len__
    del TL.__len__
    r.append(truth(t))
    TL.__len__ = f
    return r


def add(a, b):
    return a + b


def mul(a, b):
    return a * b


def iadd(a, b):
    a += b
    return a


def neg(a):
    return -a


def attempt(f, *args):
    try:
        return f(*args)
    except TypeError as e:
        return str(e)


class N:
    pass


def added_add():
    a = N()
    r = [attempt(add, a, 1), attempt(add, 1, a), attempt(neg, a),
         attempt(iadd, a, 1)]
    N.__add__ = lambda self, o: ('add', o)
    N.__radd__ = lambda self, o: ('radd', o)
    N.__neg__ = lambda self: 'neg'
    r += [attempt(add, a, 1), attempt(add, 1, a), attempt(neg, a),
          attempt(iadd, a, 1)]
    N.__iadd__ = lambda self, o: ('iadd', o)
    r.append(attempt(iadd, a, 1))
    del N.__add__, N.__radd__, N.__neg__, N.__iadd__
    r += [attempt(add, a, 1), attempt(add, 1, a), attempt(neg, a),
          attempt(iadd, a, 1)]
    return r


class I(int):
    pass


def overridden_int():
    a = I(2)
    r = [add(a, 1), mul(a, 3), neg(a)]
    I.__add__ = lambda self, o: 'add'
    I.__mul__ = lambda self, o: 'mul'
    I.__neg__ = lambda self: 'neg'
    r += [add(a, 1), mul(a, 3), neg(a)]
    del I.__add__, I.__mul__, I.__neg__
    r += [add(a, 1), mul(a, 3), neg(a)]
    return r


class L(list):
    pass


def overridden_list():
    a = L([1])
    r = [add(a, [2]), mul(a, 2)]
    L.__add__ = lambda self, o: 'add'
    L.__mul__ = lambda self, o: 'mul'
    r += [add(a, [2]), mul(a, 2)]
    L.__add__ = L.__mul__ = lambda self, o: NotImplemented
    r += [attempt(add, a, [2]), attempt(mul, a, 2), attempt(mul, 2, a)]
    del L.__add__, L.__mul__
    r += [add(a, [2]), mul(a, 2), mul(2, a)]
    return r


CASES = [
    ('added_bool', [()], {}),
    ('deleted_len', [()], {}),
    ('added_add', [()], {}),
    ('overridden_int', [()], {}),
    ('overridden_list', [()], {}),
]
