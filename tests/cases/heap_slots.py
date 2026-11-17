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


CASES = [
    ('added_bool', [()], {}),
    ('deleted_len', [()], {}),
]
