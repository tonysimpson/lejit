"""C statements and expressions over ctokens' tokens (GENERATION step 4).

The token rules of the earlier generators match fixed shapes; general
translation (gen_abstract.py) needs the structure of a function body: which
statement is inside which 'if', and the operands of each operator. This
reads it, still from ctokens' tokens and with no dependency: a statement
reader (blocks, declarations, if/else, return, goto, labels) and an
operator-precedence expression reader. Anything else (loops, switch, the
comma operator, ++) becomes an 'unsupported' node that keeps its tokens,
so a caller can refuse it, or copy the C as it is. With loops=True (the
general translator's reading, D78) loops, break, continue, ++ and -- and
the commas of a for-loop's head are read too.

What it needs from outside is which identifiers name types (a declaration
and a cast look like expressions otherwise); Decls.read gets them, with
struct fields, prototypes and the function-pointer typedefs, from the
preprocessor's output (cpp() below), which also gives every macro as the
compiled file sees it (#if branches resolved).

Nodes are tuples whose first item is the kind:
  expressions: ('id', name) ('num', text) ('str', [texts]) ('call', f, args)
    ('arrow', e, field) ('dot', e, field) ('index', e, i) ('un', op, e)
    ('cast', type_text, e) ('bin', op, a, b) ('cond', c, a, b)
    ('assign', op, lhs, rhs) ('sizeof', text) ('init', [exprs])
  statements (each ends with its token span (lo, hi), hi exclusive):
    ('block', [stmts], span) ('decl', [(ctype, name, init)], span)
    ('expr', e, span) ('if', cond, then, else_or_None, span)
    ('return', e_or_None, span) ('goto', label, span)
    ('label', name, stmt, span) ('empty', span) ('unsupported', why, span)
  with loops=True, also: ('for', init, cond_or_None, [step exprs], body,
    span), init being a 'decl' or 'expr' statement, or None, with a
    comma list read as ('comma', [exprs]); ('while', cond, body, span)
    ('do', body, cond, span) ('break', span) ('continue', span); and the
    expressions ('pre', op, e) and ('post', op, e) for ++ and --.
  with switches=True as well (the general translator's reading, D84):
    ('switch', cond, body, span), whose body holds ('case', e, span) and
    ('default', span) labels among its statements; a 'do ... while (0)'
    with no break of its own is the ('block', [body], span) it stands for
    (a statement macro's body).
"""
import os
import re
import subprocess

import ctokens

TYPE_WORDS = {'void', 'char', 'short', 'int', 'long', 'float', 'double',
              'signed', 'unsigned', 'const', 'volatile', 'struct', 'union',
              'enum', '_Bool', 'static', 'register', 'inline'}
BINARY = {'||': 1, '&&': 2, '|': 3, '^': 4, '&': 5, '==': 6, '!=': 6,
          '<': 7, '>': 7, '<=': 7, '>=': 7, '<<': 8, '>>': 8, '+': 9,
          '-': 9, '*': 10, '/': 10, '%': 10}
ASSIGN = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='}


class ParseError(Exception):
    pass


