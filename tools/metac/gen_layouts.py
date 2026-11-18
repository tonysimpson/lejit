#!/usr/bin/env python3
"""Field descriptors and layouts from the target's headers (stage 0, D19, D76).

The fields Ni relies on are data: policy_table.LAYOUTS (struct fields) and
policy_table.RUNTIME (fields reached from a global variable). This reads
each named struct from the source (every Include/**/*.h, then the files an
entry names for a struct private to a C file), checks that every field
exists with the C type the table says, and writes:

- layouts_gen.h, for every unit: per field NI_OFS_<struct>__<field>
  (offsetof), NI_SIZE2_ and NI_UNSIGNED_ (how Ni reads it), with a
  _Static_assert for each assumption: the declared type, the size Ni
  reads, the kind of value (integer, pointer, floating) and signedness.
  A private struct is copied from its C file under Ni's name; a bit-field
  Ni tests gets NI_BIT_ masks.
- runtime_layouts_gen.h, for pyruntime.c only (it needs Py_BUILD_CORE):
  NI_RUNTIME_<name> for each RUNTIME path, with the same assertions.

So a renamed field or a changed type stops generation, naming the field;
a size or signedness that differs only in the compiled configuration stops
the build at the _Static_assert. The compiler does every offset.

    gen_layouts.py SRC [SRC...]     report, for each tree, what is missing
                                    or changed (nothing is written)
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctokens  # noqa: E402
import cslots  # noqa: E402
import policy_table  # noqa: E402

# Ni's reads: size in bytes, kind (__builtin_classify_type: 1 integer,
# 5 pointer, 8 floating) and signedness
READS = {'ptr': (8, 5, None), 'f64': (8, 8, None),
         'i8': (1, 1, True), 'u8': (1, 1, False),
         'i16': (2, 1, True), 'u16': (2, 1, False),
         'i32': (4, 1, True), 'u32': (4, 1, False),
         'i64': (8, 1, True), 'u64': (8, 1, False)}
# a suffix: '/low' Ni reads only the first bytes of a wider field (x86-64 is
# little-endian), so neither kind nor sign is assumed; '/bits' Ni reads the
# bytes as the read type says, whatever the declared kind and sign
HOWS = ('', 'low', 'bits')
SIZE2 = {1: 0, 2: 1, 4: 2, 8: 3}


class Field:
    """One declared member: its type text (normalised), array suffix,
    bit width, and for a nested struct or union its members."""

    def __init__(self, name, base, stars, array, bits, inner=None):
        self.name, self.base, self.stars = name, base, stars
        self.array, self.bits, self.inner = array, bits, inner

    def ctype(self):
        if self.inner is not None:
            return self.base            # 'struct {...}'
        return normalise_parts(self.base, self.stars, self.array)


def normalise_parts(base, stars, array):
    return (' '.join(base) + (' ' + '*' * stars if stars else '')
            + ''.join('[%s]' % a for a in array))


def normalise(ctype):
    """'PyObject  **' -> 'PyObject **'; 'digit [ 1 ]' -> 'digit[1]'."""
    toks = ctokens.tokenize(ctype + ' x;')[0][:-2]
    base = [t.text for t in toks if t.text not in ('*', '[', ']')
            and not _in_brackets(toks, t)]
    stars = sum(1 for t in toks if t.text == '*')
    return normalise_parts(base, stars, _arrays(toks))


def _in_brackets(toks, tok):
    depth = 0
    for t in toks:
        if t is tok:
            return depth > 0
        depth += (t.text == '[') - (t.text == ']')
    return False


def _arrays(toks):
    out, cur, depth = [], None, 0
    for t in toks:
        if t.text == '[':
            depth += 1
            cur = []
        elif t.text == ']':
            depth -= 1
            out.append(''.join(cur))
        elif depth:
            cur.append(t.text)
    return out


class Source:
    """The headers (every Include/**/*.h) and named C files of a CPython
    tree, with the macros the headers define, expanded inside struct bodies
    (PyObject_HEAD; a field list such as COMMON_FIELDS(func_), with ##)."""

    def __init__(self, src):
        self.src = src
        self.toks = {}
        self.headers = sorted(os.path.relpath(p, src) for p in glob.glob(
            os.path.join(src, 'Include', '**', '*.h'), recursive=True))
        self.macros = {}
        for path in self.headers:
            self.macros.update(self.tokens(path)[1])

    def tokens(self, path):
        if path not in self.toks:
            full = os.path.join(self.src, path)
            if not os.path.isfile(full):
                self.toks[path] = None
            else:
                with open(full, encoding='latin-1') as f:
                    self.toks[path] = ctokens.tokenize(f.read(), path)
        return self.toks[path]

    def expand(self, toks, depth=0):
        out, i = [], 0
        while i < len(toks):
            t = toks[i]
            m = self.macros.get(t.text) if t.kind == 'id' else None
            if m is None or depth >= 8:
                out.append(t)
                i += 1
            elif m.params is None:
                out += self.expand(m.body, depth + 1)
                i += 1
            elif i + 1 < len(toks) and toks[i + 1].text == '(':
                close = ctokens.match_close(toks, i + 1)
                args = ctokens.split_top(toks[i + 2:close], ',')
                out += self.expand(_substitute(m, args), depth + 1)
                i = close + 1
            else:
                out.append(t)
                i += 1
        return out

    def struct(self, name, files=()):
        """(path, body tokens) of the struct or union whose tag or typedef
        name is `name`, looking in the headers then in `files`; or None."""
        for path in self.headers + list(files):
            got = self.tokens(path)
            if got is None or not any(t.text == name for t in got[0]):
                continue
            toks = got[0]
            for i, t in enumerate(toks):
                if t.text != '{' or i < 1:
                    continue
                if toks[i - 1].text in ('struct', 'union'):
                    tag = None
                elif i >= 2 and toks[i - 2].text in ('struct', 'union'):
                    tag = toks[i - 1].text
                else:
                    continue
                try:
                    end = ctokens.match_close(toks, i)
                except SyntaxError:     # braces split by #if branches
                    continue
                if tag == name or (end + 2 < len(toks)
                                   and toks[i - 2 - (tag is not None)].text
                                   == 'typedef'
                                   and toks[end + 1].text == name):
                    return path, toks[i + 1:end]
            for i in range(len(toks) - 4):      # typedef struct TAG NAME;
                if (toks[i].text == 'typedef'
                        and toks[i + 1].text in ('struct', 'union')
                        and toks[i + 3].text == name
                        and toks[i + 4].text == ';'
                        and toks[i + 2].text != name):
                    return self.struct(toks[i + 2].text, files)
        return None

    def typedef(self, name, path):
        """Tokens of 'typedef ... name ...;' in path, or None."""
        toks = self.tokens(path)[0]
        for i, t in enumerate(toks):
            if t.text != 'typedef':
                continue
            end = next(j for j in range(i, len(toks)) if toks[j].text == ';')
            if any(x.text == name for x in toks[i:end]) and \
                    not any(x.text == '{' for x in toks[i:end]):
                return toks[i:end + 1]
        return None

    def variable(self, name):
        """(path, type name) of a global variable declared in the headers
        as PyAPI_DATA(TYPE) NAME; or extern TYPE NAME;, or None."""
        for path in self.headers:
            toks = self.tokens(path)[0]
            for i, t in enumerate(toks):
                if t.text != name or i + 1 >= len(toks) or \
                        toks[i + 1].text != ';':
                    continue
                if toks[i - 1].text == ')' and toks[i - 4].text == 'PyAPI_DATA':
                    return path, toks[i - 2].text
                if toks[i - 2].text == 'extern':
                    return path, toks[i - 1].text
        return None

    def fields(self, body):
        return parse_fields(_strip_annotations(self.expand(body)))

    def canonical(self, ctype):
        """A type's text with each typedef of a struct tag written as the
        tag ('PyTypeObject *' -> 'struct _typeobject *'), so that two
        spellings of one type compare equal."""
        if not hasattr(self, 'tags'):
            self.tags = {}
            for path in self.headers:
                toks = self.tokens(path)[0]
                for i in range(len(toks) - 4):
                    if (toks[i].text == 'typedef'
                            and toks[i + 1].text in ('struct', 'union')
                            and toks[i + 4].text == ';'):
                        self.tags[toks[i + 3].text] = '%s %s' % (
                            toks[i + 1].text, toks[i + 2].text)
        return ' '.join(self.tags.get(w, w) for w in ctype.split(' '))


# compiler annotations a header may put between members
ANNOTATIONS = {'__extension__', '__pragma'}


def _strip_annotations(toks):
    out, i = [], 0
    while i < len(toks):
        if toks[i].text in ANNOTATIONS:
            if toks[i].text != '__extension__' and i + 1 < len(toks) \
                    and toks[i + 1].text == '(':
                i = ctokens.match_close(toks, i + 1) + 1
            else:
                i += 1
            continue
        out.append(toks[i])
        i += 1
    return out


def _substitute(m, args):
    """A function-like macro's body with its arguments in, ## pasted."""
    params = dict(zip(m.params, args))
    out = []
    for t in m.body:
        if t.kind == 'id' and t.text in params:
            out += params[t.text]
        else:
            out.append(t)
    pasted = []
    k = 0
    while k < len(out):
        if out[k].text == '##' and pasted and k + 1 < len(out):
            a = pasted.pop()
            pasted.append(ctokens.Tok('id', a.text + out[k + 1].text,
                                      a.line, a.sp))
            k += 2
        else:
            pasted.append(out[k])
            k += 1
    return pasted


def parse_fields(toks):
    """{name: Field} of a struct body's members, nested aggregates kept; an
    anonymous struct or union's members are the parent's, as in C."""
    out = {}
    for decl in ctokens.split_top(toks, ';'):
        if not decl:
            continue
        agg = next((k for k, t in enumerate(decl)
                    if t.text in ('struct', 'union')), None)
        if agg == 0 and any(t.text == '{' for t in decl):
            o = next(k for k, t in enumerate(decl) if t.text == '{')
            c = ctokens.match_close(decl, o)
            inner = parse_fields(decl[o + 1:c])
            names = [d for d in ctokens.split_top(decl[c + 1:], ',')
                     if any(t.kind == 'id' for t in d)]
            if not names:
                out.update(inner)
            for d in names:
                ids = [t.text for t in d if t.kind == 'id']
                out[ids[0]] = Field(ids[0], decl[0].text + ' {...}', 0,
                                    _arrays(d), None, inner)
            continue
        if any(t.text == '(' for t in decl):        # pointer to function
            o = next(k for k, t in enumerate(decl) if t.text == '(')
            ids = [t.text for t in decl[o:] if t.kind == 'id']
            if ids:
                out[ids[0]] = Field(ids[0], ['(function pointer)'], 0, [],
                                    None)
            continue
        parts = ctokens.split_top(decl, ',')
        first = parts[0]
        k = _declarator_name(first)
        if k is None:
            continue
        s = k
        while s > 0 and first[s - 1].text == '*':
            s -= 1
        base = [t.text for t in first[:s]]
        for n, d in enumerate(parts):
            if n == 0:
                stars, rest, name = k - s, first[k + 1:], first[k].text
            else:
                j = _declarator_name(d)
                if j is None:
                    continue
                stars, rest, name = j, d[j + 1:], d[j].text
            bits = None
            if rest and rest[0].text == ':':
                bits = int(rest[1].text)
                rest = []
            out[name] = Field(name, base, stars, _arrays(rest), bits)
    return out


def _declarator_name(d):
    """Index of the declared name: the last id outside brackets and before
    a bit width."""
    depth, k = 0, None
    for j, t in enumerate(d):
        if t.text == '[':
            depth += 1
        elif t.text == ']':
            depth -= 1
        elif t.text == ':' and depth == 0:
            break
        elif depth == 0 and t.kind == 'id':
            k = j
    return k


def parse_read(read):
    kind, _, how = read.partition('/')
    if kind not in READS or how not in HOWS:
        raise cslots.GenError('policy_table: read %r is not one of %s'
                              ' with an optional /low or /bits'
                              % (read, ' '.join(READS)))
    return kind, how


def _walk(s, fields, path, files):
    """The Field at a dotted path, entering nested aggregates and member
    structs by their type; or a reason it isn't there."""
    f = None
    for n, part in enumerate(path.split('.')):
        if fields is None or part not in fields:
            return None, 'no field %s' % '.'.join(path.split('.')[:n + 1])
        f = fields[part]
        if n + 1 < len(path.split('.')):
            if f.inner is not None:
                fields = f.inner
            else:
                found = s.struct(f.base[-1], files) if f.base else None
                fields = s.fields(found[1]) if found else None
    return f, None


def check(s, entries=None, runtime=None):
    """(layouts, runtime, problems): each layout (struct name, entry, path
    found, {field path: Field}) and runtime (name, entry, path, Field), and
    every field missing or declared differently, by name."""
    entries = policy_table.LAYOUTS if entries is None else entries
    runtime = policy_table.RUNTIME if runtime is None else runtime
    problems, layouts, rt = [], [], []
    for name, e in entries.items():
        files = e.get('from', ())
        found = s.struct(name, files)
        if found is None:
            problems.append('%s: no struct or union %s in Include/ or %s'
                            % (name, name, ' '.join(files) or '-'))
            continue
        path, body = found
        fields = s.fields(body)
        got = {}
        for item in e['fields']:
            fpath, ctype, read = item[:3]
            parse_read(read)
            f, why = _walk(s, fields, fpath, files)
            if f is None:
                problems.append('%s.%s (%s): %s' % (name, fpath, path, why))
                continue
            if s.canonical(f.ctype()) != s.canonical(normalise(ctype)) and not (
                    f.inner is not None and ctype == f.base):
                problems.append('%s.%s (%s): declared %r, Ni assumes %r'
                                % (name, fpath, path, f.ctype(), ctype))
                continue
            got[fpath] = f
        for fpath, want in e.get('bits', {}).items():
            f = got.get(fpath)
            if f is None or f.inner is None:
                continue
            have = ['%s:%s' % (n, x.bits) for n, x in f.inner.items()]
            if have[:len(want)] != list(want):
                problems.append('%s.%s (%s): bit-fields %s, Ni assumes %s'
                                % (name, fpath, path, ' '.join(have),
                                   ' '.join(want)))
        layouts.append((name, e, path, got))
    for name, (var, fpath, ctype, read) in runtime.items():
        parse_read(read)
        v = s.variable(var)
        if v is None:
            problems.append('RUNTIME %s: no variable %s declared in Include/'
                            % (name, var))
            continue
        found = s.struct(v[1])
        if found is None:
            problems.append('RUNTIME %s: no struct %s (the type of %s)'
                            % (name, v[1], var))
            continue
        f, why = _walk(s, s.fields(found[1]), fpath, ())
        if f is None:
            problems.append('RUNTIME %s: %s.%s (%s): %s'
                            % (name, var, fpath, found[0], why))
            continue
        if s.canonical(f.ctype()) != s.canonical(normalise(ctype)):
            problems.append('RUNTIME %s: %s.%s (%s): declared %r, Ni assumes'
                            ' %r' % (name, var, fpath, found[0], f.ctype(),
                                     ctype))
            continue
        rt.append((name, (var, fpath, ctype, read), found[0], f))
    return layouts, rt, problems


def _asserts(expr, what, ctype, read, f, typed=True):
    """_Static_assert lines for one field reached by the C expression."""
    kind, how = parse_read(read)
    size, klass, signed = READS[kind]
    elem = expr + '[0]' * len(f.array)
    msg = 'Ni: %s' % what
    out = []
    if typed:
        out.append('_Static_assert(__builtin_types_compatible_p('
                   '__typeof__(%s), %s), "%s is not %s");'
                   % (expr, normalise(ctype), msg, ctype))
    op = '>=' if how == 'low' else '=='
    out.append('_Static_assert(sizeof(%s) %s %d, "%s: Ni reads %d byte%s");'
               % (elem, op, size, msg, size, 's' if size > 1 else ''))
    if not how:
        out.append('_Static_assert(__builtin_classify_type(%s) == %d,'
                   ' "%s: Ni reads it as %s");' % (elem, klass, msg,
                                                  {1: 'an integer',
                                                   5: 'a pointer',
                                                   8: 'floating'}[klass]))
        if signed is not None:
            out.append('_Static_assert(((__typeof__(%s))-1 < 0) == %d,'
                       ' "%s: Ni reads it as %s");'
                       % (elem, signed, msg,
                          'signed' if signed else 'unsigned'))
    return out


def macro_name(cname, fpath):
    return '%s__%s' % (cname, fpath.replace('.', '__'))


def header(s, layouts):
    lines = ['/* Generated by tools/metac/gen_layouts.py from CPython %s'
             % s.version, '   (policy_table.LAYOUTS; D76). Do not edit. */',
             '#ifndef NI_LAYOUTS_GEN_H', '#define NI_LAYOUTS_GEN_H',
             '#include <stddef.h>', '']
    for name, e, path, got in layouts:
        cname = e.get('copy', name)
        lines.append('/* %s (%s) */' % (name, path))
        if 'copy' in e:
            lines += copy_struct(s, name, e, path)
        for item in e['fields']:
            fpath, ctype, read = item[:3]
            f = got[fpath]
            kind, how = parse_read(read)
            size, _, signed = READS[kind]
            m = macro_name(cname, fpath)
            lines.append('#define NI_OFS_%s offsetof(%s, %s)'
                         % (m, cname, fpath))
            lines.append('#define NI_SIZE2_%s %d' % (m, SIZE2[size]))
            lines.append('#define NI_UNSIGNED_%s %d'
                         % (m, kind.startswith('u')))
            lines += _asserts('((%s *)0)->%s' % (cname, fpath),
                              '%s.%s' % (name, fpath), ctype, read, f,
                              typed=f.inner is None)
        for fpath, want in e.get('bits', {}).items():
            pos = 0
            for b in want:
                bname, width = b.split(':')
                lines.append('#define NI_BIT_%s__%s 0x%X'
                             % (macro_name(cname, fpath), bname,
                                ((1 << int(width)) - 1) << pos))
                pos += int(width)
            if pos > 8:
                raise cslots.GenError('%s.%s: bit-fields past the first byte'
                                      % (name, fpath))
        lines.append('')
    lines += ['#endif /* NI_LAYOUTS_GEN_H */', '']
    return '\n'.join(lines)


def copy_struct(s, name, e, path):
    """A struct private to a C file, copied under Ni's name, with the
    typedefs of that file its members use."""
    found = s.struct(name, e.get('from', ()))
    body = found[1]
    out = []
    public = set()
    for p in s.headers:
        public |= {t.text for t in s.tokens(p)[0]}
    for t in body:
        if t.kind == 'id' and t.text not in public and t.text not in (
                s.macros):
            td = s.typedef(t.text, path)
            if td is not None and t.text not in [x for x in out]:
                out.append(cslots.text(td))
    text = cslots.text(body)
    return out + ['typedef struct {%s} %s;' % (text, e['copy'])]


def runtime_header(s, rt):
    lines = ['/* Generated by tools/metac/gen_layouts.py from CPython %s'
             % s.version, '   (policy_table.RUNTIME; D76). For pyruntime.c'
             ' only: it needs Py_BUILD_CORE. Do not edit. */',
             '#ifndef NI_RUNTIME_LAYOUTS_GEN_H',
             '#define NI_RUNTIME_LAYOUTS_GEN_H', '']
    for name, (var, fpath, ctype, read), path, f in rt:
        expr = '%s.%s' % (var, fpath)
        lines.append('/* %s (%s) */' % (expr, path))
        lines.append('#define NI_RUNTIME_%s (%s)' % (name, expr))
        lines += _asserts(expr, expr, ctype, read, f)
        lines.append('')
    lines += ['#endif /* NI_RUNTIME_LAYOUTS_GEN_H */', '']
    return '\n'.join(lines)


def _source(target):
    s = Source(target.src)
    s.version = target.version
    return s


def build(target):
    s = _source(target)
    layouts, rt, problems = check(s)
    if problems:
        raise cslots.GenError('fields Ni relies on are missing or changed'
                              ' (policy_table.LAYOUTS, RUNTIME):\n    '
                              + '\n    '.join(problems))
    report = ['generated %s.%s' % (name, item[0])
              for name, e, path, got in layouts for item in e['fields']]
    return header(s, layouts), report


def build_runtime(target):
    s = _source(target)
    layouts, rt, problems = check(s)
    if problems:
        raise cslots.GenError('fields Ni relies on are missing or changed'
                              ' (policy_table.LAYOUTS, RUNTIME):\n    '
                              + '\n    '.join(problems))
    report = ['generated %s.%s' % (e[0], e[1]) for _, e, _, _ in rt]
    return runtime_header(s, rt), report


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    status = 0
    for src in argv:
        s = Source(src)
        layouts, rt, problems = check(s)
        n = sum(len(e['fields']) for e in policy_table.LAYOUTS.values()) + \
            len(policy_table.RUNTIME)
        ok = sum(len(got) for _, _, _, got in layouts) + len(rt)
        print('%s: %d of %d fields as Ni assumes' % (src, ok, n))
        for p in problems:
            print('  ' + p)
        status |= bool(problems)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
