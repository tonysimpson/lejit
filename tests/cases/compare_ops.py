# COMPARE_OP, generated from ceval.c's cmp_outcome: every compare
# op on ints, floats, strings and mixed types; is / is not; in / not in on
# tuples, lists, dicts and strings; exception matching against classes,
# tuples, subclasses and non-classes (known at compile time, and only at
# run time, which runs the generated run-time copy). Then cells: load,
# store, delete and the unbound errors.

def rich(a, b):
    return a < b, a <= b, a == b, a != b, a > b, a >= b

def identity(a, b):
    return a is b, a is not b, a is None, b is not None

def membership(x, c):
    return x in c, x not in c

def membership_error(x, c):
    try:
        return x in c
    except TypeError as e:
        return 'TypeError', str(e)

def rich_error(a, b):
    try:
        return a < b
    except TypeError as e:
        return 'TypeError', str(e)

def truth_of(a, b):
    n = 0
    if a is b:
        n += 1
    if a is not b:
        n += 2
    if a in (b, 1):
        n += 4
    if a not in [b]:
        n += 8
    return n


class MyError(KeyError):
    pass


def match_class(i):
    try:
        [KeyError, MyError, ValueError, IndexError][i]
        raise [KeyError, MyError, ValueError, IndexError][i]()
    except KeyError:
        return 'KeyError'
    except (ValueError, TypeError):
        return 'ValueError or TypeError'
    except Exception as e:
        return type(e).__name__

def match_subclass():
    try:
        raise MyError('x')
    except LookupError as e:
        return type(e).__name__

def match_nested_tuple():
    try:
        raise MyError('x')
    except ((ValueError, (KeyError,)), IndexError):
        return 'nested'

def match_runtime(exc, what):
    # 'what' known only at run time: the arm resumes in its run-time copy
    try:
        raise exc
    except what:
        return 'caught'
    except BaseException as e:
        return 'other', type(e).__name__

def match_non_class(what):
    try:
        try:
            raise KeyError
        except what:
            return 'caught'
    except TypeError as e:
        return 'TypeError', str(e)

def match_non_class_const():
    try:
        try:
            raise KeyError
        except 42:
            return 'caught'
    except TypeError as e:
        return 'TypeError', str(e)

def match_tuple_non_class_const():
    try:
        try:
            raise KeyError
        except (KeyError, 'no'):
            return 'caught'
    except TypeError as e:
        return 'TypeError', str(e)


def cell_load(a):
    def inner():
        return a + 1
    return inner(), a

def cell_store(a):
    b = 0
    def inner():
        return b
    b = a * 2
    r1 = inner()
    b = 'x'
    return r1, inner(), b

def cell_store_nonlocal(n):
    total = 0
    def add(k):
        nonlocal total
        total += k
    for i in range(n):
        add(i)
    return total

def cell_delete(a):
    c = a
    def inner():
        return c
    r = inner()
    del c
    try:
        return r, inner()
    except NameError as e:
        return r, 'NameError', str(e)

def cell_unbound_load():
    try:
        x
    except NameError as e:
        r = str(e)
    x = 1
    def inner():
        return x
    return r, inner()

def cell_unbound_delete():
    def inner():
        return y
    try:
        del y
    except NameError as e:
        return 'NameError', str(e)
    y = 1


CASES = [
    ('rich', [(1, 2), (2, 2), (3, 2), (-5, 10**30), (1.5, 2.5), (2.0, 2.0),
              (float('nan'), 1.0), ('a', 'b'), ('b', 'b'), (1, 1.0), (True, 1),
              (2, 1.5), ((1, 2), (1, 3)), ([1], [1])], {}),
    ('rich_error', [(1, 'a'), ('a', None), ([], 1), (1, 2)], {}),
    ('identity', "[(1, 1), (None, None), (None, 1), ('a', 'a'), ((), ()), (1.5, 2.5)]", {}),
    ('membership', [(1, (1, 2)), (3, (1, 2)), (1, [1, 2]), (5, []), (1, {1: 2}),
                    (2, {1: 2}), ('b', 'abc'), ('d', 'abc'), ('', ''), (1, {1}),
                    (1.0, (1,))], {}),
    ('membership_error', [(1, 1), ('a', None), (1, 'abc'), (1, (1,))], {}),
    ('truth_of', [(1, 1), (None, None), (1, 2), (2, 1)], {}),
    ('match_class', [(0,), (1,), (2,), (3,)], {}),
    ('match_subclass', [()], {}),
    ('match_nested_tuple', [()], {}),
    ('match_runtime', "[(KeyError, KeyError), (MyError, KeyError), (KeyError, MyError),"
                      " (ValueError, (KeyError, ValueError)), (IndexError, (KeyError,)),"
                      " (KeyError, ()), (MyError, LookupError)]", {}),
    ('match_non_class', "[(42,), ('no',), ((KeyError, 1),), ((1, KeyError),), (int,),"
                        " (KeyError,), ((ValueError, KeyError),)]", {}),
    ('match_non_class_const', [()], {}),
    ('match_tuple_non_class_const', [()], {}),
    ('cell_load', [(1,), (2.5,)], {}),
    ('cell_store', [(3,), ('a',)], {}),
    ('cell_store_nonlocal', [(5,), (0,)], {}),
    ('cell_delete', [(1,), ('a',)], {}),
    ('cell_unbound_load', [()], {}),
    ('cell_unbound_delete', [()], {}),
]