class Parser:
    def __init__(self, toks, types, loops=False, switches=False):
        self.toks = toks
        self.types = types          # identifiers that name types
        self.loops = loops          # read loops, break, continue, ++, --
        self.switches = switches    # read switch, case, default, do-while(0)
        self.i = 0

    # ---- helpers ----
    def peek(self, k=0):
        j = self.i + k
        return self.toks[j].text if j < len(self.toks) else None

    def take(self, text=None):
        if self.i >= len(self.toks):
            raise ParseError('unexpected end')
        t = self.toks[self.i]
        if text is not None and t.text != text:
            raise ParseError('line %d: expected %r, got %r' % (t.line, text, t.text))
        self.i += 1
        return t

    def is_type_start(self, k=0):
        t = self.peek(k)
        if t is None:
            return False
        if t in TYPE_WORDS:
            return True
        tok = self.toks[self.i + k]
        return tok.kind == 'id' and t in self.types

    # ---- statements ----
    def statements(self):
        out = []
        while self.i < len(self.toks):
            out.append(self.statement())
        return out

    def statement(self):
        lo = self.i
        t = self.peek()
        try:
            if t == '{':
                self.take('{')
                body = []
                while self.peek() != '}':
                    body.append(self.statement())
                self.take('}')
                return ('block', body, (lo, self.i))
            if t == 'if':
                self.take('if')
                self.take('(')
                c = self.expr()
                self.take(')')
                then = self.statement()
                els = None
                if self.peek() == 'else':
                    self.take('else')
                    els = self.statement()
                return ('if', c, then, els, (lo, self.i))
            if t == 'return':
                self.take('return')
                e = None if self.peek() == ';' else self.expr()
                self.take(';')
                return ('return', e, (lo, self.i))
            if t == 'goto':
                self.take('goto')
                name = self.take().text
                self.take(';')
                return ('goto', name, (lo, self.i))
            if t == ';':
                self.take(';')
                return ('empty', (lo, self.i))
            if self.switches and t in ('switch', 'case', 'default'):
                return self.switch(lo, t)
            if self.loops and t in ('for', 'while', 'do', 'break', 'continue'):
                return self.loop(lo, t)
            if t in ('while', 'for', 'do', 'switch', 'break', 'continue',
                     'case', 'default'):
                return self.skip(lo, t)
            if (self.toks[self.i].kind == 'id' and self.peek(1) == ':'
                    and not self.is_type_start()):
                name = self.take().text
                self.take(':')
                return ('label', name, self.statement(), (lo, self.i))
            if self.is_type_start() and not (
                    self.peek(1) in ('(', '.', '->', '=', ')', ',', ';', '[')
                    and self.peek() not in TYPE_WORDS):
                return self.declaration(lo)
            e = self.expr()
            self.take(';')
            return ('expr', e, (lo, self.i))
        except ParseError as err:
            self.i = lo
            return self.skip(lo, str(err))

    def loop(self, lo, t):
        self.take(t)
        if t in ('break', 'continue'):
            self.take(';')
            return (t, (lo, self.i))
        if t == 'while':
            self.take('(')
            c = self.expr()
            self.take(')')
            body = self.statement()
            return ('while', c, body, (lo, self.i))
        if t == 'do':
            body = self.statement()
            self.take('while')
            self.take('(')
            c = self.expr()
            self.take(')')
            self.take(';')
            if self.switches and c == ('num', '0') and not breaks_in(body):
                return ('block', [body], (lo, self.i))
            return ('do', body, c, (lo, self.i))
        self.take('(')
        ilo = self.i
        if self.peek() == ';':
            self.take(';')
            init = None
        elif self.is_type_start() and not (
                self.peek(1) in ('(', '.', '->', '=', ')', ',', ';', '[')
                and self.peek() not in TYPE_WORDS):
            init = self.declaration(ilo)
        else:
            init = ('expr', self.comma_list(';'), (ilo, self.i))
            self.take(';')
        c = None if self.peek() == ';' else self.expr()
        self.take(';')
        step = [] if self.peek() == ')' else self.comma_list(')')[1]
        self.take(')')
        body = self.statement()
        return ('for', init, c, step, body, (lo, self.i))

    def switch(self, lo, t):
        self.take(t)
        if t == 'default':
            self.take(':')
            return ('default', (lo, self.i))
        if t == 'case':
            e = self.conditional()
            self.take(':')
            return ('case', e, (lo, self.i))
        self.take('(')
        c = self.expr()
        self.take(')')
        body = self.statement()
        return ('switch', c, body, (lo, self.i))

    def comma_list(self, end):
        items = [self.assignment()]
        while self.peek() == ',':
            self.take(',')
            items.append(self.assignment())
        if self.peek() != end:
            raise ParseError('expected %r' % end)
        return ('comma', items) if len(items) > 1 or end == ')' else items[0]

    def skip(self, lo, why):
        """An unsupported statement: its tokens up to the end of the
        statement (a ';' at depth 0, or a closing brace block)."""
        self.i = lo
        depth = 0
        while self.i < len(self.toks):
            t = self.peek()
            if t in ('(', '[', '{'):
                depth += 1
            elif t in (')', ']', '}'):
                depth -= 1
                if depth < 0:
                    break
                if depth == 0 and t == '}' and self.peek(1) not in ('while', 'else'):
                    self.i += 1
                    break
            elif t == ';' and depth == 0:
                self.i += 1
                break
            self.i += 1
        return ('unsupported', why, (lo, self.i))

    def base_type(self):
        words = []
        while self.is_type_start():
            t = self.take().text
            words.append(t)
            if t in ('struct', 'union', 'enum'):
                words.append(self.take().text)
            elif t in self.types:
                break
        # trailing qualifiers
        while self.peek() in ('const', 'volatile'):
            words.append(self.take().text)
        return [w for w in words if w not in ('static', 'register', 'inline')]

    def declaration(self, lo):
        base = self.base_type()
        decls = []
        while True:
            stars = 0
            quals = []
            while self.peek() in ('*', 'const'):
                t = self.take().text
                if t == '*':
                    stars += 1
                else:
                    quals.append(t)
            if self.peek() == '(':
                raise ParseError('function-pointer declarator')
            name = self.take().text
            dims = []
            while self.peek() == '[':
                self.take('[')
                d = self.expr() if self.peek() != ']' else None
                self.take(']')
                dims.append(d)
            init = None
            if self.peek() == '=':
                self.take('=')
                init = self.initializer()
            ctype = ' '.join(base) + (' ' + '*' * stars if stars else '')
            decls.append((ctype, name, init, dims))
            if self.peek() == ',':
                self.take(',')
                continue
            self.take(';')
            break
        return ('decl', decls, (lo, self.i))

    def initializer(self):
        if self.peek() == '{':
            self.take('{')
            items = []
            while self.peek() != '}':
                items.append(self.assignment())
                if self.peek() == ',':
                    self.take(',')
            self.take('}')
            return ('init', items)
        return self.assignment()

    # ---- expressions ----
    def expr(self):
        e = self.assignment()
        if self.peek() == ',':
            raise ParseError('comma operator')
        return e

    def assignment(self):
        lhs = self.conditional()
        if self.peek() in ASSIGN:
            op = self.take().text
            return ('assign', op, lhs, self.assignment())
        return lhs

    def conditional(self):
        c = self.binary(1)
        if self.peek() == '?':
            self.take('?')
            a = self.expr()
            self.take(':')
            b = self.conditional()
            return ('cond', c, a, b)
        return c

    def binary(self, prec):
        lhs = self.unary()
        while True:
            op = self.peek()
            p = BINARY.get(op)
            if p is None or p < prec:
                return lhs
            self.take()
            rhs = self.binary(p + 1)
            lhs = ('bin', op, lhs, rhs)

    def unary(self):
        t = self.peek()
        if t in ('!', '-', '+', '~', '*', '&'):
            self.take()
            return ('un', t, self.unary())
        if t in ('++', '--'):
            if not self.loops:
                raise ParseError('%s' % t)
            self.take()
            return ('pre', t, self.unary())
        if t == 'sizeof':
            self.take()
            lo = self.i
            if self.peek() == '(':
                close = ctokens.match_close(self.toks, self.i)
                self.i = close + 1
            else:
                self.unary()
            return ('sizeof', ' '.join(x.text for x in self.toks[lo:self.i]))
        if t == '(' and self.is_type_start(1):
            self.take('(')
            words = []
            while self.peek() != ')':
                words.append(self.take().text)
            self.take(')')
            return ('cast', ' '.join(words), self.unary())
        return self.postfix(self.primary())

    def postfix(self, e):
        while True:
            t = self.peek()
            if t == '(':
                self.take('(')
                args = []
                while self.peek() != ')':
                    args.append(self.assignment())
                    if self.peek() == ',':
                        self.take(',')
                self.take(')')
                e = ('call', e, args)
            elif t == '[':
                self.take('[')
                i = self.expr()
                self.take(']')
                e = ('index', e, i)
            elif t == '->':
                self.take('->')
                e = ('arrow', e, self.take().text)
            elif t == '.':
                self.take('.')
                e = ('dot', e, self.take().text)
            elif t in ('++', '--'):
                if not self.loops:
                    raise ParseError('%s' % t)
                self.take()
                e = ('post', t, e)
            else:
                return e

    def primary(self):
        tok = self.take()
        if tok.kind == 'id':
            return ('id', tok.text)
        if tok.kind == 'num':
            return ('num', tok.text)
        if tok.kind == 'str':
            texts = [tok.text]
            while self.i < len(self.toks) and self.toks[self.i].kind == 'str':
                texts.append(self.take().text)
            return ('str', texts)
        if tok.text == '(':
            e = self.assignment()
            # '(assert(X), E)', as PEP 393's accessor macros write it: the
            # assert is the statement the translator ignores, so this is E
            while self.peek() == ',' and e[0] == 'call' and e[1] == ('id', 'assert'):
                self.take(',')
                e = self.assignment()
            if self.peek() == ',':
                raise ParseError('comma operator')
            self.take(')')
            return e
        raise ParseError('line %d: unexpected %r' % (tok.line, tok.text))


