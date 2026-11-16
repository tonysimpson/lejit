# Calls with keyword arguments (CALL_FUNCTION_KW) and with *args/**kwargs
# (CALL_FUNCTION_EX): KNOWN_BUGS F19, and more than 255 arguments (F3).

def target(a, b=2, *rest, c=3, **kw):
    return (a, b, rest, c, sorted(kw.items()))

def by_len(s):
    return len(s)

def kw_call(x):
    return target(x, c=10)

def kw_two(x):
    return target(x, 5, c=x, d=7)

def kw_builtin(xs):
    return sorted(xs, key=by_len, reverse=True)

def kw_bad_name(x):
    return target(x, nope=1, a=2)

def star_call(t):
    return target(*t)

def star_star_call(t, d):
    return target(*t, **d)

def star_list(xs):
    return max(*xs)

def star_bad(x):
    return target(*x)

def star_star_bad(d):
    return target(1, **d)

def star_star_mapping(d):
    return target(*(1,), **d)

class Mapping:
    def keys(self):
        return ['c']
    def __getitem__(self, k):
        return 99

def kw_loop(n):
    t = 0
    for i in range(n):
        t += len(target(i, c=i)[2]) + target(i, b=i)[1]
    return t

def count_args(*a, **k):
    return len(a), sum(a), len(k)

# 300 positional arguments: CALL_FUNCTION's oparg is the plain count in
# Python 3 (KNOWN_BUGS F3: it used to be masked to 8 bits)
exec('def call_300(x):\n    return count_args(%s)\n'
     % ', '.join('x + %d' % i for i in range(300)))
exec('def call_300_kw(x):\n    return count_args(%s)\n'
     % ', '.join('k%d=x' % i for i in range(300)))

CASES = [
    ('kw_call', "[(1,), ('s',)]", {}),
    ('kw_two', "[(1,), (4,)]", {}),
    ('kw_builtin', "[(['aaa', 'b', 'cc'],)]", {}),
    ('kw_bad_name', "[(1,)]", {}),
    ('star_call', "[((1,),), ((1, 2, 3, 4),), ((),)]", {}),
    ('star_star_call', "[((1,), {'c': 5}), ((1, 2), {'z': 0}), ((), {'a': 1})]", {}),
    ('star_list', "[([3, 9, 2],)]", {}),
    ('star_bad', "[(5,), ([7, 8],), ('ab',)]", {}),
    ('star_star_bad', "[(5,), ({'b': 1, 'a': 2},)]", {}),
    ('star_star_mapping', "[(Mapping(),)]", {}),
    ('kw_loop', "[(10,)]", {}),
    ('call_300', "[(1,)]", {}),
    ('call_300_kw', "[(1,)]", {}),
]
