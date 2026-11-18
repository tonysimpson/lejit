# The opcodes generated from ceval.c by tools/metac/gen_ceval.py (D26, D67,
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


class Obj:
    pass


def attributes(k):
    # LOAD_ATTR, STORE_ATTR, DELETE_ATTR
    o = Obj()
    o.a = k
    o.b = o.a + 1
    del o.a
    return o.b, hasattr(o, 'a')


def attr_errors(k):
    return catch((err_load_attr, err_store_attr, err_del_attr), k)


def err_load_attr(k):
    return k.missing


def err_store_attr(k):
    k.x = 1


def err_del_attr(k):
    o = Obj()
    del o.missing


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


def builds(a, b, xs):
    # BUILD_TUPLE, BUILD_LIST (0, 1 and several items), BUILD_SLICE (2 and
    # 3 items), LIST_APPEND and MAP_ADD (comprehensions)
    t0, t1, t3 = (), (a,), (a, b, a + b)
    l0, l1, l3 = [], [a], [a, b, a * b]
    s2, s3 = xs[a:b], xs[a:b:2]
    s4 = xs[::-1]
    sl = slice(a, b), slice(a, b, 2)
    sq = [x * x for x in xs]
    d = {x: x + a for x in xs}
    nested = [[y for y in range(x)] for x in range(a)]
    return t0, t1, t3, l0, l1, l3, s2, s3, s4, sl, sq, d, nested


def err_map_add(k):
    # MAP_ADD's error exit: an unhashable key, part-way through
    return {(x if x < 2 else [x]): x for x in range(k + 3)}


def err_slice(k):
    # the slice built by BUILD_SLICE is used, and fails, in the subscript
    return [1, 2, 3][k:'a':2]


def build_errors(k):
    return catch((err_map_add, err_slice), k)


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
    ('attributes', [(1,), (2.5,)], {}),
    ('attr_errors', [(0,), (None,)], {}),
    ('builds', [(1, 3, [5, 6, 7, 8]), (0, 2, (1, 2, 3))], {}),
    ('build_errors', [(0,), (1,)], {}),
]