def _children(s):
    for x in s[1:]:
        if isinstance(x, tuple):
            yield x
        elif isinstance(x, list):
            yield from (y for y in x if isinstance(y, tuple))


def breaks_in(s):
    """Does statement s hold a break or continue of its own (not one of a
    loop or switch inside it; a continue inside a switch is its own)?"""
    if not isinstance(s, tuple) or not s:
        return False
    if s[0] in ('break', 'continue'):
        return True
    if s[0] in ('for', 'while', 'do'):
        return False
    if s[0] == 'switch':
        return continues_in(s[2])
    return any(breaks_in(x) for x in _children(s))


def continues_in(s):
    if not isinstance(s, tuple) or not s:
        return False
    if s[0] == 'continue':
        return True
    if s[0] in ('for', 'while', 'do'):
        return False
    return any(continues_in(x) for x in _children(s))


def parse_body(toks, types, loops=False, switches=False):
    """The statements of a function body (the tokens inside its braces)."""
    return Parser(toks, types, loops, switches).statements()


def parse_expr(toks, types, loops=False):
    p = Parser(toks, types, loops)
    e = p.expr()
    if p.i != len(toks):
        raise ParseError('trailing tokens in %r' % ' '.join(t.text for t in toks))
    return e


# ---- printing an expression back as C ---------------------------------------

