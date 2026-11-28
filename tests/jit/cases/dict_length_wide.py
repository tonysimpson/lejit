# len() of a dict reads all of its count, ma_used, which is a Py_ssize_t.
# No test makes a dict of 2**31 items (it would take tens of gigabytes):
# this one sets a small dict's count past 32 bits with ctypes, calls len()
# alone on it, and puts the count back.
import ctypes

_WORD = ctypes.sizeof(ctypes.c_void_p)


def _count(d):
    # ma_used is the first field after the object header
    return ctypes.c_ssize_t.from_address(id(d) + 2 * _WORD)


def wide_lengths(n):
    d = {'a': 1, 'b': 2, 'c': 3}
    count = _count(d)
    real = count.value
    count.value = real + (1 << 32)
    try:
        lengths = []
        for i in range(n):
            lengths.append(len(d))
        return lengths[0], lengths[-1], len(lengths)
    finally:
        count.value = real


CASES = [
    ('wide_lengths', [(5,), (500,)], {}),
]
