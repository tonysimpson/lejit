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


CASES = [
    ('long_compare_run', [()], {}),
]
