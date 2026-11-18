#!/usr/bin/env python3
"""The parse gate (doc/GENERATOR_DESIGN.md "Migration", step 1): the front
end (tools/metac/front.py) preprocesses and parses every CPython .c file Ni
mirrors, for each target, with the target's own pyconfig.h and flags.

    parse_gate.py [PYTHON ...]

The targets are the interpreters given, else NI_PY37 and NI_PY38 (defaults:
pyenv's 3.7.17 and uv's 3.8.20). Runs under NI_GEN_PYTHON (3.10 or later)
and needs no build. Exits 1 if any file fails, naming the file and line.
"""
import glob
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
METAC = os.path.join(ROOT, 'tools', 'metac')
sys.path.insert(0, METAC)
import front  # noqa: E402
import policy_table  # noqa: E402
from pycparser import c_ast  # noqa: E402
from pycparser.c_parser import ParseError  # noqa: E402

TARGETS = [
    os.environ.get('NI_PY37', os.path.expanduser(
        '~/.pyenv/versions/3.7.17/bin/python3.7')),
    os.environ.get('NI_PY38', os.path.expanduser(
        '~/.local/share/uv/python/cpython-3.8.20-linux-x86_64-gnu/bin/python3.8')),
]


def mirrored_files():
    """Every .c file Ni mirrors: those the policy table names, those the
    generators read, and ceval.c."""
    files = {k.split(':')[0] for k in policy_table.ENTRIES}
    files |= set(policy_table.TRANSLATED_SLOTS) | set(policy_table.TRANSLATED_FUNCTIONS)
    for path in glob.glob(os.path.join(METAC, 'gen_*.py')):
        with open(path) as f:
            files |= set(re.findall(r"'((?:Objects|Python|Modules)/[\w/-]+\.c)'",
                                    f.read()))
    files.add('Python/ceval.c')
    return sorted(f for f in files if f.endswith('.c'))


def check_the_check():
    """check_vocabulary must see a header's definition that the shim didn't
    replace before the file's code, and accept one that it did."""
    name = front.vocabulary()[0]
    late = '# 1 "a.c"\n# 1 "h.h"\n#define %s(x) x\n# 2 "a.c"\nint y;\n' % name
    fixed = late.replace('# 2 "a.c"', '# 1 "%s"\n#undef %s\n#define %s(...) m\n'
                         '# 2 "a.c"' % (front.VOCAB, name, name))
    return (front.check_vocabulary(late, 'a.c', [name]) != []
            and front.check_vocabulary(fixed, 'a.c', [name]) == [])


def misplaced(ast, path):
    """The first function of `path` whose line in the AST holds neither its
    name nor a macro that defines it, if any."""
    with open(path) as f:
        lines = f.read().split('\n')
    for d in ast.ext:
        if isinstance(d, c_ast.FuncDef) and d.coord.file == path:
            line = lines[d.coord.line - 1]
            if d.decl.name not in line and not re.match(r'[A-Z_][A-Z0-9_]*\(', line):
                return '%s (line %d)' % (d.decl.name, d.coord.line)
    return None


def main(argv):
    if not check_the_check():
        print('FAIL front.check_vocabulary misses a late definition')
        return 1
    files = mirrored_files()
    failed = 0
    for python in argv or TARGETS:
        try:
            t = front.target(python)
        except front.FrontError as e:
            print('FAIL %s: %s' % (python, e))
            failed += 1
            continue
        print('CPython %s (%s), %d files' % (t.version, t.pyconfig, len(files)))
        total = time.time()
        for cfile in files:
            start = time.time()
            if not os.path.isfile(os.path.join(t.src, cfile)):
                print('  FAIL %s: not in the source' % cfile)
                failed += 1
                continue
            try:
                ast = front.parse(t, cfile)
            except (front.FrontError, ParseError) as e:
                print('  FAIL %s: %s' % (cfile, e))
                failed += 1
                continue
            wrong = misplaced(ast, os.path.join(t.src, cfile))
            if wrong:
                print('  FAIL %s: line numbers wrong at %s' % (cfile, wrong))
                failed += 1
                continue
            print('  ok   %-28s %5d decls %5.2fs'
                  % (cfile, len(ast.ext), time.time() - start))
        print('  total %.1fs' % (time.time() - total))
    if failed:
        print('%d failed' % failed)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
