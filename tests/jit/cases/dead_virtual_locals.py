# A loop head that sees more states than a merge point compiles blocks
# for, while dead locals still hold objects compiled code had kept
# virtual: a list, its bound append, a tuple, a bound Python method, an
# int. Past the cap they are made real there. Their values stay what
# CPython shows: in f_locals, in a traceback's frame, the same object
# where two locals share one, and each released once when rebound.
import sys
import weakref


class Obj:
    def meth(self):
        return 1


def shapes(n):
    out = []
    for i in range(n):
        if i & 1:
            chars = [i]
            add = chars.append
            add(i + 1)
            out.append(len(chars))
        if i & 2:
            pair = (i, str(i))
            out.append(pair[1])
        if i & 4:
            m = Obj().meth
            out.append(m())
        if i & 8:
            k = i * 3
            out.append(k + 1)
        if i & 16:
            out.append('x')
    return out


def seen_by_frame(n):
    out = []
    for i in range(n):
        if i & 1:
            chars = [i]
            add = chars.append
            add(-i)
        if i & 2:
            pair = (i, i + 1)
            out.append(pair[0])
        if i & 4:
            m = Obj().meth
            out.append(m())
        if i & 8:
            out.append(i)
        loc = sys._getframe().f_locals
        row = [loc.get('chars'), loc.get('pair'), 'm' in loc]
        if 'add' in loc:
            row.append(loc['add'].__self__ is loc['chars'])
        out.append(row)
    return out


def in_traceback(n):
    out = []
    for i in range(n):
        if i & 1:
            chars = [i]
            add = chars.append
            add(i)
        if i & 2:
            pair = (i, -i)
            out.append(pair[1])
        if i & 4:
            out.append(i)
        if i & 8:
            m = Obj().meth
            out.append(m())
        try:
            if i % 3 == 0:
                raise ValueError(i)
        except ValueError as e:
            loc = e.__traceback__.tb_frame.f_locals
            out.append((loc.get('chars'), loc.get('pair'), 'add' in loc))
    return out


def released(n):
    alive = []
    out = []
    for i in range(n):
        if i & 1:
            o = Obj()
            alive.append(weakref.ref(o))
            pair = (o, i)
            del o
            out.append(pair[1])
        if i & 2:
            chars = [i]
            add = chars.append
            add(i)
        if i & 4:
            out.append(-i)
        if i & 8:
            m = Obj().meth
            out.append(m())
        out.append(sum(r() is not None for r in alive))
    return out


CASES = [
    ('shapes', [(64,)], {}),
    ('seen_by_frame', [(48,)], {}),
    ('in_traceback', [(48,)], {}),
    ('released', [(48,)], {}),
]
