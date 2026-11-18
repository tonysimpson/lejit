# Wrong answers the differential fuzzer found (tools/fuzz/), each reduced
# to the smallest program that still showed it.


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




def tuple_repeat_huge():
    # a known tuple times a constant, 2**40 items: the generated tuplerepeat
    # must keep the size as Py_ssize_t (as an int it is 0, and the copy
    # writes past it). CPython's PyTuple_New fails at once (8 TB).
    t = (1, 2)
    try:
        return len(t * 2**39)
    except MemoryError:
        return 'MemoryError'


CASES = [
    ('str_index_len', [('a',), ('abc',), ('',), ('été',)], {}),
    ('str_index_len_neg', [('a',), ('xyz',)], {}),
    ('tuple_repeat_huge', [()], {}),
]