def c_expr(e):
    k = e[0]
    if k in ('id', 'num'):
        return e[1]
    if k == 'str':
        return ' '.join(e[1])
    if k == 'call':
        fn = c_expr(e[1])
        if e[1][0] == 'un':
            fn = '(%s)' % fn        # (*f)(x), not *(f)(x)
        return '%s(%s)' % (fn, ', '.join(c_expr(a) for a in e[2]))
    if k == 'arrow':
        return '%s->%s' % (c_expr(e[1]), e[2])
    if k == 'dot':
        return '%s.%s' % (c_expr(e[1]), e[2])
    if k == 'index':
        return '%s[%s]' % (c_expr(e[1]), c_expr(e[2]))
    if k == 'un':
        return '%s(%s)' % (e[1], c_expr(e[2]))
    if k == 'cast':
        return '((%s)%s)' % (e[1], c_expr(e[2]))
    if k == 'bin':
        return '(%s %s %s)' % (c_expr(e[2]), e[1], c_expr(e[3]))
    if k == 'cond':
        return '(%s ? %s : %s)' % tuple(c_expr(x) for x in e[1:])
    if k == 'assign':
        return '%s %s %s' % (c_expr(e[2]), e[1], c_expr(e[3]))
    if k == 'sizeof':
        return e[1]
    if k == 'pre':
        return '%s%s' % (e[1], c_expr(e[2]))
    if k == 'post':
        return '%s%s' % (c_expr(e[2]), e[1])
    if k == 'comma':
        return ', '.join(c_expr(x) for x in e[1])
    raise ValueError(k)


