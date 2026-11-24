# Locals bound on some paths and not others: a loop head sees more
# combinations than a merge point compiles blocks for, so past the cap a
# local may be unbound at run time. Reading or deleting one raises as
# CPython does, and a bound one is released exactly once.
import sys
import weakref


class Obj:
    pass


def read_all(n):
    out = []
    for i in range(n):
        if i & 1:
            a = i
        if i & 2:
            b = str(i)
        if i & 4:
            c = [i]
        if i & 8:
            d = Obj()
        if i & 16:
            del a
        row = []
        for name in 'abcd':
            try:
                if name == 'a':
                    row.append(a)
                elif name == 'b':
                    row.append(b)
                elif name == 'c':
                    row.append(c)
                else:
                    row.append(type(d).__name__)
            except UnboundLocalError as e:
                row.append(str(e))
        out.append(row)
    return out


def delete_all(n):
    out = []
    for i in range(n):
        if i % 3 == 0:
            a = i
        if i % 5 == 0:
            b = i
        if i % 7 == 0:
            c = i
        for k in range(3):
            try:
                if k == 0:
                    del a
                elif k == 1:
                    del b
                else:
                    del c
                out.append((i, k, 'deleted'))
            except UnboundLocalError as e:
                out.append((i, k, str(e)))
    return out


def released(n):
    # each object dies as soon as its last local goes
    refs = []
    dead = []
    for i in range(n):
        if i & 1:
            a = Obj()
            refs.append(weakref.ref(a, lambda r, i=i: dead.append(('a', i))))
        if i & 2:
            b = Obj()
            refs.append(weakref.ref(b, lambda r, i=i: dead.append(('b', i))))
        if i & 4:
            c = Obj()
            refs.append(weakref.ref(c, lambda r, i=i: dead.append(('c', i))))
        if i & 8:
            try:
                del a
            except UnboundLocalError:
                dead.append(('none', i))
        dead.append(('step', i))
    return dead


def seen_by_frame(n):
    out = []
    for i in range(n):
        if i & 1:
            a = i
        if i & 2:
            b = i
        if i & 4:
            c = i
        if i & 8:
            d = i
        out.append(sorted(k for k in sys._getframe().f_locals
                          if k in 'abcd'))
    return out


def in_traceback(n):
    out = []
    for i in range(n):
        if i & 1:
            a = i
        if i & 2:
            b = i
        if i & 4:
            c = i
        if i & 8:
            d = i
        try:
            raise KeyError(i)
        except KeyError:
            f = sys.exc_info()[2].tb_frame
            out.append(sorted(k for k in f.f_locals if k in 'abcd'))
            del f
    return out


def gen_pauses(n):
    for i in range(n):
        if i & 1:
            a = i
        if i & 2:
            b = i
        if i & 4:
            c = i
        if i & 8:
            d = i
        yield i
        try:
            yield (a, b, c, d)
        except UnboundLocalError as e:
            yield str(e)


def gen_all(n):
    return list(gen_pauses(n))


CASES = [
    ('read_all', [(40,)], {}),
    ('delete_all', [(40,)], {}),
    ('released', [(30,)], {}),
    ('seen_by_frame', [(32,)], {}),
    ('in_traceback', [(32,)], {}),
    ('gen_all', [(32,)], {}),
]
