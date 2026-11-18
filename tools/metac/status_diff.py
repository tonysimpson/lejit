#!/usr/bin/env python3
"""Compare two versions' status files (generate.py's report.txt, or the
baselines in expected/) function by function, for a port review.

    status_diff.py A B [-r]

A and B are versions ('3.7.17', read from expected/) or paths. Prints each
key whose status differs, or that only one has; -r also prints keys whose
status is the same but whose reason differs. Ends with the counts.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read(arg):
    path = arg if os.path.exists(arg) else os.path.join(HERE, 'expected', arg + '.txt')
    out = {}
    with open(path) as f:
        for ln in f:
            if ln.strip() and not ln.startswith('#'):
                key, status, reason = (ln.rstrip('\n').split('\t') + ['', ''])[:3]
                out[key] = (status, reason)
    return out


def main(argv):
    reasons = '-r' in argv
    argv = [a for a in argv if a != '-r']
    if len(argv) != 2:
        print(__doc__)
        return 2
    a, b = read(argv[0]), read(argv[1])
    changed = only_a = only_b = same_status = 0
    for key in sorted(set(a) | set(b)):
        x, y = a.get(key), b.get(key)
        if x is None:
            only_b += 1
            print('+ %-40s %s' % (key, y[0]))
        elif y is None:
            only_a += 1
            print('- %-40s %s' % (key, x[0]))
        elif x[0] != y[0]:
            changed += 1
            print('~ %-40s %s -> %s' % (key, x[0], y[0]))
        elif reasons and x[1] != y[1]:
            same_status += 1
            print('r %-40s %s\n    %s\n    %s' % (key, x[0], x[1], y[1]))
    print('%d status changes, %d only in %s, %d only in %s%s'
          % (changed, only_a, argv[0], only_b, argv[1],
             ', %d reason changes' % same_status if reasons else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
