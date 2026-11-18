"""C tokens for the meta-compiler (doc/METACOMPILER.md, stage 1).

Comments are skipped, string and character literals are single tokens, and
preprocessor lines are taken out of the stream: #define becomes a Macro, the
rest is dropped. No preprocessing is done, so a macro's uses stay visible
as calls, which is what the rewrite rules match on.
"""
import re
from collections import namedtuple

# kind: id num str op; sp: whitespace before it (tells '#define F(x)' from
# '#define F (x)')
Tok = namedtuple('Tok', 'kind text line sp')
Macro = namedtuple('Macro', 'name params body line')   # params None: no ()

_PUNCT = sorted('''... <<= >>= -> ++ -- << >> <= >= == != && || += -= *= /=
%= &= ^= |= ## { } [ ] ( ) ; , : ? . + - * / % & | ^ ! ~ < > = #'''.split(),
                key=len, reverse=True)
_TOKEN = re.compile(r'''
    (?P<ws>[ \t\r\f\v]+|\\\n)
  | (?P<nl>\n)
  | (?P<comment>/\*.*?\*/|//[^\n]*)
  | (?P<str>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
  | (?P<num>\.?[0-9](?:[eEpP][+-]|[0-9A-Za-z_.])*)
  | (?P<id>[A-Za-z_][A-Za-z_0-9]*)
  | (?P<op>''' + '|'.join(re.escape(p) for p in _PUNCT) + r''')
''', re.S | re.X)


def tokenize(src, filename='<c>', directives=False):
    """Return (tokens, macros) for C source text. With directives, every
    preprocessor line except #define is also left in the stream as one
    token of kind 'pp' (its text, '#if X'), so a reader can refuse code
    that the preprocessor chooses between."""
    toks, macros = [], {}
    pos, line, at_bol, pp = 0, 1, True, None   # pp: tokens of a # line
    sp = True
    while pos < len(src):
        m = _TOKEN.match(src, pos)
        if m is None:
            raise SyntaxError('%s:%d: cannot tokenise %r'
                              % (filename, line, src[pos:pos + 20]))
        kind, text = m.lastgroup, m.group()
        pos = m.end()
        nls = text.count('\n')
        if kind == 'nl':
            if pp is not None:
                _directive(pp, macros)
                if directives and pp and pp[0].text != 'define':
                    toks.append(Tok('pp', '#' + ' '.join(t.text for t in pp),
                                    pp[0].line, True))
                pp = None
            at_bol = sp = True
        elif kind in ('ws', 'comment'):
            sp = True
        elif pp is not None:
            pp.append(Tok(kind, text, line, sp))
        elif at_bol and text == '#':
            pp = []
        else:
            toks.append(Tok(kind, text, line, sp))
            at_bol = False
        if kind not in ('ws', 'comment', 'nl'):
            sp = False
        line += nls
    if pp is not None:
        _directive(pp, macros)
    return toks, macros


def _directive(pp, macros):
    if not pp or pp[0].text != 'define' or len(pp) < 2:
        return
    name = pp[1]
    rest = pp[2:]
    params = None
    if rest and rest[0].text == '(' and not rest[0].sp:   # function-like
        close = next(i for i, t in enumerate(rest) if t.text == ')')
        params = [t.text for t in rest[1:close] if t.text != ',']
        rest = rest[close + 1:]
    macros[name.text] = Macro(name.text, params, rest, name.line)


def match_close(toks, i):
    """Index of the bracket closing toks[i]."""
    pairs = {'(': ')', '[': ']', '{': '}'}
    open_, close = toks[i].text, pairs[toks[i].text]
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].text == open_:
            depth += 1
        elif toks[j].text == close:
            depth -= 1
            if depth == 0:
                return j
    raise SyntaxError('line %d: unbalanced %r' % (toks[i].line, open_))


def split_top(toks, sep):
    """Split toks at top-level (unbracketed) separators."""
    out, cur, depth = [], [], 0
    for t in toks:
        if t.text in '([{':
            depth += 1
        elif t.text in ')]}':
            depth -= 1
        if depth == 0 and t.text == sep:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    return out


def functions(toks):
    """{name: (params tokens, body tokens)} for top-level function
    definitions: NAME ( ... ) { ... } at brace depth 0."""
    out = {}
    i = 0
    while i < len(toks):
        t = toks[i]
        if t.text == '{':
            i = match_close(toks, i) + 1
            continue
        if (t.kind == 'id' and i + 1 < len(toks) and toks[i + 1].text == '('):
            close = match_close(toks, i + 1)
            if close + 1 < len(toks) and toks[close + 1].text == '{':
                end = match_close(toks, close + 1)
                out[t.text] = (toks[i + 2:close], toks[close + 2:end])
                i = end + 1
                continue
            i = close + 1
            continue
        i += 1
    return out


def struct_fields(toks, typedef_name):
    """Field names, in order, of 'typedef struct {...} typedef_name;'."""
    for i, t in enumerate(toks):
        if t.text == '}' and i + 1 < len(toks) and toks[i + 1].text == typedef_name:
            depth, j = 0, i
            while True:
                if toks[j].text == '}':
                    depth += 1
                elif toks[j].text == '{':
                    depth -= 1
                    if depth == 0:
                        break
                j -= 1
            decls = split_top(toks[j + 1:i], ';')
            return [[x.text for x in d if x.kind == 'id'][-1] for d in decls if d]
    raise LookupError('no struct ' + typedef_name)


def initializer(toks, var_name):
    """Entries of 'TYPE var_name = { ... };', casts removed: each is a name,
    '0', or the joined tokens of anything else."""
    for i, t in enumerate(toks):
        if (t.text == var_name and toks[i + 1].text == '='
                and toks[i + 2].text == '{'):
            end = match_close(toks, i + 2)
            out = []
            for e in split_top(toks[i + 3:end], ','):
                while len(e) > 1 and e[0].text == '(':
                    e = e[match_close(e, 0) + 1:]      # drop a cast
                out.append(' '.join(x.text for x in e))
            return out
    raise LookupError('no initializer ' + var_name)
