# A call LeJIT folds at compile time (CfPure, every argument known) must not
# read memory that can change: callable() of a known instance reads its
# class's tp_call, which assigning __call__ sets (effects.py's pure).


class Plain:
    pass


OBJ = Plain()


def give_call(flip):
    if flip:
        Plain.__call__ = lambda self: 1
    else:
        try:
            del Plain.__call__
        except AttributeError:
            pass


def callable_after_class_change(flip):
    before = callable(OBJ)
    give_call(flip)
    return before, callable(OBJ)


CASES = [
    ('callable_after_class_change', [(0,), (1,), (0,), (1,)], {}),
]
