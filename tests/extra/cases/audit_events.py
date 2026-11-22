# Audit events raised by generated metas where the source has audit hooks:
# id() (builtins.id) and a read-restricted member (object.__getattr__, a
# bound method's __func__). A hook records the events and raises on a
# marked object, so both the event and its error path are compared. Where
# there are no audit hooks (3.7) the same calls run with nothing recorded.
import sys

EVENTS = []


class Refused:
    pass


def hook(event, args):
    if event in ('builtins.id', 'object.__getattr__'):
        EVENTS.append(event)
        if args and isinstance(args[0], Refused):
            raise RuntimeError('refused by the hook')


if hasattr(sys, 'addaudithook'):
    sys.addaudithook(hook)


class C:
    def m(self):
        return 1


def ids(n):
    del EVENTS[:]
    x = C()
    same = 0
    for i in range(n):
        same += id(x) == id(x)
    return same, len(EVENTS)


def id_refused():
    del EVENTS[:]
    try:
        id(Refused())
    except RuntimeError as e:
        return str(e), len(EVENTS)
    return 'no error', len(EVENTS)


def method_func(n):
    del EVENTS[:]
    b = C().m
    k = 0
    for i in range(n):
        k += b.__func__ is C.m
    return k, len(EVENTS)


CASES = [
    ('ids', [(0,), (3,)], {}),
    ('id_refused', [()], {}),
    ('method_func', [(0,), (4,)], {}),
]