def names_in(e, out=None):
    """Identifiers read in an expression or statement tree, in order."""
    if out is None:
        out = []
    if isinstance(e, tuple):
        if e and e[0] == 'id':
            out.append(e[1])
        elif e and e[0] in ('arrow', 'dot'):
            names_in(e[1], out)
        else:
            for x in e[1:]:
                names_in(x, out)
    elif isinstance(e, list):
        for x in e:
            names_in(x, out)
    return out


# ---- macros -------------------------------------------------------------------

def expand(toks, macros, keep, active=frozenset()):
    """Expand function-like macros (and object-like ones in 'objlike')
    in a token list, except those named in 'keep'. Standard rescanning;
    a macro using # or ## is left as it is (a caller then sees an
    unknown call)."""
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        m = macros.get(t.text) if t.kind == 'id' else None
        if m is None or t.text in keep or t.text in active:
            out.append(t)
            i += 1
            continue
        if any(b.text in ('#', '##') for b in m.body) and m.params is None:
            out.append(t)
            i += 1
            continue
        if m.params is None:
            body = [b._replace(line=t.line, sp=t.sp) for b in m.body]
            out.extend(expand(body, macros, keep, active | {t.text}))
            i += 1
            continue
        if i + 1 >= len(toks) or toks[i + 1].text != '(':
            out.append(t)
            i += 1
            continue
        close = ctokens.match_close(toks, i + 1)
        args = ctokens.split_top(toks[i + 2:close], ',')
        if len(m.params) == 0 and not args:
            args = []
        if len(args) != len(m.params) and not (m.params and m.params[-1] == '...'):
            out.append(t)
            i += 1
            continue
        if any(b.text in ('#', '##') for b in m.body):
            body = _paste(m, args, [expand(a, macros, keep, active) for a in args], t)
            if body is None:
                out.append(t)
                i += 1
                continue
        else:
            args = [expand(a, macros, keep, active) for a in args]
            env = dict(zip(m.params, args))
            body = []
            for b in m.body:
                if b.kind == 'id' and b.text in env:
                    # the argument's first token is spaced as the parameter
                    body.extend([a._replace(sp=b.sp) if j == 0 else a
                                 for j, a in enumerate(env[b.text])])
                else:
                    body.append(b._replace(line=t.line))
        if body:
            body[0] = body[0]._replace(sp=t.sp)
        out.extend(expand(body, macros, keep, active | {t.text}))
        i = close + 1
    return out


def _paste(m, raw, expanded, t):
    """The body of a function-like macro using # or ## with these
    arguments, as C does it: '# p' is the argument's text as a string, an
    operand of '##' is the argument unexpanded and the two sides are
    joined into one token; other parameters are the expanded argument.
    None if a paste does not give one token (the macro is left as it
    is)."""
    env_raw = dict(zip(m.params, raw))
    env_exp = dict(zip(m.params, expanded))
    body, out = m.body, []
    k = 0
    while k < len(body):
        b = body[k]
        if b.text == '#' and k + 1 < len(body) and body[k + 1].text in env_raw:
            words = env_raw[body[k + 1].text]
            text = ''.join((' ' if j and w.sp else '') + w.text for j, w in enumerate(words))
            out.append(ctokens.Tok('str', '"%s"' % text.replace('\\', '\\\\').replace('"', '\\"'),
                                   t.line, b.sp))
            k += 2
            continue
        if b.text == '##':
            out.append(b._replace(kind='paste', line=t.line))
            k += 1
            continue
        pasted = (k + 1 < len(body) and body[k + 1].text == '##') or \
            (k > 0 and body[k - 1].text == '##')
        if b.kind == 'id' and b.text in env_raw:
            out.extend([a._replace(sp=b.sp) if j == 0 else a
                        for j, a in enumerate((env_raw if pasted else env_exp)[b.text])])
        else:
            out.append(b._replace(line=t.line))
        k += 1
    res = []
    for k, b in enumerate(out):
        if res and res[-1].kind == 'paste':
            res.pop()
            left = res.pop() if res else None
            text = (left.text if left else '') + b.text
            toks = ctokens.tokenize(text)[0]
            if len(toks) != 1:
                return None
            res.append(toks[0]._replace(line=t.line, sp=left.sp if left else b.sp))
            continue
        res.append(b)
    return res


