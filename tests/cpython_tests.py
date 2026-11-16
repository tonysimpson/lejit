"""Run some of CPython's own unit tests with and without Ni and compare.

Each module runs in a fresh interpreter, once plain and once with Ni
imported first. The test counts (run, failures, errors, skips) and the exit
status must match. Known differences are listed in KNOWN with their
KNOWN_BUGS ids; one that starts matching is reported, so it can be removed.

    python3.7 tests/cpython_tests.py [module ...]
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MODULES = ['test_bisect', 'test_heapq', 'test_textwrap', 'test_fractions',
           'test_string', 'test_operator', 'test_itertools', 'test_enum',
           'test_csv', 'test_base64', 'test_functools', 'test_collections',
           'test_json', 'test_statistics']

# module -> reason it is expected to differ
KNOWN = {
}

CHILD = r'''
import sys, unittest, io
if sys.argv[1] == 'ni':
    sys.path.insert(0, sys.argv[3]); import ni
mod = __import__('test.' + sys.argv[2], fromlist=['x'])
suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
r = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
print('run=%d fail=%d err=%d skip=%d' % (r.testsRun, len(r.failures),
                                         len(r.errors), len(r.skipped)))
sys.stdout.flush()
'''


def run(mode, name):
    try:
        p = subprocess.run([sys.executable, '-c', CHILD, mode, name, ROOT],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=600)
    except subprocess.TimeoutExpired:
        return 'timeout'
    out = p.stdout.decode().strip().splitlines()
    return '%s exit=%d' % (out[-1] if out else '(no output)', p.returncode)


def main():
    names = sys.argv[1:] or MODULES
    bad = 0
    for name in names:
        plain, with_ni = run('plain', name), run('ni', name)
        same = plain == with_ni
        known = KNOWN.get(name)
        if same and not known:
            status = 'ok'
        elif not same and known:
            status = 'known'
        elif same:
            status = 'FIXED?'
            bad += 1
        else:
            status = 'FAIL'
            bad += 1
        line = '%-6s %-17s %s' % (status, name, plain)
        if not same:
            line += '   with Ni: ' + with_ni
        if known:
            line += '  [%s]' % known
        print(line)
        sys.stdout.flush()
    print('%d modules, %d unexpected' % (len(names), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
