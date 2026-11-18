"""Lexer generated as Python source with one huge function per keyword table (hundreds of if/elif branches and inlined character tests), regenerated for each dialect and run on modest input: huge methods whose compilation costs more than they ever run (HotSpot DontCompileHugeMethods and its 8000-bytecode limit; LuaJIT trace aborts on trace too long)."""
N = 80

BASE_WORDS = ['select', 'from', 'where', 'group', 'order', 'by', 'having', 'limit', 'offset',
              'join', 'inner', 'outer', 'left', 'right', 'on', 'as', 'and', 'or', 'not', 'null',
              'insert', 'into', 'values', 'update', 'set', 'delete', 'create', 'table', 'index',
              'drop', 'alter', 'add', 'column', 'primary', 'key', 'foreign', 'references',
              'distinct', 'union', 'all', 'case', 'when', 'then', 'else', 'end', 'exists', 'in',
              'between', 'like', 'is', 'asc', 'desc', 'with', 'recursive', 'over', 'partition']


def dialect_words(d):
    words = list(BASE_WORDS)
    for k in range(40 * (d + 1)):
        words.append('%s_%s%d' % (BASE_WORDS[k % len(BASE_WORDS)], 'xyz'[d % 3], k))
    return words


def generate(words, d):
    lines = ['def lex(src):', '    out = []', '    i = 0', '    n = len(src)', '    while i < n:',
             '        c = src[i]',
             '        if c == " " or c == "\\n" or c == "\\t":', '            i += 1',
             '            continue',
             '        if "a" <= c <= "z" or c == "_":', '            j = i + 1',
             '            while j < n and ("a" <= src[j] <= "z" or "0" <= src[j] <= "9" or src[j] == "_"):',
             '                j += 1', '            w = src[i:j]']
    first = True
    for idx, w in enumerate(words):
        kw = 'if' if first else 'elif'
        first = False
        lines.append('            %s len(w) == %d and w == %r:' % (kw, len(w), w))
        lines.append('                out.append((%d, %r))' % (idx, w.upper()))
    lines += ['            else:', '                out.append((-1, w))', '            i = j',
              '            continue',
              '        if "0" <= c <= "9":', '            j = i + 1',
              '            while j < n and "0" <= src[j] <= "9":', '                j += 1',
              '            out.append((-2, int(src[i:j]) * %d))' % (d + 1), '            i = j',
              '            continue']
    for op in ['<=', '>=', '<>', '!=', '||', '::']:
        lines.append('        if src.startswith(%r, i):' % op)
        lines.append('            out.append((-3, %r))' % op)
        lines.append('            i += 2')
        lines.append('            continue')
    lines += ['        out.append((-4, c))', '        i += 1', '    return out']
    ns = {}
    exec(compile('\n'.join(lines), '<lexer %d>' % d, 'exec'), ns)
    return ns['lex']


def make_query(k, words):
    parts = []
    for j in range(60):
        w = words[(k * 17 + j * 29) % len(words)]
        parts.append(w)
        if j % 5 == 0:
            parts.append(str(k * j))
        if j % 7 == 3:
            parts.append(['<=', '<>', '(', ')', ',', '::', '*'][j % 7])
        if j % 11 == 0:
            parts.append('ident%d' % j)
    return ' '.join(parts)


def main(n, variant=0):
    out = []
    for k in range(n * 4):
        d = (k % 3) + 3 * variant
        words = dialect_words(d)
        lex = generate(words, d)
        toks = lex(make_query(k, words))
        kw = sum(1 for t in toks if t[0] >= 0)
        out.append((d, len(words), len(toks), kw, toks[:8], toks[-4:]))
    return out
