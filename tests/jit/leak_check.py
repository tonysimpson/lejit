"""Checks run.py's leak check on tests/jit/leak_check_cases: code compiled
late in the counted rounds is not a leak, and a leak still fails.

    python3.X tests/jit/leak_check.py
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPEATED = 'code compiled during the rounds, so they were run 2 times'
WANT = [
    ('pass  late.compiles_late  [', REPEATED + ']'),
    ('FAIL  late.compiles_late_and_leaks: leaks: ', '; ' + REPEATED),
    ('FAIL  late.leaks: leaks: ',
     'without LeJIT); blocks every 10 rounds, then collected: '),
]

out = subprocess.run([sys.executable, os.path.join(HERE, 'run.py'),
                      os.path.join(HERE, 'leak_check_cases'), '-v'],
                     stdout=subprocess.PIPE, universal_newlines=True).stdout
lines = out.splitlines()
for start, end in WANT:
    if not any(l.startswith(start) and end in l for l in lines):
        sys.exit('expected a line like %r in:\n%s' % (start, out))
