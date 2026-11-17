# The opcodes generated from tools/metac/bytecodes37.c (D26,
# ni/Python/pycompiler_gen.h): each one at least once, with its error exit
# where it has one. An error exit leaves the inputs on the stack for the
# unwinding code, so the exceptions here also check that nothing is
# released twice or leaked.
import collections


def arith(a, b):
    # BINARY_* and UNARY_*
    return [a + b, a - b, a * b, a / b, a // b, a % b, a ** 2, -a, +a,
            a & b, a | b, a ^ b, a << 3, a >> 1, ~a]


def arith_float(a, b):
    return [a + b, a - b, a * b, a / b, a // b, a % b, a ** 2.0, -a, +a]


def inplace(a, b):
    # INPLACE_*
    x = a
    x += b
    x -= 1
    x *= b
    x //= 2
    x %= 1000
    x **= 2
    x <<= 2
    x >>= 1
    x &= 0xffff
    x |= 1
    x ^= 5
    y = float(x)
    y /= 3.0
    return x, y


def inplace_list(xs):
    xs += [1]              # INPLACE_ADD on a list: same object
    return xs


def stack_shuffles(a, b, c):
    a, b = b, a            # ROT_TWO
    a, b, c = c, a, b      # ROT_THREE, ROT_TWO
    ok = a < b < c         # DUP_TOP, ROT_THREE
    return a, b, c, ok


def subscripts(xs, d):
    xs[0] = xs[1] + xs[2]  # BINARY_SUBSCR, STORE_SUBSCR
    xs[1] += 10            # DUP_TOP_TWO, ROT_THREE
    d['k'] = xs[1]
    d['k'] += 1
    return xs, d


def iterate(xs):
    t = 0
    for x in xs:           # GET_ITER
        t = t + x
    return t


def catch(fs, k):
    out = []
    for f in fs:
        try:
            out.append(f(k))
        except Exception as e:
            out.append((type(e).__name__, str(e)))
    return out


def errors(k):
    # each error exit, through the unwinding code
    return catch((err_add, err_subscr, err_store, err_pow, err_inplace), k)


def messages(k):
    # TypeError texts from Ni's abstract layer must be 3.7's (F28)
    return catch((err_neg, err_pos, err_inv, err_iter, err_not_subscr,
                  err_seq_index, err_no_assign, err_seq_assign, err_no_del,
                  err_repeat), k)


def err_pos(k):
    return +'s' if k else k


def err_inv(k):
    return ~'s' if k else k


def err_not_subscr(k):
    return k[0]


def err_seq_index(k):
    return collections.deque([1])['a']


def err_no_assign(k):
    k[0] = 1


def err_seq_assign(k):
    d = collections.deque([1])
    d['a'] = 1


def err_no_del(k):
    del k[0]


def err_repeat(k):
    return [1] * 'a'


def err_add(k):
    return k + 'a'


def err_neg(k):
    return -'s' if k else k


def err_subscr(k):
    return [1, 2][k + 5]


def err_store(k):
    xs = [1, 2]
    xs[k + 5] = 0
    return xs


def err_iter(k):
    for x in k:
        return x


def err_pow(k):
    return k ** 'x'


def err_inplace(k):
    x = k
    x -= None
    return x


def unbound(flag):
    # LOAD_FAST's error exit: the local is NULL on one path
    if flag:
        x = 1
    try:
        return x
    except UnboundLocalError as e:
        return str(e)


CASES = [
    ('arith', [(7, 3), (-12, 5), (2 ** 40, 7)], {}),
    ('arith_float', [(7.5, 2.0), (-1.25, 0.5)], {}),
    ('inplace', [(5, 3), (100, 7)], {}),
    ('inplace_list', [([0],)], {}),
    ('stack_shuffles', [(1, 2, 3), (3, 1, 2)], {}),
    ('subscripts', [([1, 2, 3], {}), ([5, 6, 7], {'z': 0})], {}),
    ('iterate', [([1, 2, 3],), ((4.5, 1),)], {}),
    ('errors', [(0,), (1,)], {}),
    ('messages', [(0,), (1,)], {}),
    ('unbound', [(True,), (False,)], {}),
]