# ---- what the compiler sees: cpp and the declarations ----------------------

def include_dirs(src):
    """The source directories CPython's own Makefile puts on the include
    path of the core, in its order: the -I$(srcdir)/... of PY_CFLAGS_NODIST
    (Include/internal from 3.8), then PY_CPPFLAGS' (Include)."""
    try:
        with open(os.path.join(src, 'Makefile.pre.in')) as f:
            text = f.read()
    except OSError:
        return [os.path.join(src, 'Include')]
    out = []
    for var in ('PY_CFLAGS_NODIST', 'PY_CPPFLAGS'):
        m = re.search(r'^%s=(.*)$' % var, text, re.M)
        for d in re.findall(r'-I\$\(srcdir\)/(\S+)', m.group(1) if m else ''):
            if os.path.join(src, d) not in out:
                out.append(os.path.join(src, d))
    return out or [os.path.join(src, 'Include')]


def cpp(src, pyconfig, cfile, defines=('Py_BUILD_CORE',)):
    """(macros, preprocessed tokens) of a CPython source file as it is
    compiled: every #define after the #if branches are resolved, from
    'gcc -E -dM', and the preprocessed text. The configuration is the
    target's installed pyconfig.h."""
    cmd = ['gcc', '-E'] + [w for d in include_dirs(src) for w in ('-I', d)] \
        + ['-I', os.path.dirname(pyconfig)] + ['-D' + d for d in defines]
    full = os.path.join(src, cfile)
    dm = subprocess.run(cmd + ['-dM', full], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, universal_newlines=True,
                        timeout=120)
    if dm.returncode != 0:
        raise ParseError('gcc -E -dM %s: %s' % (cfile, dm.stderr[:400]))
    pp = subprocess.run(cmd + ['-P', full], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, universal_newlines=True,
                        timeout=120)
    if pp.returncode != 0:
        raise ParseError('gcc -E %s: %s' % (cfile, pp.stderr[:400]))
    macros = ctokens.tokenize(dm.stdout, cfile + ' (macros)')[1]
    toks = ctokens.tokenize(pp.stdout, cfile + ' (preprocessed)')[0]
    return macros, toks


_DIRECTIVE = re.compile(r'\s*#\s*(\w+)')
_COND_OPEN = {'if', 'ifdef', 'ifndef'}
_COND_BRANCH = _COND_OPEN | {'elif', 'else'}


