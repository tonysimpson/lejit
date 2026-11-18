"""Definitions in C source by name, and their shapes (GENERATION step 3).

A definition is named 'path:name', path relative to the tree it is in:
'Objects/abstract.c:PyNumber_Add' (a function), 'Include/object.h:Py_DECREF'
(a macro), 'Python/ceval.c:TARGET(LOAD_GLOBAL)' (an opcode case),
'Include/listobject.h:PyListObject' (a struct, by tag or typedef name),
'Objects/longobject.c:PyLong_Type' (a variable with an initializer), and in
Ni's own tree 'ni/Python/pycompiler.c:case LOAD_GLOBAL' (a case label).

Its shape is a hash of its tokens (ctokens.py: comments and spacing don't
count, preprocessor lines other than #define are dropped), so any change to
the C a policy entry was written for changes the shape.
"""
import hashlib
import os

import ctokens

# C words that look like calls or declarations but are neither
KEYWORDS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'do', 'else',
            'case', 'goto', 'break', 'continue', 'default', 'typedef',
            'struct', 'union', 'enum', 'static', 'const', 'volatile',
            'extern', 'inline', 'register', 'unsigned', 'signed', 'void',
            'char', 'short', 'int', 'long', 'float', 'double', '_Bool',
            'defined', '_Static_assert', 'Py_LOCAL_INLINE', 'PyAPI_FUNC',
            'PyAPI_DATA'}


class Def:
    """One definition: kind is function, macro, target, struct, var or
    case; toks its tokens; for a function also ret (return type tokens),
    params and body."""

    def __init__(self, kind, name, toks, ret=None, params=None, body=None,
                 path=None):
        self.kind, self.name, self.toks = kind, name, toks
        self.ret, self.params, self.body, self.path = ret, params, body, path

    def shape(self):
        h = hashlib.sha256('\x1f'.join(t.text for t in self.toks).encode())
        return h.hexdigest()[:12]


def scan(toks, macros, path):
    """{name: Def} for the top-level definitions in a file's tokens (the
    last one wins when the preprocessor offers two)."""
    out = {}
    for m in macros.values():
        out[m.name] = Def('macro', m.name, [ctokens.Tok('id', m.name, m.line, 0)]
                          + ([ctokens.Tok('op', '(', 0, 0)] + [
                              ctokens.Tok('id', p, 0, 0) for p in m.params]
                             + [ctokens.Tok('op', ')', 0, 0)]
                             if m.params is not None else []) + list(m.body),
                          path=path)
    start = 0          # first token of the current top-level construct
    i = 0
    while i < len(toks):
        t = toks[i]
        if t.text in (';', '}'):     # '}': the end of an extern "C" {
            start = i + 1
        elif t.text == '{':
            head = toks[start:i]
            words = [x.text for x in head]
            if words == ['extern', '"C"']:
                i = start = i + 1       # a header's C++ wrapper: look inside
                continue
            end = ctokens.match_close(toks, i)
            if words and words[-1] == ')' and _function_name(head):
                k = _function_name(head)
                name = head[k].text
                # the return type: type words just before the name (a
                # macro call with no ';' before it is not part of it)
                paren = head[k - 1].text == '('
                r = k - paren
                while r > 0 and (head[r - 1].text == '*' or (
                        head[r - 1].kind == 'id')):
                    r -= 1
                params = head[k + 2 + 2 * paren:len(head) - 1]
                out[name] = Def('function', name, head[r:] + toks[i:end + 1],
                                ret=head[r:k - paren], params=params,
                                body=toks[i + 1:end], path=path)
                if name == '_PyEval_EvalFrameDefault':
                    out.update(_targets(toks, i, end, path))
            elif 'struct' in words or 'union' in words:
                tail = toks[end + 1:end + 3]
                names = [x.text for x in head if x.kind == 'id'
                         and x.text not in KEYWORDS]
                if tail and tail[0].kind == 'id':
                    names.append(tail[0].text)
                semi = next(j for j in range(end, len(toks)) if toks[j].text == ';')
                for n in names[-2:]:
                    out[n] = Def('struct', n, toks[start:semi + 1], path=path)
            elif '=' in words:
                # the last name outside brackets: a[2][N + 1] = is 'a'
                depth, ids = 0, []
                for x in head[:words.index('=')]:
                    depth += (x.text in '([') - (x.text in ')]')
                    if depth == 0 and x.kind == 'id':
                        ids.append(x.text)
                name = ids[-1]
                semi = next(j for j in range(end, len(toks)) if toks[j].text == ';')
                out[name] = Def('var', name, toks[start:semi + 1], path=path)
            i = end + 1
            if i < len(toks) and toks[i].text == ';':
                i += 1
            start = i
            continue
        i += 1
    return out


