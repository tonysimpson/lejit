"""Parser combinators rebuilt for every document from closures over constant strings and character sets (thousands per run), parsing a config language with heavy backtracking, where each variant builds a different grammar: per-closure value specialisation multiplying compiled code, and branchy recursion (Chambers and Ungar, Customization, PLDI 1989; Gal et al., Trace-based Just-in-Time Type Specialization, PLDI 2009)."""
import zlib

N = 100


class Fail(Exception):
    pass


def lit(s):
    n = len(s)

    def p(text, i):
        if text.startswith(s, i):
            return s, i + n
        raise Fail()
    return p


def charset(chars):
    allowed = frozenset(chars)

    def p(text, i):
        if i < len(text) and text[i] in allowed:
            return text[i], i + 1
        raise Fail()
    return p


def seq(*ps):
    def p(text, i):
        out = []
        for q in ps:
            v, i = q(text, i)
            out.append(v)
        return out, i
    return p


def alt(*ps):
    def p(text, i):
        for q in ps:
            try:
                return q(text, i)
            except Fail:
                pass
        raise Fail()
    return p


def many(q, least=0):
    def p(text, i):
        out = []
        while True:
            try:
                v, i = q(text, i)
            except Fail:
                break
            out.append(v)
        if len(out) < least:
            raise Fail()
        return out, i
    return p


def fmap(q, f):
    def p(text, i):
        v, i = q(text, i)
        return f(v), i
    return p


def opt(q, default=None):
    def p(text, i):
        try:
            return q(text, i)
        except Fail:
            return default, i
    return p


def lazy(thunk):
    cache = []

    def p(text, i):
        if not cache:
            cache.append(thunk())
        return cache[0](text, i)
    return p


def build_grammar(keywords, seps):
    ws = many(charset(' \t\n'))

    def tok(q):
        return fmap(seq(q, ws), lambda v: v[0])

    digit = charset('0123456789')
    letter = charset('abcdefghijklmnopqrstuvwxyz_')
    number = tok(alt(
        fmap(seq(many(digit, 1), lit('.'), many(digit, 1)),
             lambda v: float(''.join(v[0]) + '.' + ''.join(v[2]))),
        fmap(many(digit, 1), lambda v: int(''.join(v)))))
    ident = tok(fmap(seq(letter, many(alt(letter, digit))), lambda v: v[0] + ''.join(v[1])))
    string = tok(fmap(seq(lit('"'), many(charset('abcdefghijklmnopqrstuvwxyz0123456789 _-')),
                          lit('"')), lambda v: ''.join(v[1])))
    kw = [tok(fmap(lit(k), lambda v, k=k: ('kw', k))) for k in sorted(keywords, key=len, reverse=True)]
    value = lazy(lambda: alt(*(kw + [number, string, array, table, ident])))
    array = fmap(seq(tok(lit('[')), opt(seq(value, many(fmap(seq(tok(lit(seps[0])), value),
                                                             lambda v: v[1])))), tok(lit(']'))),
                 lambda v: [] if v[1] is None else [v[1][0]] + v[1][1])
    pair = fmap(seq(ident, tok(alt(*(lit(s) for s in sorted(seps[1:], key=len, reverse=True)))), value), lambda v: (v[0], v[2]))
    table = fmap(seq(tok(lit('{')), many(fmap(seq(pair, opt(tok(lit(seps[0])))), lambda v: v[0])),
                     tok(lit('}'))), lambda v: dict(v[1]))
    section = fmap(seq(tok(lit('section')), ident, table), lambda v: (v[1], v[2]))
    return fmap(seq(ws, many(section)), lambda v: v[1])


def make_doc(k, keywords, seps, variant):
    sections = []
    for s in range(6 + k % 5):
        items = []
        for j in range(8):
            v = (s * 31 + j * 7 + k) % 23
            if v % 5 == 0:
                val = keywords[v % len(keywords)]
            elif v % 5 == 1:
                val = '%d.%d' % (v, j)
            elif v % 5 == 2:
                val = '"text %d"' % v
            elif v % 5 == 3:
                val = '[' + (seps[0] + ' ').join(str(x) for x in range(v % 4)) + ']'
            else:
                val = '{ inner%d %s %d }' % (j, seps[1 + j % (len(seps) - 1)], v)
            items.append('key%d %s %s' % (j, seps[1 + (j + s) % (len(seps) - 1)], val))
        sep = seps[0] + '\n  ' if variant == 0 else seps[0] + ' '
        sections.append('section sec%d_%d {\n  %s\n}' % (s, k, sep.join(items)))
    return '\n'.join(sections)


GRAMMARS = [
    (['true', 'false', 'null'], [',', '=']),
    (['yes', 'no', 'on', 'off', 'none', 'nan', 'nil', 'never', 'nowhere'], [';', ':=', ':', '=', '=>']),
]


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    keywords, seps = GRAMMARS[variant]
    out = []
    for k in range(n * 3):
        grammar = build_grammar(keywords, seps)
        doc = make_doc(k, keywords, seps, variant)
        tree, end = grammar(doc, 0)
        out.append((end == len(doc), tree))
        if k % 3 == 2:
            bad = doc[:len(doc) * 2 // 3] + ' }} @'
            tree, end = grammar(bad, 0)
            out.append(('partial', end, len(tree)))
    return digest(out, 3)
