"""C text from the AST: run-time copies of CPython's code, expressions in
generated conditions, and statements named in the report.

The AST is preprocessed, so a copy is CPython's code with its macros
expanded; a vocabulary marker is printed as the macro it stands for, so the
copy's reference counting reads as the source's. The report names a
statement by its text in the source file (as written, macros unexpanded),
which survives a change of header; where the AST's position doesn't lead
there (code a macro of the file writes), by the printed AST.
"""
import os
import sys

from pycparser import c_ast, c_generator

from . import vocabulary
from .ctyping import strip_casts

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import ctokens  # noqa: E402


def offsetof(e):
    """(struct type text, field) when e is offsetof's expansion
    '(unsigned long) &((T *) 0)->m', else None."""
    if not isinstance(e, c_ast.Cast):
        return None
    x = e.expr
    if not (isinstance(x, c_ast.UnaryOp) and x.op == '&' and isinstance(x.expr, c_ast.StructRef)
            and x.expr.type == '->'):
        return None
    base = x.expr.name
    if isinstance(base, c_ast.Cast) and isinstance(base.expr, c_ast.Constant) \
            and base.expr.value == '0':
        from .ctyping import text
        t = text(base.to_type).rstrip(' *')
        return t, x.expr.field.name
    return None


def is_null(e):
    """NULL as the preprocessor leaves it: '((void *) 0)' or 0."""
    e = strip_casts(e)
    return isinstance(e, c_ast.Constant) and e.value == '0'


class Printer(c_generator.CGenerator):
    """pycparser's C, with vocabulary markers as their macros, offsetof
    and NULL as written, and calls renamed (a static's run-time copy)."""

    def __init__(self, rename=None):
        super().__init__()
        self.rename = rename or {}

    def visit_FuncCall(self, n):
        if isinstance(n.name, c_ast.ID):
            v = vocabulary.of_marker(n.name.name)
            name = v.name if v else self.rename.get(n.name.name)
            if name:
                args = self.visit(n.args) if n.args is not None else ''
                return '%s(%s)' % (name, args)
        return super().visit_FuncCall(n)

    def visit_Cast(self, n):
        o = offsetof(n)
        if o is not None:
            return 'offsetof(%s, %s)' % o
        if is_null(n) and isinstance(n.expr, c_ast.Constant):
            return 'NULL'
        return super().visit_Cast(n)


def expr(e):
    """One line of C for expression e."""
    return Printer().visit(e)


def stmt(s, rename=None):
    """C for statement s, as it would stand in a block."""
    p = Printer(rename)
    if isinstance(s, c_ast.Decl):
        return p.visit(s) + ';'
    return p._generate_stmt(s).rstrip('\n')


_SOURCES = {}


def _source(path):
    if path not in _SOURCES:
        try:
            with open(path, encoding='latin-1') as f:
                _SOURCES[path] = f.read().split('\n')
        except OSError:
            _SOURCES[path] = None
    return _SOURCES[path]


def stmt_text(s, limit=70):
    """The statement as the source writes it, spaced as written and on
    one line, up to `limit` characters."""
    # (a macro's trailing ';' is placed past the end of its line)
    found = ';' if isinstance(s, c_ast.EmptyStatement) else _from_source(s)
    if found is None:
        found = ' '.join(stmt(s).split())
    return found[:limit]


def _from_source(s):
    c = s.coord
    lines = _source(c.file) if c is not None and c.file else None
    if not lines or c.line > len(lines) or not c.column:
        return None
    line = lines[c.line - 1]
    col = c.column - 1
    if col >= len(line):
        return None
    want = _first_word(s)
    if isinstance(s, c_ast.Decl) and want:
        # a declaration's position is its declarator's
        col = line.rfind(want, 0, col + 1)
        if col < 0:
            return None
    first = line[col:]
    for n in (60, 200, 30, 10, 3, 0):
        # a comment cut off where the text ends doesn't tokenize
        try:
            toks = ctokens.tokenize('\n'.join([first] + lines[c.line:c.line + n]))[0]
        except SyntaxError:
            continue
        end = _end(toks, 0)
        if end is not None:
            break
    else:
        return None
    if not toks or (toks[0].text != want and (
            isinstance(s, c_ast.Decl) or toks[0].kind != 'id')):
        # (a statement a macro writes, Py_RETURN_NONE, starts with its name)
        return None
    out = toks[:end + 1]
    return ''.join((' ' if t.sp and k else '') + t.text for k, t in enumerate(out))


def _first_word(s):
    """The first token the statement's C starts with."""
    toks = ctokens.tokenize(stmt(s))[0]
    return toks[0].text if toks else None


def _close(toks, i):
    pairs = {'(': ')', '{': '}', '[': ']'}
    want, depth = pairs[toks[i].text], 0
    for j in range(i, len(toks)):
        if toks[j].text in pairs:
            depth += 1
        elif toks[j].text in pairs.values():
            depth -= 1
            if depth == 0:
                return j if toks[j].text == want else None
    return None


def _end(toks, i):
    """The index of the last token of the statement starting at toks[i]."""
    if i >= len(toks):
        return None
    t = toks[i].text
    if t == '{':
        return _close(toks, i)
    if t in ('if', 'while', 'for', 'switch'):
        j = _close(toks, i + 1) if i + 1 < len(toks) and toks[i + 1].text == '(' else None
        k = _end(toks, j + 1) if j is not None else None
        if k is not None and t == 'if' and k + 1 < len(toks) and toks[k + 1].text == 'else':
            return _end(toks, k + 2)
        return k
    if t == 'do':
        k = _end(toks, i + 1)
        if k is None or k + 2 >= len(toks) or toks[k + 1].text != 'while':
            return None
        j = _close(toks, k + 2)
        return j + 1 if j is not None else None
    if i + 1 < len(toks) and toks[i].kind == 'id' and toks[i + 1].text == ':':
        return _end(toks, i + 2)
    depth = 0
    for j in range(i, len(toks)):
        x = toks[j].text
        if x in '({[':
            depth += 1
        elif x in ')}]':
            depth -= 1
        elif x == ';' and depth == 0:
            return j
    return None
