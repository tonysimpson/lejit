# Two translated-layer differences from 3.7 that CPython's tests found once
# their methods compiled: a class subscripted (PEP 560 __class_getitem__,
# typing.Generic[T]) raised "'type' object is not subscriptable", and a
# tuple subclass plus () gave the subclass instance back, where tupleconcat
# returns an operand only if it is an exact tuple.
import typing


class G:
    def __class_getitem__(cls, item):
        return ('G', item)


class MadTuple(tuple):
    pass


T = typing.TypeVar('T')


def class_getitem(k):
    return G[k], G[k, 1]


def class_getitem_arg(cls, k):
    return cls[k]


def generic_alias(k):
    S = typing.TypeVar('S')
    return (repr(typing.Generic[T]), repr(typing.Generic[S]),
            repr(typing.List[int]))


def not_subscriptable(x):
    return x[0]


def concat_types(a, b):
    r1 = a + ()
    r2 = () + b
    r3 = a + b
    return type(r1).__name__, type(r2).__name__, type(r3).__name__, r1, r2, r3


def concat_identity(a):
    return (a + ()) is a, (() + a) is a


CASES = [
    ('class_getitem', [(1,), ('x',)], {}),
    ('class_getitem_arg', "[(G, 1), (typing.Generic, T), (int, 1)]", {}),
    ('generic_alias', [(0,)], {}),
    ('not_subscriptable', "[(5,), (G,), (int,), (type,), (None,)]", {}),
    ('concat_types', "[(MadTuple((1, 2)), MadTuple((3,))), ((1,), (2,)), "
                     "(MadTuple(), MadTuple()), ((), MadTuple((4,)))]", {}),
    ('concat_identity', "[((1, 2),), (MadTuple((1,)),), ((),)]", {}),
]
