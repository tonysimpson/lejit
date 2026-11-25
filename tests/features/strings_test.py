"""The message probes (tools/features/strings.py) give, for each supported
CPython, a text that is in that version's source: a string literal (with
adjacent literals joined, and an integer conversion's length modifier,
%zd or %ld, taken as %d's), or a literal format with one conversion filled
in by another literal (an operator's name CPython passes as a constant,
or as the argument of a macro that joins it into its literal).
A fragment (an operator's name) is a literal, or fills its message's
conversion to make one. 3.11 and 3.12 are shown for information.

The CPython texts LeJIT keeps as they are, because no Python code reaches
them (they need malformed bytecode or more memory than there is), are
checked to be literals of the versions LeJIT uses them on.

    python3 tests/features/strings_test.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'features'))
import cpython_src  # noqa: E402
import ctokens  # noqa: E402
import strings  # noqa: E402

PYENV = os.path.expanduser('~/.pyenv/versions')
SUPPORTED = ['3.7.17', '3.8.20', '3.9.21', '3.10.16']
INFORMATION = ['3.11.11', '3.12.9', '3.13.2']

# CPython's texts LeJIT keeps as literals: (text, the versions LeJIT uses it on)
UNREACHABLE = [
    ('bad memberdescr type', SUPPORTED),
    ('popped block is not an except handler', SUPPORTED),
    ('join() result is too long for a Python string', SUPPORTED),
    ('super(): bad __class__ cell', SUPPORTED),
    ('bad RAISE_VARARGS oparg', SUPPORTED),
    ('bad BUILD_CONST_KEY_MAP keys argument', SUPPORTED),
    ('unexpected conversion flag %d', SUPPORTED[1:]),
]

_ESC = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"',
        "'": "'", '?': '?', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}


def unescape(body):
    """A C string literal's text (between the quotes)."""
    out, i = bytearray(), 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            out += c.encode('latin-1')
            i += 1
            continue
        n = body[i + 1]
        m = re.match(r'[0-7]{1,3}', body[i + 1:])
        if m:
            out.append(int(m.group(), 8) & 0xff)
            i += 1 + len(m.group())
        elif n == 'x':
            m = re.match(r'[0-9a-fA-F]+', body[i + 2:])
            out.append(int(m.group(), 16) & 0xff)
            i += 2 + len(m.group())
        else:
            out += _ESC.get(n, n).encode('latin-1')
            i += 2
    return out.decode('utf-8', 'replace')


def _joined(toks, into):
    run = []
    for t in list(toks) + [None]:
        if t is not None and t.kind == 'str' and t.text.startswith('"'):
            run.append(t.text[1:-1])
        elif run:
            into.add(unescape(''.join(run)))
            run = []


def _macro_formats(macro, into):
    """A function-like macro's literal with one parameter joined into it
    ("bad operand type for "descr": '%.200s'"), with %s for the parameter:
    the literal a call passes fills it."""
    run, holes = [], 0
    for t in list(macro.body) + [None]:
        if t is not None and t.kind == 'str' and t.text.startswith('"'):
            run.append(t.text[1:-1])
        elif t is not None and t.kind == 'id' and t.text in macro.params:
            run.append('%s')
            holes += 1
        else:
            if holes == 1 and len(run) > 1:
                into.add(unescape(''.join(run)))
            run, holes = [], 0


def literals(src, formats):
    """Every string literal's text in the tree, adjacent ones joined; each
    macro's literal with a parameter in it goes in formats."""
    out = set()
    for top in ('Include', 'Objects', 'Python', 'Modules', 'Parser'):
        for dirpath, _, files in os.walk(os.path.join(src, top)):
            for name in files:
                if not name.endswith(('.c', '.h')):
                    continue
                with open(os.path.join(dirpath, name), encoding='latin-1') as f:
                    toks, macros = ctokens.tokenize(f.read(), name)
                _joined(toks, out)
                for m in macros.values():
                    _joined(m.body, out)
                    if m.params:
                        _macro_formats(m, formats)
    return out


CONV = re.compile(r'%[-.0-9]*(?:z|l)?[a-zA-Z]')
# an integer conversion's length modifier, which follows the C argument's
# type (the caller's) and shows the same digits
INT_LENGTH = re.compile(r'%(?:z|l)d')


def same_digits(text):
    """text with the conversions that show the same as another made that
    one: an integer's length modifier dropped, and %N (a type's qualified
    name, which LeJIT passes the type for) as %T (an object's type's)"""
    return INT_LENGTH.sub('%d', text).replace('%N', '%T')


def found(text, lits, filled, lits_digits=frozenset()):
    """How `text` is in the source: 'literal', 'filled' or None."""
    if text in lits:
        return 'literal'
    if same_digits(text) in lits_digits:
        return 'literal'
    for lit in filled:
        for m in CONV.finditer(lit):
            head, tail = lit[:m.start()], lit[m.end():]
            if (text.startswith(head) and text.endswith(tail)
                    and len(text) >= len(head) + len(tail)):
                middle = text[len(head):len(text) - len(tail)]
                # a constant filled in, never a conversion
                if middle and '%' not in middle and middle in lits:
                    return 'filled'
    return None


def interpreter_of(version):
    return os.path.join(PYENV, version, 'bin', 'python' + version.rsplit('.', 1)[0])


def version_of(python):
    return subprocess.check_output(
        [python, '-I', '-c', 'import sys; print(sys.version.split()[0])'],
        universal_newlines=True).strip()


def check_version(version, python=None):
    """(problems, notes) for one version."""
    src = cpython_src.find_source(version)
    python = python or interpreter_of(version)
    formats = set()
    lits = literals(src, formats)
    filled = [l for l in lits if '%' in l] + sorted(formats)
    lits_digits = {same_digits(l) for l in lits if '%' in l}
    got = strings.probe(python)
    problems, notes = [], []
    for name in sorted(got):
        e, text = strings.entry_of(name), got[name]
        if text is None:
            notes.append('%s: no such case' % name)
            continue
        how = found(text, lits, filled, lits_digits)
        if how is None and 'parent' in e:
            pieces, convs = strings._format_pieces(got[e['parent']])
            whole = ''.join(
                p.replace('%', '%%') + (
                    (text.replace('%', '%%') if j == e['hole'] else convs[j])
                    if j < len(convs) else '')
                for j, p in enumerate(pieces))
            if whole in lits:
                how = 'fills its message'
        if how is None:
            problems.append('%s = %r is not in the source' % (name, text))
    for text, versions in UNREACHABLE:
        if version in versions and text not in lits:
            problems.append('kept literal %r is not in the source' % text)
    return problems, notes, len(got)


def main(argv):
    """Every supported version and, for information, a few newer ones;
    or just the interpreters named on the command line."""
    if argv:
        runs = [(version_of(p), p) for p in argv]
    else:
        runs = [(v, None) for v in SUPPORTED + INFORMATION]
    failed = False
    for version, python in runs:
        info = version in INFORMATION
        try:
            problems, notes, n = check_version(version, python)
        except (strings.StringError, cpython_src.SourceError) as e:
            problems, notes, n = [str(e)], [], 0
        status = 'ok' if not problems else ('differs' if info else 'FAIL')
        print('%s: %s, %d messages in the source%s%s' % (
            version, status, n - len(notes),
            ', %d cases it has not' % len(notes) if notes else '',
            ' (for information)' if info else ''))
        for p in problems:
            print('  ' + p)
        if problems and not info:
            failed = True
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
