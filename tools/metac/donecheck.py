#!/usr/bin/env python3
"""GOAL's done test, point 1, as a check.

Lists every definition in Ni that mirrors a CPython one and puts each in one
class:
  (a) generated   the functions generate.py's report.txt gives as generated,
                  resumed or restarted, and the lines of the headers
  (b) reasoned    hand-written, with a reason that is either
                  final         an item of hand_table.REASONS of a kind in
                                FINAL (core, mechanism, judgement)
                  transitional  any other hand_table kind (limit, helper),
                                the generator's refusal of its CPython
                                counterpart, or the policy items of the
                                policy table entry that names it: open work
                                for the phase, never enough for the done test
  (c) unreasoned  hand-written with no reason: the done test fails

A hand-written definition mirrors CPython when the code says so, by any of:
  table    policy_table.ENTRIES names it ('ni'), keyed by the CPython one
  name     its name is a CPython function's in the source once Ni's
           prefixes are taken off (Psyco -> Py, psyco_, cimpl_, p,
           pbuiltin_ -> builtin_), or a Ni macro PsycoX for CPython's PyX
  cimpl    it is a run-time copy (cimpl_*)
  meta     Psyco_DefineMeta (or Psyco_DefineModuleFn) registers it
  opcode   a 'case X:' of pycompiler.c where X is one of ceval.c's TARGETs
A name match that mirrors nothing is in hand_table.REASONS as 'not_mirror';
the files hand_table.EXCLUDED names are not read.

  donecheck.py --gen DIR [--src DIR] [-v] [--list a|b|t|c|x]

DIR is where generate.py wrote its headers and report.txt; a report from
another generator commit or another source is refused as stale. The source
is --src, else as generate.py finds it for the report's version.

Exit 0 when the done test passes (no (c), no transitional reasons), 3 when
only transitional reasons remain, 1 when (c) is not empty, 2 when the report
is stale or hand_table.py names something that isn't in Ni or a reference
that isn't in the docs.
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(HERE))

import csource  # noqa: E402
import ctokens  # noqa: E402

# Ni's prefixes for its copies of CPython names, tried in this order
PREFIXES = [('_Psyco', '_Py'), ('Psyco', 'Py'), ('psyco_', ''), ('cimpl_', ''),
            ('pbuiltin_', 'builtin_'), ('p', '')]
# where CPython's functions are looked for (Modules: only what Ni has a
# meta for, so that a word like array_new doesn't match arraymodule.c)
CPY_DIRS = ['Objects', 'Objects/clinic', 'Python', 'Python/clinic']
CPY_FILES = ['Modules/mathmodule.c', 'Modules/clinic/mathmodule.c.h']
KINDS_SHOWN = ('function', 'macro', 'case', 'var')
# hand_table kinds that are reasons for good; not_mirror is class (x)
FINAL = {'core', 'mechanism', 'judgement'}

Unit = collections.namedtuple('Unit', 'name path lines cpython signals span')


def def_span(d):
    ls = [t.line for t in d.toks if t.line]
    return (min(ls), max(ls)) if ls else (0, 0)


def def_lines(d):
    lo, hi = def_span(d)
    return hi - lo + 1


# ---- (a): what the generators wrote ------------------------------------------

def read_report(gen):
    """(version, source sha, generator commit, [(key, status, reason,
    location)]), without the features' lines (features.py)."""
    with open(os.path.join(gen, 'report.txt')) as f:
        head, *lines = f.read().split('\n')
    version, sha, commit = head.split()[1:]
    return version, sha, commit, [tuple(ln.split('\t')) for ln in lines
                                  if ln and not ln.startswith('NI_HAS_')]


def stale(sha, commit, src):
    """Why the report doesn't describe this source and these generators,
    or None."""
    import cpython_src
    import generate
    if sha != cpython_src.source_sha(src):
        return 'its source has sha256 %s, not %s' % (sha, cpython_src.source_sha(src))
    if commit != generate.generator_commit():
        return 'it was generated at %s, not %s' % (commit, generate.generator_commit())
    return None


