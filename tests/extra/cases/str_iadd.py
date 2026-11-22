# s += t and s = s + t on exact str (pycompiler.c:unicode_concatenate, as
# ceval's): when the variable holds the only other reference, CPython
# unbinds it and PyUnicode_Append resizes the string in place. The results,
# the identities CPython gives, the old value kept by another reference,
# subclasses on either side, and the variable after a MemoryError (unbound
# when the append took its reference, bound when it was shared). 'grow'
# would be quadratic without the in-place append and time out.
import resource
import sys


class S(str):
    pass


class R(str):
    def __radd__(self, other):
        return ('radd', other)


class Peek(str):
    def __radd__(self, other):
        return sorted(sys._getframe(1).f_locals)


def grow(n):
    s = ''
    t = ''
    for i in range(n):
        s += 'abcdefgh'
        t = t + 'ijklmnop'
    return len(s), len(t), s[-9:], t[:9]


def widen(pieces):
    s = 'x' * 3
    for p in pieces:
        s += p
        s = s + p
    return s, len(s), max(map(ord, s))


def kept(n):
    s = 'start' * 2
    old = []
    for i in range(n):
        old.append(s)
        s += '.'
    alias = s
    s += 'end'
    return old[0], old[-1], alias, s, alias is s


def identities(t):
    s = ''.join(['a', 'b'])
    first = s
    s += ''
    same = s is first
    u = ''
    u += t
    return same, u is t, s


def interned(n):
    s = sys.intern(''.join(['in', 'tern']))
    for i in range(n):
        s += 'x'
    return s, sys.intern('intern') == 'intern', sys.intern('intern') is not s


def hashed(n):
    s = ''.join(['h', 'a'])
    d = {s: 1}
    for i in range(n):
        s += 'y'
        d[s] = i
    return sorted(d.items())


def subclass_right(n):
    s = ''.join(['s', 'u'])
    for i in range(n):
        s += S('b')
    r = s
    r += R('c')
    return type(s).__name__, s, r


def radd_sees_locals(n):
    s = ''.join(['p', 'k'])
    s += Peek('x')
    return s, n


def subclass_left(n):
    s = S('left')
    for i in range(n):
        s += 'z'
    return type(s).__name__, s


def cell(n):
    s = ''

    def read():
        return s
    for i in range(n):
        s += 'c'
        s = s + 'd'
    return len(read()), read()[-4:]


def via_other(n):
    s = ''.join(['o', 'k'])
    for i in range(n):
        s = s[:] + 'e'
        s = s.strip() + 'f'
    return s


PIECE = 'b' * (48 << 20)


def _vmsize():
    with open('/proc/self/status') as f:
        for line in f:
            if line.startswith('VmSize:'):
                return int(line.split()[1]) * 1024


def append_fails(shared):
    s = 'a' * (48 << 20)
    keep = s if shared else None
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (_vmsize() + (32 << 20), hard))
    try:
        s += PIECE
        out = 'appended'
    except MemoryError:
        out = 's' in sys._getframe().f_locals
    finally:
        resource.setrlimit(resource.RLIMIT_AS, (soft, hard))
    try:
        return out, len(s), keep is s
    except UnboundLocalError as e:
        return out, str(e)


CASES = [
    ('grow', [(200000,),
              (3,)], {}),
    ('widen', [(['a', 'é', '€', '😀', 'b'],), (['', 'q'],)], {}),
    ('kept', [(5,)], {}),
    ('identities', [('t',), ('',)], {}),
    ('interned', [(3,)], {}),
    ('hashed', [(4,)], {}),
    ('subclass_right', [(3,)], {}),
    ('radd_sees_locals', [(1,)], {}),
    ('subclass_left', [(3,)], {}),
    ('cell', [(1000,)], {}),
    ('via_other', [(5,)], {}),
    ('append_fails', [(False,), (True,)], {}),
]
