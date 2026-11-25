"""C tokens, for features.py's questions on CPython's source.

Comments are skipped, string and character literals are single tokens, and
preprocessor lines are taken out of the stream: #define becomes a Macro, the
rest is dropped. No preprocessing is done, so a macro's uses stay visible
as calls, and both branches of an #if are kept. ` and $ are tokens too, for
the JavaScript an EM_JS macro holds.
"""
import re
from collections import namedtuple

# kind: id num str op; sp: whitespace before it (tells '#define F(x)' from
# '#define F (x)')
Tok = namedtuple('Tok', 'kind text line sp')
Macro = namedtuple('Macro', 'name params body line')   # params None: no ()

_PUNCT = sorted('''... <<= >>= -> ++ -- << >> <= >= == != && || += -= *= /=
%= &= ^= |= ## { } [ ] ( ) ; , : ? . + - * / % & | ^ ! ~ < > = #
` $'''.split(),
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