def header_units(gen):
    """[Unit] for each definition of the generated headers that hold
    functions, with its lines."""
    import generate
    units = []
    for h in sorted(os.listdir(gen)):
        if not h.endswith('_gen.h') or h in generate.NOT_FUNCTIONS - {
                'pycompiler_copies_gen.h'} or h.startswith('mechanism_'):
            continue
        with open(os.path.join(gen, h)) as f:
            toks, macros = ctokens.tokenize(f.read(), h)
        defs = csource.scan(toks, macros, h)
        defs.update(csource._cases(toks, h))
        units += [Unit(n, h, def_lines(d), None, (), None) for n, d in defs.items()
                  if d.kind in ('function', 'case')]
    return units


# ---- (b) and (c): what is hand-written ---------------------------------------

def cpython_index(src):
    """{function name: 'path:name'} and {macro name: 'path:name'} of the
    source (first definition wins), and the set of ceval.c's opcodes."""
    tree = csource.Tree(src)
    funcs, macros = {}, {}
    paths = []
    for d in CPY_DIRS:
        full = os.path.join(src, d)
        paths += ['%s/%s' % (d, f) for f in sorted(os.listdir(full))
                  if f.endswith(('.c', '.h'))]
    paths += [p for p in CPY_FILES if os.path.isfile(os.path.join(src, p))]
    for p in paths:
        f = tree.file(p)
        tree.check_balanced(p, 'its functions')
        for n, d in (f[1].items() if f else ()):
            if d.kind == 'function':
                funcs.setdefault(n, '%s:%s' % (p, n))
    for h in tree.headers():
        f = tree.file(h)
        for n, d in (f[1].items() if f else ()):
            if d.kind == 'macro':
                macros.setdefault(n, '%s:%s' % (h, n))
    ceval = tree.file('Python/ceval.c')[1]
    opcodes = {n[7:-1] for n in ceval if n.startswith('TARGET(')}
    return funcs, macros, opcodes


def ni_files(excluded):
    out = []
    for d, _, fs in os.walk(os.path.join(ROOT, 'ni')):
        for f in sorted(fs):
            if f.endswith(('.c', '.h')):
                p = os.path.relpath(os.path.join(d, f), ROOT)
                if p not in excluded:
                    out.append(p)
    return sorted(out)


def registered(toks):
    """Names Psyco_DefineMeta / Psyco_DefineModuleFn register as metas."""
    out = set()
    for i, t in enumerate(toks):
        if t.text in ('Psyco_DefineMeta', 'Psyco_DefineModuleFn') and \
                i + 1 < len(toks) and toks[i + 1].text == '(':
            end = ctokens.match_close(toks, i + 1)
            last = [x for x in toks[i + 2:end] if x.kind == 'id']
            if last and toks[end - 1].kind == 'id':
                out.add(last[-1].text)
    return out


def counterpart(name, kind, funcs, macros):
    table = funcs if kind == 'function' else macros
    for a, b in PREFIXES:
        if name.startswith(a) and (b + name[len(a):]) in table:
            if kind == 'macro' and a not in ('Psyco', '_Psyco'):
                continue
            return table[b + name[len(a):]]
    if kind == 'function' and name in funcs:
        return funcs[name]
    return None


def hand_units(src, table):
    import policy_table
    funcs, macros, opcodes = cpython_index(src)
    by_ni = collections.defaultdict(list)       # Ni name -> ENTRIES keys
    for key, e in policy_table.ENTRIES.items():
        for n in e.get('ni', ()):
            by_ni[n].append(key)
    tree = csource.Tree(ROOT)
    units = []
    for path in ni_files(table.EXCLUDED):
        toks, defs = tree.file(path)
        metas = registered(toks)
        for name, d in sorted(defs.items(), key=lambda kv: def_span(kv[1])):
            if d.kind not in KINDS_SHOWN:
                continue
            key = '%s:%s' % (path, name)
            signals, cpy = [], None
            if by_ni.get(key):
                signals.append('table')
                cpy = by_ni[key][0]
            if d.kind == 'case':
                if d.name in opcodes and path == 'ni/Python/pycompiler.c':
                    signals.append('opcode')
                    cpy = cpy or 'Python/ceval.c:TARGET(%s)' % d.name
            elif d.kind in ('function', 'macro'):
                c = counterpart(name, d.kind, funcs, macros)
                if c:
                    signals.append('name')
                    cpy = cpy or c
                if name.startswith('cimpl_') and d.kind == 'function':
                    signals.append('cimpl')
                if name in metas:
                    signals.append('meta')
            if signals:
                units.append(Unit(key, path, def_lines(d), cpy, tuple(signals),
                                  def_span(d)))
    return nested(units), by_ni


