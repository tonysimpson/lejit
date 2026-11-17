# A C function called directly from compiled code gets CPython's result
# checks (Objects/call.c:_Py_CheckFunctionResult, KNOWN_BUGS C25): NULL
# without an exception, or a result with one set, is a SystemError.
# _testcapi has a function of each kind (CPython's test_capi).
import _testcapi


def null_without_error(n):
    r = []
    for i in range(n):
        try:
            _testcapi.return_null_without_error()
        except SystemError as e:
            r.append(str(e))
    return r


def result_with_error(n):
    r = []
    for i in range(n):
        try:
            _testcapi.return_result_with_error()
        except SystemError as e:
            r.append((str(e), type(e.__cause__).__name__))
    return r


def good_calls(xs):
    # the checks don't disturb ordinary results and exceptions
    r = [len(xs), abs(-3), max(xs), sorted(xs), repr(xs)]
    try:
        int('x')
    except ValueError as e:
        r.append(str(e))
    return r


CASES = [
    ('null_without_error', [(3,)], {}),
    ('result_with_error', [(3,)], {}),
    ('good_calls', [([3, 1, 2],)], {}),
]