def _function_name(head):
    """Index in head of the name of 'RET NAME ( PARAMS )', or None."""
    if len(head) < 3 or head[-1].text != ')':
        return None
    depth = 0
    for j in range(len(head) - 1, -1, -1):
        if head[j].text == ')':
            depth += 1
        elif head[j].text == '(':
            depth -= 1
            if depth == 0:
                if (j > 0 and head[j - 1].kind == 'id'
                        and head[j - 1].text not in KEYWORDS
                        and '=' not in [x.text for x in head]):
                    return j - 1
                if (j > 2 and head[j - 1].text == ')' and head[j - 3].text == '('
                        and head[j - 2].kind == 'id'):
                    return j - 2        # (name)(params): kept from a macro
                return None
    return None


def _targets(toks, lo, hi, path):
    """TARGET(X) cases of ceval's loop: each from its TARGET to the next.
    Where the label is written 'case TARGET(X):' (3.8) the 'case' and ':'
    are the label's, not the case's, so they don't count in its shape."""
    at = [j for j in range(lo, hi) if toks[j].text == 'TARGET'
          and toks[j + 1].text == '(' and toks[j + 3].text == ')']
    out = {}
    for k, j in enumerate(at):
        end = at[k + 1] if k + 1 < len(at) else hi
        if k + 1 < len(at) and toks[end - 1].text == 'case':
            end -= 1
        body = toks[j:j + 4] + toks[j + 4 + (toks[j + 4].text == ':'):end]
        name = 'TARGET(%s)' % toks[j + 2].text
        out[name] = Def('target', name, body, path=path)
    return out


def _cases(toks, path):
    """'case X' labels of a C switch (Ni's opcode cases): from the label to
    the next case label or the end of the enclosing block."""
    out = {}
    at = [j for j in range(len(toks) - 2) if toks[j].text == 'case'
          and toks[j + 1].kind == 'id' and toks[j + 2].text == ':']
    for k, j in enumerate(at):
        end = at[k + 1] if k + 1 < len(at) else j + 3
        out['case ' + toks[j + 1].text] = Def('case', toks[j + 1].text,
                                              toks[j:end], path=path)
    return out


class Tree:
    """A source tree, read one file at a time as names ask for them."""

    def __init__(self, root):
        self.root = root
        self.files = {}

    def file(self, path):
        """(tokens, {name: Def}) of one file, or None if it doesn't exist."""
        if path not in self.files:
            full = os.path.join(self.root, path)
            if not os.path.isfile(full):
                self.files[path] = None
            else:
                with open(full, encoding='latin-1') as f:
                    toks, macros = ctokens.tokenize(f.read(), path)
                try:
                    defs = scan(toks, macros, path)
                except (SyntaxError, StopIteration):
                    # braces unbalanced by #if branches: its macros only
                    defs = scan([], macros, path)
                defs.update(_cases(toks, path))
                self.files[path] = (toks, defs)
        return self.files[path]

    def find(self, key):
        """The Def named 'path:name', or None. A header's definition is
        looked for in every Include/**/*.h when its own header doesn't have
        it, as the mechanisms and layouts do: from 3.8 many moved into
        Include/cpython/ (PyTupleObject, Py_SETREF), so the key keeps naming
        the header Ni was written against."""
        path, _, name = key.partition(':')
        f = self.file(path)
        d = f[1].get(name) if f else None
        if d is None and path.startswith('Include/'):
            for h in self.headers():
                g = self.file(h)
                if g and name in g[1]:
                    return g[1][name]
        return d

    def headers(self):
        """Every Include/**/*.h of the tree, relative, sorted."""
        if not hasattr(self, '_headers'):
            inc = os.path.join(self.root, 'Include')
            self._headers = sorted(
                os.path.relpath(os.path.join(d, f), self.root)
                for d, _, fs in os.walk(inc) for f in fs if f.endswith('.h'))
        return self._headers
