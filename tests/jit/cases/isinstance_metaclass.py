# isinstance(x, C) where C's metaclass isn't type (pbltinmodule.c:
# isinstance_known_c): folded where the metaclass finds type's own
# __instancecheck__, under a guard of that lookup, and decided by CPython
# where it finds another, or where a test of the recursion limit could fail.
import abc
import ctypes
import enum
import numbers
import re
import sys
import types


class Meta(type):
    pass


class C(metaclass=Meta):
    pass


class D(C):
    pass


class Always(type):
    def __instancecheck__(cls, obj):
        return True


class MetaMeta(type):
    pass


class Meta2(type, metaclass=MetaMeta):
    def __getattribute__(cls, name):
        return type.__getattribute__(cls, name)


class E(metaclass=Meta2):
    pass


class Color(enum.Enum):
    RED = 1


class Bits(enum.IntFlag):
    A = 1
    B = 2


class Integral(abc.ABC):
    pass


class Claims:
    @property
    def __class__(self):
        return C


PAIR = (str, Bits)
NESTED = ((bytes,), (Color, int))
UNION = (Bits | None) if hasattr(types, 'UnionType') else (Bits, type(None))


def enum_classes():
    out = []
    for x in (0, 7, 'x', None, 1.5, True, re.I, Bits.A, Color.RED, b''):
        out.append((isinstance(x, re.RegexFlag), isinstance(x, Bits),
                    isinstance(x, Color), isinstance(x, enum.Enum),
                    isinstance(x, enum.IntFlag)))
    return out


def plain_metaclass():
    out = []
    for x in (0, 'a', C(), D(), E(), C, None):
        out.append((isinstance(x, C), isinstance(x, D), isinstance(x, E),
                    isinstance(x, Meta)))
    return out


def overriding_metaclass():
    # ABCMeta and Always define __instancecheck__: CPython calls them
    Integral.register(float)
    out = []
    for x in (0, 1.5, 'a', True):
        out.append((isinstance(x, numbers.Integral), isinstance(x, Integral),
                    isinstance(x, numbers.Number)))
    return out


def registered_later(n):
    # an ABC's answer changes with register(), after compiling
    class Late(abc.ABC):
        pass
    out = []
    for i in range(n):
        if i == n // 2:
            Late.register(int)
        out.append(isinstance(i, Late))
    return out


def _check(x):
    return isinstance(x, C), isinstance(x, (str, C))


def check_added_to_metaclass(n):
    # the metaclass gains __instancecheck__ after the code is compiled,
    # then loses it
    out = []
    try:
        for i in range(n):
            if i == n // 3:
                Meta.__instancecheck__ = lambda cls, obj: obj == 2
            if i == 2 * n // 3:
                del Meta.__instancecheck__
            out.append(_check(i))
    finally:
        if '__instancecheck__' in Meta.__dict__:
            del Meta.__instancecheck__
    return out


class Swapped(metaclass=Meta):
    pass


def metaclass_assigned(n):
    # the class's own metaclass is replaced, then put back
    out = []
    try:
        for i in range(n):
            if i == n // 3:
                Swapped.__class__ = Always
            if i == 2 * n // 3:
                Swapped.__class__ = Meta
            out.append((isinstance(i, Swapped), isinstance('s', Swapped)))
    finally:
        Swapped.__class__ = Meta
    return out


def class_property():
    # object_isinstance asks obj.__class__ after the type's own test
    out = []
    for x in (Claims(), 0):
        out.append((isinstance(x, C), isinstance(x, D), isinstance(x, int),
                    isinstance(x, (str, C))))
    return out


def tuples():
    out = []
    for x in (0, 'a', Bits.B, Color.RED, b'', None):
        out.append((isinstance(x, (str, re.RegexFlag)),
                    isinstance(x, (re.RegexFlag, int)), isinstance(x, PAIR),
                    isinstance(x, NESTED), isinstance(x, ((), (C, (Color,)))),
                    isinstance(x, (numbers.Integral, Color)),
                    isinstance(x, ())))
    return out


def unions():
    out = []
    for x in (0, Bits.A, None, 'a'):
        out.append((isinstance(x, UNION), isinstance(x, (UNION, str))))
    return out


def static_metaclass():
    # ctypes' metaclasses: static types on some versions
    return [(isinstance(x, ctypes.c_int), isinstance(x, ctypes.Structure))
            for x in (0, ctypes.c_int(1), 'a')]


def bad_class():
    out = []
    for cls in (1, (int, 1), (1, int), 'a'):
        try:
            out.append(isinstance(0, cls))
        except TypeError as e:
            out.append(str(e))
    return out


def _down_meta(n):
    isinstance(0, C)
    return _down_meta(n + 1)


def _down_tuple(n):
    isinstance(0, (str, int))
    return _down_tuple(n + 1)


def _down_nested(n):
    isinstance(0, (str, (bytes, int)))
    return _down_nested(n + 1)


def _down_tuple_meta(n):
    isinstance(0, (str, C))
    return _down_tuple_meta(n + 1)


def _down_enum(n):
    isinstance(0, re.RegexFlag)
    return _down_enum(n + 1)


def recursion_limit():
    # CPython's tests of the recursion limit in isinstance: one for each
    # tuple, two for a metaclass's __instancecheck__ (the checker, and the
    # call of it). Nothing else in the recursion tests the limit (a
    # comparison would, in CPython)
    old = sys.getrecursionlimit()
    sys.setrecursionlimit(300)
    out = []
    try:
        for i in range(2):
            for f in (_down_meta, _down_tuple, _down_nested,
                      _down_tuple_meta, _down_enum):
                try:
                    f(0)
                except RecursionError as e:
                    out.append(str(e))
    finally:
        sys.setrecursionlimit(old)
    return out


CASES = [
    ('enum_classes', [()], {}),
    ('plain_metaclass', [()], {}),
    ('overriding_metaclass', [()], {}),
    ('registered_later', [(8,)], {}),
    ('check_added_to_metaclass', [(9,)], {}),
    ('metaclass_assigned', [(9,)], {}),
    ('class_property', [()], {}),
    ('tuples', [()], {}),
    ('unions', [()], {}),
    ('static_metaclass', [()], {}),
    ('bad_class', [()], {}),
    ('recursion_limit', [()], {}),
]