def strip_comments(text):
    """The text with each comment replaced by spaces, its newlines kept, so
    lines keep their numbers. String and character literals are kept; a
    quote with no partner on its line (an apostrophe in an '#if 0' block)
    is an ordinary character."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == '/' and text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c in '"\'':
            m = re.compile(r'%s(?:\\.|[^%s\\\n])*%s' % (c, c, c)).match(text, i)
            j = m.end() if m else i + 1
            out.append(text[i:j])
            i = j
        else:
            j = i + 1
            while j < n and text[j] not in '/"\'':
                j += 1
            out.append(text[i:j])
            i = j
    return ''.join(out)


def logical_lines(lines):
    """[(first, last)] line indexes of each logical line (backslash
    continuations joined)."""
    out, i = [], 0
    while i < len(lines):
        j = i
        while lines[j].endswith('\\') and j + 1 < len(lines):
            j += 1
        out.append((i, j))
        i = j + 1
    return out


def conditionals(src, pyconfig, cfile, defines=('Py_BUILD_CORE',), text=None):
    """The file's conditional compilation as the target's build does it
    (rule preprocess_conditionals): (active text, undefs). The active text
    is the file (or 'text', a spliced copy of it) with comments blanked,
    and every conditional directive and every line of a branch the
    compiler skips made empty, so lines keep their numbers, the file's own
    #defines are the ones in force, and no body has a conditional left.
    'undefs' is [(line, name, macro or None)] for each #undef the compiler
    obeys, with the definition in force just before it.

    One gcc run decides: a probe made of the file's directives only (text
    lines don't change what is defined), each branch followed by a marker
    line and each #undef preceded by one, preprocessed with -dD so the
    #defines and #undefs it obeys come out in order among the markers."""
    if text is None:
        with open(os.path.join(src, cfile), encoding='latin-1') as f:
            text = f.read()
    lines = strip_comments(text).split('\n')
    probe = []
    for lo, hi in logical_lines(lines):
        m = _DIRECTIVE.match(lines[lo])
        if not m:
            continue
        probe.extend(lines[lo:hi + 1])
        word = m.group(1)
        if word in _COND_BRANCH:
            probe.append('__ni_branch_%d' % lo)
        elif word == 'undef':
            probe.insert(len(probe) - (hi - lo + 1), '__ni_undef_%d' % lo)
    full = os.path.join(src, cfile)
    cmd = ['gcc', '-E', '-dD', '-P', '-I', os.path.dirname(full)] \
        + [w for d in include_dirs(src) for w in ('-I', d)] \
        + ['-I', os.path.dirname(pyconfig)] + ['-D' + d for d in defines]
    r = subprocess.run(cmd + ['-x', 'c', '-'], input='\n'.join(probe) + '\n',
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True, timeout=120)
    if r.returncode != 0:
        raise ParseError('gcc -E -dD %s (its directives): %s'
                         % (cfile, r.stderr[:400]))
    active, undefs, defs, pending = set(), [], {}, None
    for ln in r.stdout.split('\n'):
        w = ln.strip()
        if w.startswith('__ni_branch_'):
            active.add(int(w[len('__ni_branch_'):]))
        elif w.startswith('__ni_undef_'):
            pending = int(w[len('__ni_undef_'):])
        elif w.startswith('#define'):
            ms = ctokens.tokenize(w + '\n', cfile + ' (macros)')[1]
            defs.update(ms)
        elif w.startswith('#undef'):
            name = w.split()[1]
            if pending is not None:
                undefs.append((pending + 1, name, defs.get(name)))
                pending = None
            defs.pop(name, None)
    # every line inside a branch not taken, and every conditional
    # directive, is emptied
    out = list(lines)
    stack = []      # per open conditional: is the enclosing text live
    live = True
    for lo, hi in logical_lines(lines):
        m = _DIRECTIVE.match(lines[lo])
        word = m.group(1) if m else None
        if word in _COND_BRANCH or word == 'endif':
            if word in _COND_OPEN:
                stack.append(live)
            if not stack:
                raise ParseError('%s:%d: #%s with no #if' % (cfile, lo + 1, word))
            if word == 'endif':
                live = stack.pop()
            else:
                live = stack[-1] and lo in active
            for k in range(lo, hi + 1):
                out[k] = ''
        elif not live:
            for k in range(lo, hi + 1):
                out[k] = ''
    if stack:
        raise ParseError('%s: unterminated #if' % cfile)
    return '\n'.join(out), undefs


