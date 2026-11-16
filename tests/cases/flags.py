# A comparison result can stay in the processor flags as a virtual bool
# until it is used. Code that runs in between must not clobber them.

import random

# KNOWN_BUGS B8: when a code buffer fills up, compilation pauses behind a
# proxy call that clobbers the flags; a live comparison must be saved first.
# A long run of comparisons ends a buffer right after one.
_rnd = random.Random(1)
_lines = ['def many_compares(a, b, c, d, e, f):']
for _i in range(500):
    _lines.append('    x%d = %s %s %s' % (_i % 20, _rnd.choice('abcdef'),
                                         _rnd.choice(['<', '<=', '==', '!=',
                                                      '>', '>=']),
                                         _rnd.choice('abcdef')))
_lines.append('    return ' + ' + '.join('x%d' % _i for _i in range(20)))
exec('\n'.join(_lines))


def long_compare_run():
    return [many_compares(1, 2, 3, -4, 5, 0), many_compares(3, 2, 1, 0, -1, 7),
            many_compares(0, 0, 0, 0, 0, 0)]


# KNOWN_BUGS B16: where paths join, the stack is lined up before the jump;
# with a SUB that clobbered a live comparison (the zero path of an int
# operand joins the others right after comparing)
def ident(x):
    return x


def chained(a, b, c):
    return ident(a <= b <= c)


def and_compare(a, b, c):
    return a and b <= c


def joined_compares():
    vals = [-2, -1, 0, 1, 2]
    out = []
    for a in vals:
        for b in vals:
            for c in vals:
                out.append((chained(a, b, c), and_compare(a, b, c)))
    return out


CASES = [
    ('long_compare_run', [()], {}),
    ('joined_compares', [()], {}),
]