def nested(units):
    """A unit inside another (pycompiler.c's opcode cases inside its loop)
    counts its own lines only once: the outer one loses them."""
    out = []
    for u in units:
        inner = sum(v.lines for v in units if v is not u and v.path == u.path
                    and u.span[0] <= v.span[0] and v.span[1] <= u.span[1]
                    and v.span != u.span)
        out.append(u._replace(lines=u.lines - inner))
    return out


def classify(units, by_ni, refused, table):
    """{class: [(unit, reason)]}; classes 'b' (final), 't' (transitional),
    'c' and 'x' (not a mirror)."""
    import policy_table
    out = collections.defaultdict(list)
    for u in units:
        r = table.REASONS.get(u.name)
        if r is not None:
            out['x' if r[0] == 'not_mirror' else 'b' if r[0] in FINAL else 't'].append((u, r))
            continue
        items = [i for k in by_ni.get(u.name, ())
                 for i in policy_table.ENTRIES[k].get('policy', ())]
        if items:
            out['t'].append((u, ('policy', [], '; '.join(
                ' '.join(str(x) for x in i[:2]) for i in items))))
            continue
        if u.cpython in refused:
            out['t'].append((u, ('refused', [], refused[u.cpython])))
            continue
        out['c'].append((u, None))
    return out


# ---- the table's own checks --------------------------------------------------

def check_table(table, units):
    problems = []
    names = {u.name for u in units}
    tree = csource.Tree(ROOT)
    docs = {}
    for f in ('DECISIONS.md', 'KNOWN_BUGS.md', 'METACOMPILER.md', 'PATTERNS.md'):
        with open(os.path.join(ROOT, 'doc', f)) as fh:
            docs[f] = fh.read()
    failures = docs['METACOMPILER.md'].partition('## Failures log')[2]
    for key, r in table.REASONS.items():
        if not (isinstance(r, tuple) and len(r) == 3 and r[0] in table.KINDS):
            problems.append('%s: not (kind, refs, why) with a kind of KINDS' % key)
            continue
        if key not in names:
            problems.append('%s: %s' % (key, 'not a mirror the tool finds' if tree.find(key)
                                        else 'not in Ni'))
        for ref in r[1]:
            m = re.fullmatch(r'(D|F|P|C)(\d+)', ref)
            if m:
                doc, pat = {'D': ('DECISIONS.md', r'\b%s\b' % ref),
                            'F': ('KNOWN_BUGS.md', r'\b%s\b' % ref),
                            'C': ('KNOWN_BUGS.md', r'\b%s\b' % ref),
                            'P': ('PATTERNS.md', r'\b%s\b' % ref)}[m.group(1)]
                if not re.search(pat, docs[doc]):
                    problems.append('%s: %s not in doc/%s' % (key, ref, doc))
            elif ref.startswith('failures:'):
                if ref[9:] not in failures:
                    problems.append('%s: no failures-log row names %s' % (key, ref[9:]))
            else:
                problems.append('%s: reference %r is not Dn, Fn, Pn, Cn or failures:NAME'
                                % (key, ref))
    for path in table.EXCLUDED:
        if not os.path.isfile(os.path.join(ROOT, path)):
            problems.append('EXCLUDED %s: no such file' % path)
    # every Ni name the policy table keys to CPython must be a unit here
    import policy_table
    for key, e in policy_table.ENTRIES.items():
        for n in e.get('ni', ()):
            if n.startswith('ni/') and n not in names and \
                    n.partition(':')[0] not in table.EXCLUDED:
                problems.append('ENTRIES %s names %s, which donecheck.py does not'
                                ' see as a definition' % (key, n))
    return problems


# ---- output -------------------------------------------------------------------

def summary(title, rows):
    """rows: [(file, unit)]: per file, count and lines."""
    per = collections.OrderedDict()
    for f, u in sorted(rows, key=lambda r: r[0]):
        n, l = per.get(f, (0, 0))
        per[f] = (n + 1, l + u.lines)
    print('%s: %d definitions, %d lines' % (title, len(rows), sum(u.lines for _, u in rows)))
    for f, (n, l) in per.items():
        print('  %-56s %4d %6d' % (f, n, l))