class Decls:
    """Top-level declarations of a preprocessed file: typedef names, the
    return type of each function-pointer typedef, struct fields with their
    types, function prototypes' return types, and variables."""

    def __init__(self, toks):
        self.types = set()
        self.fnptr = {}          # typedef name -> return type text
        self.structs = {}        # struct tag or typedef name -> {field: type}
        self.protos = {}         # function name -> return type text
        self.vars = set()
        self.aliases = {}        # typedef name -> the type it names, as text
        self.read(toks)

    def read(self, toks):
        i, start = 0, 0
        while i < len(toks):
            t = toks[i].text
            if t == '{':
                end = ctokens.match_close(toks, i)
                head = toks[start:i]
                words = [x.text for x in head]
                tail_end = i
                # a function's head or an initialised variable's may name
                # a struct too (dict_length's PyDictObject was a function's)
                if ('struct' in words or 'union' in words) and \
                        '(' not in words and '=' not in words:
                    semi = next(j for j in range(end, len(toks)) if toks[j].text == ';')
                    fields = self.fields(toks[i + 1:end])
                    tag = [x.text for x in head if x.kind == 'id'
                           and x.text not in ('typedef', 'struct', 'union')]
                    names = tag[-1:] + [x.text for x in toks[end + 1:semi]
                                        if x.kind == 'id']
                    for n in names:
                        self.structs[n] = fields
                    if words and words[0] == 'typedef':
                        self.types.update(x.text for x in toks[end + 1:semi]
                                          if x.kind == 'id')
                    i = semi + 1
                    start = i
                    continue
                if words and words[0] == 'enum' or (words[:2] == ['typedef', 'enum']):
                    semi = next(j for j in range(end, len(toks)) if toks[j].text == ';')
                    for part in ctokens.split_top(toks[i + 1:end], ','):
                        if part and part[0].kind == 'id':
                            self.vars.add(part[0].text)
                    if words[0] == 'typedef':
                        self.types.update(x.text for x in toks[end + 1:semi]
                                          if x.kind == 'id')
                    i = semi + 1
                    start = i
                    continue
                i = end + 1         # a function body
                if i < len(toks) and toks[i].text == ';':
                    i += 1
                start = i
                del tail_end
                continue
            if t == ';':
                self.one(toks[start:i])
                start = i + 1
            i += 1

    @staticmethod
    def strip_attrs(head):
        out, i = [], 0
        while i < len(head):
            if head[i].text in ('__attribute__', '__asm__', '__asm') and \
                    i + 1 < len(head) and head[i + 1].text == '(':
                i = ctokens.match_close(head, i + 1) + 1
                continue
            if head[i].text in ('__extension__', '__restrict', 'extern',
                                '__inline', '__inline__', 'static'):
                i += 1
                continue
            out.append(head[i])
            i += 1
        return out

    def one(self, head):
        head = self.strip_attrs(head)
        if not head:
            return
        words = [x.text for x in head]
        if words[0] == 'typedef':
            # function pointer: typedef RET (*NAME)(ARGS)
            for j in range(len(head) - 2):
                if words[j] == '(' and words[j + 1] == '*' and head[j + 2].kind == 'id':
                    self.types.add(words[j + 2])
                    self.fnptr[words[j + 2]] = ' '.join(words[1:j])
                    return
            depth = 0
            name = None
            for x in head:
                depth += (x.text in '([') - (x.text in ')]')
                if depth == 0 and x.kind == 'id':
                    name = x.text
            if name:
                self.types.add(name)
                # 'typedef struct _longobject PyLongObject;', 'typedef
                # int32_t sdigit;': what the name stands for
                if '[' not in words and '(' not in words and words[-1] == name:
                    self.aliases[name] = ' '.join(words[1:-1])
            return
        # prototype: RET NAME ( ... )
        if words[-1] == ')':
            depth = 0
            for j in range(len(head) - 1, -1, -1):
                if words[j] == ')':
                    depth += 1
                elif words[j] == '(':
                    depth -= 1
                    if depth == 0:
                        if j > 0 and head[j - 1].kind == 'id':
                            self.protos[words[j - 1]] = ' '.join(words[:j - 1])
                        return
            return
        # variable: the last id at depth 0
        depth, name = 0, None
        for x in head:
            depth += (x.text in '([') - (x.text in ')]')
            if depth == 0 and x.kind == 'id':
                name = x.text
        if name:
            self.vars.add(name)

    def fields(self, toks):
        out = {}
        for part in ctokens.split_top(toks, ';'):
            part = self.strip_attrs(part)
            words = [x.text for x in part]
            if not words:
                continue
            fp = None
            for j in range(len(part) - 2):
                if words[j] == '(' and words[j + 1] == '*' and part[j + 2].kind == 'id':
                    fp = j
                    break
            if fp is not None:
                out[words[fp + 2]] = ' '.join(words[:fp]) + ' (*)()'
                continue
            if '{' in words:
                continue
            # T a, *b;
            base = []
            k = 0
            while k < len(part) and part[k].text != '*' and not (
                    part[k].kind == 'id' and k + 1 < len(part)
                    and part[k + 1].text in (',', '[', ':') or k == len(part) - 1):
                base.append(words[k])
                k += 1
            for d in ctokens.split_top(part[k:], ','):
                stars = sum(1 for x in d if x.text == '*')
                ids = [x.text for x in d if x.kind == 'id']
                if ids:
                    out[ids[0]] = ' '.join(base) + (' ' + '*' * stars if stars else '')
        return out
