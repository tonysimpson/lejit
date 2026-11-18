#!/usr/bin/env python3
"""GOAL's done test, point 1, as a check (GENERATION step 10, D90).

Lists every definition in Ni that mirrors a CPython one and puts each in one
class:
  (a) generated   written at build time by a generator (its report says
                  'generated'); counted in the generated headers' lines
  (b) reasoned    hand-written, with a named reason: an item of
                  hand_table.REASONS, the policy items of the policy table
                  entry that names it, or the generator's own refusal of
                  its CPython counterpart
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

  donecheck.py --python EXE [--src DIR] [-v] [--list a|b|c|x] [--past-policy]

--past-policy runs past the policy table's problems, as generate.py's
does, for a dry run on a version Ni isn't built for yet.

Exit 0 when (c) is empty, 1 when it is not, 2 when hand_table.py names
something that isn't in Ni or a reference that isn't in the docs.
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

Unit = collections.namedtuple('Unit', 'name path lines cpython signals span')


def def_span(d):
    ls = [t.line for t in d.toks if t.line]
    return (min(ls), max(ls)) if ls else (0, 0)


def def_lines(d):
    lo, hi = def_span(d)
    return hi - lo + 1


# ---- (a): what the generators write ------------------------------------------

def run_generators(target, past_policy=False):
    """[(header, text, report lines)] of every generator, as generate.py
    runs them (the policy table checked first, unless past_policy)."""
    import cslots
    import generate
    import gen_virtual
    import policy
    problems = policy.check(target.src)
    if problems and past_policy:
        print('policy table: %d problems against this source (--past-policy)'
              % len(problems))
    elif problems:
        raise cslots.GenError('the policy table does not match this source:\n  '
                              + '\n  '.join(problems))
    out = []
    for name, filename in generate.GENERATORS:
        mod, _, func = name.partition(':')
        text, report = getattr(__import__(mod), func or 'build')(target)
        out.append((filename, text, report))
    out += gen_virtual.build(target)
    return out


def generated_units(outputs):
    """[Unit] for each 'generated' report line that is a definition (not a
    layout field), with the lines of what the header holds for it."""
    units = []
    for filename, text, report in outputs:
        if filename.startswith(('layouts', 'runtime_layouts', 'virtual_fields')):
            continue
        toks, macros = ctokens.tokenize(text, filename)
        defs = csource.scan(toks, macros, filename)
        defs.update(csource._cases(toks, filename))
        used = set()
        for line in report:
            words = line.split()
            if not words or words[0] != 'generated' or len(words) < 2:
                continue
            cname = words[1]
            ni_names = [w.strip(',') for w in words[2:]] or [cname]
            if ':' in cname:          # a struct's field descriptors (gen_virtual)
                continue
            mine = []
            for n in ni_names + ['case ' + cname]:
                if n in defs:
                    mine.append(n)
            # its run-time copies: cimpl_<name>, cimpl_<name>_<k>
            for n in defs:
                for base in [cname] + ni_names:
                    if re.fullmatch(r'cimpl_%s(_\d+)?' % re.escape(base), n):
                        mine.append(n)
            mine = [n for n in dict.fromkeys(mine) if n not in used]
            used.update(mine)
            lines = sum(def_lines(defs[n]) for n in mine)
            units.append(Unit(cname, filename, lines, cname, ('generated',), None))
    return units


def refusals(outputs):
    """{CPython name: reason} for each 'skipped' report line."""
    out = {}
    for filename, text, report in outputs:
        for line in report:
            words = line.split(None, 2)
            if len(words) >= 2 and words[0] == 'skipped':
                out.setdefault(words[1], '%s: %s' % (filename, words[2] if len(words) > 2 else ''))
    return out


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
    """{class: [(unit, reason)]}; classes 'b', 'c' and 'x' (not a mirror)."""
    import policy_table
    out = collections.defaultdict(list)
    for u in units:
        r = table.REASONS.get(u.name)
        if r is not None:
            out['x' if r[0] == 'not_mirror' else 'b'].append((u, r))
            continue
        items = [i for k in by_ni.get(u.name, ())
                 for i in policy_table.ENTRIES[k].get('policy', ())]
        if items:
            out['b'].append((u, ('policy', [], '; '.join(
                ' '.join(str(x) for x in i[:2]) for i in items))))
            continue
        cname = u.cpython.rpartition(':')[2] if u.cpython else None
        if cname and cname.startswith('TARGET('):
            cname = cname[7:-1]
        if cname in refused:
            out['b'].append((u, ('refused', [], refused[cname])))
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


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--python', required=True, help='the interpreter Ni is built for')
    ap.add_argument('--src', help='the CPython source (else as generate.py finds it)')
    ap.add_argument('-v', '--verbose', action='store_true', help='list every definition')
    ap.add_argument('--list', choices='abcx', action='append', default=[],
                    help='list the definitions of a class (c is always listed)')
    ap.add_argument('--past-policy', action='store_true',
                    help="run past the policy table's problems (a dry run)")
    args = ap.parse_args(argv)
    import cpython_src
    import cslots
    import generate
    import hand_table
    version, pyconfig = generate.target_info(args.python)
    if version is None:
        print('donecheck.py: cannot run %s' % args.python)
        return 2
    src = args.src or cpython_src.find_source(version)
    if args.src:
        cpython_src.check_version(src, version)
    target = cslots.Target(os.path.abspath(src), version, args.python, pyconfig)
    try:
        outputs = run_generators(target, args.past_policy)
    except cslots.GenError as e:
        print('donecheck.py: %s' % e)
        return 2
    gen = generated_units(outputs)
    hand, by_ni = hand_units(target.src, hand_table)
    classes = classify(hand, by_ni, refusals(outputs), hand_table)
    problems = check_table(hand_table, hand)

    print('Done test, point 1 (GOAL): what mirrors CPython %s, by class' % version)
    print()
    summary('(a) generated', [(u.path, u) for u in gen])
    print()
    summary('(b) hand-written, reasoned', [(u.path, u) for u, _ in classes['b']])
    kinds = collections.Counter(r[0] for _, r in classes['b'])
    lines = collections.Counter()
    for u, r in classes['b']:
        lines[r[0]] += u.lines
    print('  by reason: ' + ', '.join('%s %d (%d lines)' % (k, n, lines[k])
                                      for k, n in sorted(kinds.items())))
    print()
    summary('(c) hand-written, no reason', [(u.path, u) for u, _ in classes['c']])
    print()
    print('not mirrors (name matches, hand_table not_mirror): %d; files not read: %s'
          % (len(classes['x']), ', '.join(sorted(hand_table.EXCLUDED))))

    show = set(args.list) | {'c'} | (set('abcx') if args.verbose else set())
    if 'a' in show:
        print('\n(a) generated')
        for u in gen:
            print('  %-28s %-34s %4d' % (u.path, u.name, u.lines))
    for cls, title in (('b', 'hand-written, reasoned'), ('x', 'not mirrors'),
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
    return 1 if classes['c'] else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