def by_kind(rows, key):
    n, lines = collections.Counter(), collections.Counter()
    for u, x in rows:
        n[key(u, x)] += 1
        lines[key(u, x)] += u.lines
    return ', '.join('%s %d (%d lines)' % (k, n[k], lines[k]) for k in sorted(n))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--gen', required=True,
                    help="generate.py's output directory (headers and report.txt)")
    ap.add_argument('--src', help="the CPython source (else as generate.py finds it)")
    ap.add_argument('-v', '--verbose', action='store_true', help='list every definition')
    ap.add_argument('--list', choices='abtcx', action='append', default=[],
                    help='list the definitions of a class (c is always listed)')
    args = ap.parse_args(argv)
    import cpython_src
    import hand_table
    try:
        version, sha, commit, rows = read_report(args.gen)
        src = args.src or cpython_src.find_source(version)
        cpython_src.check_version(src, version)
    except (OSError, ValueError, cpython_src.SourceError) as e:
        print('donecheck.py: no report to read in %s (%s)' % (args.gen, e))
        return 2
    why = stale(sha, commit, src)
    if why:
        print('donecheck.py: %s/report.txt is stale: %s; run generate.py again'
              % (args.gen, why))
        return 2
    refused = {loc: why for _, st, why, loc in rows if st == 'refused'}
    gen = header_units(args.gen)
    try:
        hand, by_ni = hand_units(src, hand_table)
    except csource.Unbalanced as e:
        print('donecheck.py: %s' % e)
        return 2
    classes = classify(hand, by_ni, refused, hand_table)
    problems = check_table(hand_table, hand)

    print('Done test, point 1 (GOAL): what mirrors CPython %s, by class' % version)
    print()
    statuses = collections.Counter(st for _, st, _, _ in rows)
    print('(a) generated: %s of %d functions the generators attempt (%s)'
          % (sum(statuses.values()) - statuses['refused'], len(rows), ', '.join(
              '%s %d' % kv for kv in sorted(statuses.items()))))
    summary('  in the headers', [(u.path, u) for u in gen])
    print()
    summary('(b) hand-written, final reason', [(u.path, u) for u, _ in classes['b']])
    print('  by reason: ' + by_kind(classes['b'], lambda _, r: r[0]))
    print()
    summary('(t) hand-written, transitional reason (open work: the done test'
            ' waits on these)', [(u.path, u) for u, _ in classes['t']])
    print('  by reason: ' + by_kind(classes['t'], lambda _, r: r[0]))
    print()
    summary('(c) hand-written, no reason', [(u.path, u) for u, _ in classes['c']])
    print()
    print('not mirrors (name matches, hand_table not_mirror): %d; files not read: %s'
          % (len(classes['x']), ', '.join(sorted(hand_table.EXCLUDED))))

    show = set(args.list) | {'c'} | (set('abtcx') if args.verbose else set())
    if 'a' in show:
        print('\n(a) generated')
        for key, st, _, loc in rows:
            if st != 'refused':
                print('  %-40s %-10s %s' % (key, st, loc))
    for cls, title in (('b', 'hand-written, final reason'),
                       ('t', 'hand-written, transitional reason'),
                       ('x', 'not mirrors'),
                       ('c', 'hand-written, no reason (the done test fails on these)')):
        if cls not in show or not classes[cls]:
            continue
        print('\n(%s) %s' % (cls, title))
        last = None
        for u, r in sorted(classes[cls], key=lambda x: x[0].path):
            if u.path != last:
                print('  %s' % u.path)
                last = u.path
            name = u.name.partition(':')[2]
            why = ''
            if r:
                why = '%s%s: %s' % (r[0], ' ' + ','.join(r[1]) if r[1] else '', r[2])
            print('    %-34s %4d  %-44s %s' % (name, u.lines, u.cpython or '-',
                                              '[%s]' % '+'.join(u.signals)))
            if why:
                print('        %s' % why[:300])
    if problems:
        print('\nhand_table.py problems:')
        for p in problems:
            print('  ' + p)
        return 2
    if classes['c']:
        return 1
    return 3 if classes['t'] else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
