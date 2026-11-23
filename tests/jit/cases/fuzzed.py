# Programs from the differential fuzzers (tests/extra/fuzz), cut down to
# the smallest code that shows each case.


def str_index_len(s):
    # the index is the very length field the bounds check reads: an
    # unsigned >= of a value with itself is true (codegen.c:integer_cmp)
    try:
        return s[len(s)]
    except IndexError as e:
        return str(e)


def str_index_len_neg(s):
    n = len(s)
    return s[n - n], s[-n] if s else None


NAN = float('nan')


def folded_nan_in_set(n):
    # NAN is a global, a constant to LeJIT: NAN + 1.5 folds, but each
    # iteration must still add a new NaN object, as CPython does
    return {NAN + 1.5 for _ in range(n)}, len({NAN * 2.0: 0 for _ in range(n)})


def tuple_repeat_huge():
    # a known tuple times a constant, 2**40 items: the generated tuplerepeat
    # must keep the size as Py_ssize_t (as an int it is 0, and the copy
    # writes past it). CPython's PyTuple_New fails at once (8 TB).
    t = (1, 2)
    try:
        return len(t * 2**39)
    except MemoryError:
        return 'MemoryError'


class Index:
    # __index__ is Python code: a constant of this class gives a new
    # answer each time
    def __index__(self):
        return INDEX_VALUE


INDEX_VALUE = 2
GLOBAL_INDEX = Index()


def repeat_by_constant_index(v):
    # GLOBAL_INDEX is a constant to LeJIT; the repeat must still call its
    # __index__ every time (plongobject.h:PsycoNumber_AsSsize_t)
    global INDEX_VALUE
    INDEX_VALUE = v
    try:
        return len('ab' * GLOBAL_INDEX), len([0] * GLOBAL_INDEX), len((0,) * GLOBAL_INDEX)
    except TypeError as e:
        return str(e)


class Rec:
    def __eq__(self, o):
        return [Rec()] == [Rec()]


def recursion_message():
    # CPython trips its limit in the comparison's call of __eq__; LeJIT's
    # direct C calls skip that count, so its frame check trips instead
    try:
        Rec() == Rec()
    except RecursionError as e:
        return str(e)


def c_calls_counted():
    import sys
    # CPython's message names the C check that trips ('in comparison') only
    # where C calls count in the same depth as Python's, which the limit
    # then stops a nested repr at
    old = sys.getrecursionlimit()
    nested = []
    for _ in range(500):
        nested = [nested]
    sys.setrecursionlimit(100)
    try:
        repr(nested)
        return False
    except RecursionError:
        return True
    finally:
        sys.setrecursionlimit(old)


NAMES_CHECK = c_calls_counted()

CASES = [
    ('str_index_len', [('a',), ('abc',), ('',), ('été',)], {}),
    ('str_index_len_neg', [('a',), ('xyz',)], {}),
    ('folded_nan_in_set', [(2,), (3,)], {}),
    ('tuple_repeat_huge', [()], {}),
    ('repeat_by_constant_index', [(2,), (3,), ('s',), (2**63,)], {}),
    ('recursion_message', [()],
     {'xfail': 'the RecursionError message names another check'} if NAMES_CHECK else {}),
]
