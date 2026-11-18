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


NAN = float('nan')


def folded_nan_in_set(n):
    # NAN is a global, a constant to Ni: NAN + 1.5 is boxed once, so each
    # iteration adds the same NaN object, where CPython makes a new one
    return {NAN + 1.5 for _ in range(n)}, len({NAN * 2.0: 0 for _ in range(n)})


CASES = [
    ('str_index_len', [('a',), ('abc',), ('',), ('été',)], {}),
    ('str_index_len_neg', [('a',), ('xyz',)], {}),
    ('folded_nan_in_set', [(2,), (3,)],
     {'xfail': 'F117: a float folded from constants is one object for every run of its code'}),
]
