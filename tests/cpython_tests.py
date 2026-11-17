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
           'test_json', 'test_statistics',
           'test_list', 'test_dict', 'test_set', 'test_tuple', 'test_math',
           'test_random', 'test_difflib', 'test_pprint', 'test_shlex',
           'test_copy', 'test_deque', 'test_sort', 'test_contextlib',
           'test_abc', 'test_dataclasses', 'test_struct', 'test_re',
           'test_ordered_dict', 'test_generators', 'test_scope', 'test_class',
           'test_exceptions', 'test_with', 'test_keywordonlyarg',
           'test_grammar', 'test_types', 'test_bool', 'test_int', 'test_float',
           'test_long', 'test_complex', 'test_builtin', 'test_richcmp',
           'test_userdict', 'test_userlist', 'test_userstring', 'test_super',
           'test_descr', 'test_coroutines', 'test_asyncgen', 'test_fstring']

# module -> reason it is expected to differ, or (the expected result with
# Ni, reason) when only that difference is accepted
KNOWN = {
    'test_re': 'C11: warnings stacklevel skips compiled callees',
    'test_exceptions': ('run=53 fail=2 err=0 skip=1 exit=0',
                        'C18: tracebacks don\'t keep compiled locals alive'),
    'test_generators': ('run=16 fail=1 err=0 skip=0 exit=0',
                        'C19: getrefcount misses borrowed arguments'),
    'test_descr': ('run=138 fail=1 err=0 skip=2 exit=0',
                   'C20: compiling allocates a GC object test_slots counts'),
    'test_coroutines': ('run=94 fail=3 err=0 skip=0 exit=0',
                        'C11: a compiled frame\'s f_lineno is its def line '
                        '(also in coroutine origins and warning locations)'),
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
        if isinstance(known, tuple):
            expected, known = known
            if same:
                known = None    # the accepted difference may come and go
            elif with_ni != expected:
                known = None    # a difference other than the accepted one
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
