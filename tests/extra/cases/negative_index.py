# A negative index on a tuple or list is counted from the object's own size,
# as tuplesubscript and list_subscript do, not from a subclass's __len__.


def msg(f, *a):
    try:
        return ('ok', f(*a))
    except Exception as e:
        return (type(e).__name__, str(e))


class T(tuple):
    def __len__(self):
        return 1


class L(list):
    def __len__(self):
        return 1


def get(s, i):
    return msg(lambda: s[i])


def get_tuple(x):
    t = T((x, 2, 3))
    return t[-1], t[-3], msg(lambda: t[-4])


def set_del(i):
    out = []
    for s in (L([1, 2, 3]), [1, 2, 3]):
        def st():
            s[i] = 'v'
            return list(s)

        def dl():
            del s[i]
            return list(s)
        out.append((msg(st), msg(dl)))
    return out


CASES = [
    ('get', '[(T((1, 2, 3)), -1), (T((1, 2, 3)), -3), (T((1, 2, 3)), -4),'
            ' (L([1, 2, 3]), -1), (L([1, 2, 3]), -4), ((1, 2, 3), -1),'
            ' ([1, 2, 3], -2), ((1,), -2)]', {}),
    ('get_tuple', [(1,), ('a',)], {}),
    ('set_del', [(-1,), (-3,), (-4,), (0,), (2,)], {}),
]
