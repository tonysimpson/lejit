# A class's slots can change after code using them is compiled: deleting
# __next__ makes tp_iternext _PyObject_NextNotImplemented (TypeError),
# and setting __hash__, __eq__, __ne__ or __iter__ on the class changes
# tp_hash, tp_richcompare or tp_iter (KNOWN_BUGS F50). Compiled code reads
# a heap type's slots at run time, as CPython does.


class It:
    def __init__(self, n):
        self.i = 0
        self.n = n

    def __iter__(self):
        return self

    def __next__(self):
        if self.i < self.n:
            self.i += 1
            return self.i
        raise StopIteration


def drain(it):
    r = []
    for x in it:
        r.append(x)
    return r


class C:
    def __init__(self, n):
        self.i = 0
        self.n = n

    def __iter__(self):
        return self


def with_next(self):
    return It.__next__(self)


def deleted_next(n):
    C.__next__ = with_next
    r = [drain(C(n))]
    del C.__next__
    try:
        r.append(drain(C(n)))
    except TypeError as e:
        r.append(str(e))
    try:
        r.append(next(C(n)))
    except TypeError as e:
        r.append(str(e))
    return r


class Bad:
    # test_iter's test_3720: an iterator that deletes its own __next__
    def __iter__(self):
        return self


def bad_next(self):
    del Bad.__next__
    return 1


def self_deleting():
    Bad.__next__ = bad_next
    r = []
    try:
        for x in Bad():
            r.append(x)
    except TypeError as e:
        r.append(str(e))
    return r


def hash_of(x):
    return hash(x)


def eq(a, b):
    return a == b, a != b


class H:
    pass


def changed_hash():
    h = H()
    r = [hash_of(h) == object.__hash__(h)]
    H.__hash__ = lambda self: 42
    r.append(hash_of(h))
    del H.__hash__
    r.append(hash_of(h) == object.__hash__(h))
    return r


class E:
    pass


def changed_compare():
    a, b = E(), E()
    r = [eq(a, b), eq(a, a)]
    E.__eq__ = lambda self, other: 'eq'
    r.append(eq(a, b))
    E.__ne__ = lambda self, other: 'ne'
    r.append(eq(a, b))
    del E.__eq__, E.__ne__
    r.append(eq(a, b))
    return r


class S:
    def __getitem__(self, i):
        if i < 2:
            return i
        raise IndexError


def changed_iter():
    r = [drain(S())]
    S.__iter__ = lambda self: iter('ab')
    r.append(drain(S()))
    del S.__iter__
    return r


CASES = [
    ('deleted_next', [(2,)], {}),
    ('self_deleting', [()], {}),
    ('changed_hash', [()], {}),
    ('changed_compare', [()], {}),
    ('changed_iter', [()], {}),
]
