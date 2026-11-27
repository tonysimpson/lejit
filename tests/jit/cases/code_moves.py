# A checked build moves the code each compile writes as the compile ends
# (codemanager.c:psyco_code_move_made), patching every place noted as
# holding where code is, so that one not noted runs the old copy, which
# traps. The other cases show nothing was missed; this one shows the code
# does move, with calls, a loop, a promotion and a branch compiled late.
import sys


def _moved():
    lejit = sys.modules.get('lejit')
    if lejit is None:
        return True
    moved, stayed, relocs = lejit._core._stats()['code_moved']
    return moved > 0 and relocs > 0


class A:
    v = 1


class B:
    v = 2


def _work(objs, n):
    total = 0
    for i in range(n):
        o = objs[i % len(objs)]
        total += o.v
        if total > 1000000:
            total = -total
    return total


def moved():
    out = [_work([A()], 50), _work([A(), B()], 50)]
    return out, _moved()


CASES = [
    ('moved', [()], {'checked': 'reads lejit._core._stats'}),
]
