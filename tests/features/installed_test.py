"""The installed detector (tools/features/installed.py) agrees with the
source detector (tools/features/features.py) on every interpreter: the
installed one asking the interpreter and its headers, the source one
reading that release's source (cpython_src.py, cached).

    python3 tests/features/installed_test.py [--strict] [PYTHON...]

Without PYTHONs it checks the supported interpreters, which must agree,
and then the unsupported ones it finds, which are only reported. A wrong
answer on a supported interpreter fails; so does an unanswered feature
with --strict, which is what building with the installed detector needs.
When both detectors answer everything, the headers they write must be the
same but for their first line.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'features'))
import cpython_src  # noqa: E402
import features  # noqa: E402
import installed  # noqa: E402

PYENV = os.path.expanduser('~/.pyenv/versions')
SUPPORTED = [os.path.join(PYENV, v, 'bin', exe) for v, exe in (
    ('3.7.17', 'python3.7'), ('3.8.20', 'python3.8'),
    ('3.9.21', 'python3.9'), ('3.10.16', 'python3.10'))]
UNSUPPORTED = [os.path.join(PYENV, v, 'bin', exe) for v, exe in (
    ('3.11.11', 'python3.11'), ('3.12.9', 'python3.12'),
    ('3.13.2', 'python3.13'))]


def source_answers(src):
    """{LE_HAS_name: value or the reason it is unanswered}, each feature
    asked on its own so that one unanswered doesn't hide the rest."""
    s = features.Source(src)
    out = {}
    for name, d in features.FEATURES:
        try:
            out['LE_HAS_' + name] = int(bool(d(s)[0]))
        except features.Unanswered as e:
            out['LE_HAS_' + name] = str(e)
    return out


def check(python):
    """(row for the table, wrong, unanswered by the installed detector)"""
    probed, found, unanswered = installed.detect(python)
    version = probed['version']
    src = cpython_src.find_source(version, log=lambda *a: None)
    want = source_answers(src)
    got = {name: value for name, value, _, _, _ in found}
    wrong, src_unanswered = [], []
    for name, _ in features.FEATURES:
        key = 'LE_HAS_' + name
        if isinstance(want[key], str):
            src_unanswered.append(key)
            if key in got:
                print('  %s %s: the source can\'t answer (%s); installed says %d'
                      % (version, key, want[key], got[key]))
        elif key in got and got[key] != want[key]:
            wrong.append(key)
            print('  WRONG %s %s: installed %d (%s), source %d' % (
                version, key, got[key],
                [f[3] for f in found if f[0] == key][0], want[key]))
    for key in sorted(unanswered):
        print('  %s %s: installed can\'t answer: %s' % (version, key, unanswered[key]))
    try:
        theirs = features.opcodes(src)
        mine = installed.opcodes(probed)
        for field in theirs._fields:
            if getattr(mine, field) != getattr(theirs, field):
                wrong.append('opcodes ' + field)
                print('  WRONG %s: opcodes %s %r, source %r' % (
                    version, field, getattr(mine, field), getattr(theirs, field)))
    except features.FeatureError as e:
        print('  %s: the source has no opcode tables (%s)' % (version, e))
    if not unanswered and not src_unanswered and not wrong:
        mine = installed.build(python)[0].split('\n', 1)[1]
        theirs = features.build(src, version).split('\n', 1)[1]
        if mine != theirs:
            wrong.append('header')
            print('  WRONG %s: the headers differ' % version)
    agree = len(features.FEATURES) - len(wrong) - len(unanswered) - len(
        [k for k in src_unanswered if k not in unanswered])
    row = '%-8s %5d %5d %11d %11d' % (version, agree, len(wrong),
                                     len(unanswered), len(src_unanswered))
    return row, wrong, unanswered


def main(argv):
    strict = '--strict' in argv
    pythons = [a for a in argv if a != '--strict']
    checks = [(p, True) for p in pythons] or (
        [(p, True) for p in SUPPORTED]
        + [(p, False) for p in UNSUPPORTED if os.path.exists(p)])
    rows, failed = [], 0
    for python, supported in checks:
        print('%s%s' % (python, '' if supported else ' (not supported: reported only)'))
        row, wrong, unanswered = check(python)
        rows.append(row + ('' if supported else '  not supported'))
        if supported and (wrong or (strict and unanswered)):
            failed += 1
    print('\n%-8s %5s %5s %11s %11s' % ('version', 'agree', 'wrong',
                                       'unanswered', 'source'))
    print('%-8s %5s %5s %11s %11s' % ('', '', '', '(installed)', 'unanswered'))
    for row in rows:
        print(row)
    print('%d features each; %s' % (
        len(features.FEATURES),
        'FAIL' if failed else 'ok (no wrong answers%s)' % (
            '' if strict else '; --strict also requires every feature answered')))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
