"""Promotion sites, from ni._promostats() (NI_CODESTATS=1 build).

Called by tools/codesize.py -p. A site is one run-time switch: the stub
psyco_finish_promotion emits, and a block compiled for each value it sees.
For each site this prints who asked for the value (a backtrace into ni.so,
resolved with addr2line), what kind of value it was, how many values it saw,
the bytes and time spent compiling after each, how often each value's block
ran and how often the inline cache missed (the C dispatcher was entered).

Each promoted block in this build starts with a 13-byte counter that a
release build doesn't have; the byte counts here leave it out.
"""
import collections
import dis
import json
import os
import subprocess

COUNTER = 13             # psyco_cs_promo_counter
KINDS = ('analyse', 'call', 'pause', 'respawn', 'promotion')
# helpers that only forward the request: skipped when naming who asked
GENERIC = {
    'PycException_Promote', 'psyco_atcompiletime', 'psyco_pyobj_atcompiletime',
    'psyco_pyobj_atcompiletime_mega', 'psyco_atcompiletime_static_type',
    'backtrace', '??',
}
PFLAGS = {0: 'word', 1: 'pyobj', 3: 'pyobj_mega', 7: 'static_type', 11: 'type_interp'}


def resolve(sofile, offsets):
    """offset -> [function names, innermost first] (inline frames too)"""
    offsets = sorted({o for o in offsets if o > 0})
    if not offsets:
        return {}
    out = subprocess.run(
        ['addr2line', '-a', '-f', '-i', '-e', sofile] +
        ['0x%x' % (o - 1) for o in offsets],
        stdout=subprocess.PIPE, universal_newlines=True).stdout.splitlines()
    res = {}
    cur = None
    i = 0
    while i < len(out):
        line = out[i]
        if line.startswith('0x'):
            cur = int(line, 16) + 1
            res[cur] = []
            i += 1
            continue
        res[cur].append(line)       # function name, then file:line
        i += 2
    return res


def requester(bt, names):
    """the chain of functions that asked, generic helpers dropped"""
    chain = []
    for off in bt[1:]:
        for fn in names.get(off, ['??']):
            if fn in GENERIC:
                continue
            if fn.startswith(('psyco_pycompiler_mainloop', 'psyco_compile',
                              'psyco_pycompiler')):
                return ' < '.join(chain[:3]) or fn
            if not chain or chain[-1] != fn:
                chain.append(fn)
    return ' < '.join(chain[:3]) or '?'


def value_kind(desc):
    t, detail = desc
    if t in ('type', 'MEGA', 'word'):
        return t
    if t in ('function', 'code', 'builtin_function_or_method', 'int',
             'bool', 'str', 'module', 'method', 'NoneType', 'tuple'):
        return t
    return 'object'


def where(co, instr):
    line = co.co_firstlineno
    for off, ln in dis.findlinestarts(co):
        if off > instr:
            break
        line = ln
    return '%s:%d @%d' % (co.co_name, line, instr)


def since(sites, before):
    """[(site id, site)] for what happened after 'before' (an earlier
    ni._promostats()): values compiled since count in full; older values
    keep only their runs since; misses since"""
    out = []
    for i, s in enumerate(sites):
        if i < len(before):
            old = before[i][7]
            vals = []
            for j, (d, ns, by, runs) in enumerate(s[7]):
                if j >= len(old):
                    vals.append((d, ns, by, runs))
                elif runs > old[j][3]:
                    vals.append((d, 0, 0, runs - old[j][3]))
            misses = s[8] - before[i][8]
            if not vals and not misses:
                continue
            s = s[:4] + (0,) + s[5:7] + (vals, misses) + s[9:]  # stub: older
        out.append((i, s))
    return out


def report(ni, stats0, stats1, before, after, bufs, dupsites, top,
           jsonfile=None):
    """before, after: ni._promostats() around the run; stats0, stats1:
    ni._stats() likewise"""
    sites = since(after, before)
    sofile = ni.__file__
    names = resolve(sofile, [o for _, s in sites for o in s[5]])
    rows = []
    for sid, (co, instr, op, pflags, stub, bt, entry, vals, misses,
              parent, *_) in sites:
        vals = [(d, ns, max(b - COUNTER, 0), runs) for d, ns, b, runs in vals]
        rows.append(dict(
            id=sid, where=where(co, instr), opcode=dis.opname[op],
            pflags=PFLAGS.get(pflags, str(pflags)), stub=stub, parent=parent,
            who=requester(bt, names), entry=KINDS[entry],
            values=vals, misses=misses,
            nvalues=sum(1 for v in vals if v[0][0] != 'MEGA' and v[1]),
            mega=any(v[0][0] == 'MEGA' for v in vals),
            bytes=sum(v[2] for v in vals), ns=sum(v[1] for v in vals),
            runs=sum(max(v[3], 0) for v in vals),
            kind=collections.Counter(value_kind(v[0]) for v in vals
                                     if v[0][0] != 'MEGA').most_common(1)[0][0]
            if any(v[0][0] != 'MEGA' for v in vals) else 'MEGA'))

    nprom = sum(bufs.get(k, (0, 0))[0] for k in ('promotion', 'megamorphic'))
    total = sum(b for n, b in bufs.values()) - COUNTER * nprom
    after = sum(r['bytes'] for r in rows)
    stubs = sum(r['stub'] for r in rows)
    comp_ns = sum(stats1[k][1] - stats0[k][1] for k in KINDS) * 1e9
    prom_ns = sum(r['ns'] for r in rows)
    print('\n== promotions ==')
    print('%d sites (%d compiled no value yet), %d values, %d megamorphic '
          'versions' % (len(rows), sum(1 for r in rows if not r['values']),
                        sum(r['nvalues'] for r in rows),
                        sum(1 for r in rows if r['mega'])))
    print('code: %d KB total; promotion stubs %.1f KB (%.1f%%, %.0f B each); '
          'compiled after a value %.1f KB (%.1f%%)' % (
              total / 1024, stubs / 1024, pct(stubs, total),
              stubs / max(len(rows), 1), after / 1024, pct(after, total)))
    print('compile time: %.2f ms total; after a value %.2f ms (%.0f%%)' % (
        comp_ns / 1e6, prom_ns / 1e6, pct(prom_ns, comp_ns)))
    print('  (the code compiled after a value includes every buffer closed '
          'while compiling it, and its time all of its compile; code reached '
          'later through a pause is counted as a pause)')

    def table(title, key):
        agg = collections.OrderedDict()
        for r in rows:
            a = agg.setdefault(key(r), [0, 0, 0, 0, 0, 0])
            a[0] += 1
            a[1] += r['nvalues']
            a[2] += r['mega']
            a[3] += r['bytes'] + r['stub']
            a[4] += r['ns']
            a[5] += r['misses']
        print('\n%-58s %5s %6s %4s %8s %7s %9s' % (
            title, 'sites', 'values', 'mega', 'KB+stub', 'ms', 'misses'))
        for k, a in sorted(agg.items(), key=lambda x: -x[1][3])[:top]:
            print('%-58s %5d %6d %4d %8.1f %7.2f %9d' % (
                k[:58], a[0], a[1], a[2], a[3] / 1024, a[4] / 1e6, a[5]))
        return agg

    table('by kind of value promoted', lambda r: r['kind'])
    table('by promotion flavour (pflags)', lambda r: r['pflags'])
    table('by who asked', lambda r: r['who'])
    table('by opcode', lambda r: r['opcode'])

    perinstr = collections.defaultdict(list)
    for r in rows:
        perinstr[r['where']].append(r)
    h2 = collections.Counter()
    hb = collections.Counter()
    for w, rs in perinstr.items():
        k = len(rs) if len(rs) < 4 else 4
        h2[k] += 1
        hb[k] += sum(r['bytes'] + r['stub'] for r in rs)
    print('\nsites per instruction (a copy of the code before compiled the '
          'site again): ' + ', '.join(
              '%s: %d instrs %.1f KB' % ('4+' if k == 4 else k, h2[k],
                                         hb[k] / 1024) for k in sorted(h2)))
    same_val = 0
    for w, rs in perinstr.items():
        if len(rs) < 2:
            continue
        seen = collections.Counter(v[0] for r in rs for v in r['values'] if v[1])
        same_val += sum(n - 1 for n in seen.values())
    print('values promoted again at an instruction where another site '
          'already promoted the same value: %d' % same_val)
    forks(rows, perinstr)
    hist = collections.Counter()
    for r in rows:
        n = r['nvalues']
        hist[n if n < 5 else 5] += 1
    print('\ndistinct values per site: ' + ', '.join(
        '%s: %d' % ('5+' if k == 5 else k, v) for k, v in sorted(hist.items())))

    # how often each value's block ran
    vals = [v for r in rows for v in r['values']]
    ran = collections.Counter()
    for v in vals:
        n = v[3]
        ran['<=1' if n <= 1 else '2-99' if n < 100 else
            '100-9999' if n < 10000 else '10000+'] += v[2]
    print('bytes compiled after a value, by how often its block ran: ' +
          ', '.join('%s %.1f KB' % (k, ran[k] / 1024)
                    for k in ('<=1', '2-99', '100-9999', '10000+')))

    if dupsites is not None:
        d1, d2, same = dupsites
        print('\nidentical copies (constants aside) by promotion site: '
              '%.1f KB from promotions (%.1f KB the same as a copy made '
              'for another value at the same site), %.1f KB otherwise' % (
                  sum(v for k, v in d2.items() if k >= 0) / 1024,
                  sum(same.values()) / 1024, d2.get(-1, 0) / 1024))
        byw = collections.Counter()
        byk = collections.Counter()
        for r in rows:
            byw[r['who']] += d2.get(r['id'], 0)
            byk[r['kind']] += d2.get(r['id'], 0)
        print('  by kind: ' + ', '.join('%s %.1f KB' % (k, v / 1024)
                                        for k, v in byk.most_common(top)))
        print('  by who asked:')
        for k, v in byw.most_common(top):
            print('    %7.1f KB  %s' % (v / 1024, k))
        print('\n%-34s %-18s %6s %6s %8s  %s' % (
            'site', 'opcode', 'values', 'dupKB', 'KB', 'who'))
        for r in sorted(rows, key=lambda r: -d2.get(r['id'], 0))[:top]:
            print('%-34s %-18s %6d %6.1f %8.1f  %s' % (
                r['where'][:34], r['opcode'], r['nvalues'],
                d2.get(r['id'], 0) / 1024, r['bytes'] / 1024, r['who'][:60]))

    print('\n%-34s %-18s %-11s %4s %6s %5s %8s %9s  %s' % (
        'site', 'opcode', 'kind', 'vals', 'KB', 'stub', 'ms', 'misses',
        'who / values (runs)'))
    for r in sorted(rows, key=lambda r: -r['bytes'])[:top]:
        print('%-34s %-18s %-11s %4d%s %6.1f %5d %8.2f %9d  %s' % (
            r['where'][:34], r['opcode'], r['kind'], r['nvalues'],
            'M' if r['mega'] else ' ', r['bytes'] / 1024, r['stub'],
            r['ns'] / 1e6, r['misses'], r['who'][:70]))
        print('%85s %s' % ('', ', '.join(
            '%s%s (%d)' % (d[0] if d[0] != 'type' else '',
                           (':' + str(d[1])[:24]) if d[1] != '' else '', runs)
            for d, ns, b, runs in r['values'][:6])))

    if jsonfile:
        with open(jsonfile, 'w') as f:
            json.dump(dict(total=total, stubs=stubs, after=after,
                           comp_ns=comp_ns, prom_ns=prom_ns,
                           sites=[dict(r, values=[list(v) for v in
                                                  r['values']])
                                  for r in rows],
                           dups=(dupsites and {str(k): v for k, v in
                                               dupsites[1].items()}),
                           same=(dupsites and {str(k): v for k, v in
                                               dupsites[2].items()}),
                           dups1=(dupsites and {str(k): v for k, v in
                                                dupsites[0].items()})),
                      f)


def forks(rows, perinstr):
    """Why an instruction has more than one site: walk each extra site's
    parents (the site whose value was being compiled when it was emitted)
    and the first site's, to where they meet. Meeting at an earlier site
    means its values forked the code; meeting at one of the two means both
    came from one value's compile (a loop reaching a merge point in a new
    state); not meeting means two compiles entered separately (pause,
    respawn, call) reached the instruction."""
    byid = {r['id']: r for r in rows}

    def chain(r):
        out = [r['id']]
        while r['parent'] in byid:
            r = byid[r['parent']]
            out.append(r['id'])
        return out
    cause = collections.Counter()
    nb = collections.Counter()
    for rs in perinstr.values():
        if len(rs) < 2:
            continue
        ca = chain(rs[0])
        for r in rs[1:]:
            cb = chain(r)
            meet = next((x for x in cb if x in ca), None)
            if meet is None:
                k = 'separate compiles, entered by %s and %s' % tuple(sorted(
                    (byid[ca[-1]]['entry'], byid[cb[-1]]['entry'])))
            elif meet in (r['id'], rs[0]['id']):
                k = 'within one value\'s compile'
            else:
                k = 'values at an earlier site'
            cause[k] += r['bytes']
            nb[k] += 1
    print('why an instruction has extra sites (sites, KB compiled after '
          'their values):')
    for k, v in cause.most_common():
        print('  %5d %7.1f KB  %s' % (nb[k], v / 1024, k))


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def shared_after(ni, stats, sites, total, top, normalise, iterations=1):
    """Option D (doc/FINDINGS.md): for each site whose code was compiled for two or
    more values in this run, how much each value's copy costs and how much
    of it depends on the value. Uses the copies of ni._codestats() (site
    and value index per copy); code compiled for a value after a coding
    pause is not attributed to it. Each instruction compiled for every
    value is 'same' if its copies are identical, 'consts' if identical once
    64-bit constants (type and guard pointers) are aside, else 'differs';
    an instruction compiled for some values only is 'differs'. The
    saving at K is the code of the K-th and later values less the largest
    of them (one shared copy kept; the generic copy taken as no larger),
    an upper bound. The same is given with each value's tree: the sites
    its code emitted (parent, parent's value) and their values' code, down
    the tree, summed over the outermost sites only. Runs: how often the blocks of the K-th and later values
    ran per iteration of the benchmark (counted from the first, so
    compiling runs count too): what would take the shared copy."""
    per = collections.defaultdict(lambda: collections.defaultdict(
        lambda: collections.defaultdict(list)))
    for (co, i), (op, n, b, copies) in stats.items():
        for c in copies:
            addr, code, kind, site = c[:4]
            vidx = c[4] if len(c) > 4 else -1
            if site < 0 or vidx < 0:
                continue
            desc = sites[site][7][vidx][0] if vidx < len(sites[site][7]) \
                else ('?', '')
            if desc[0] == 'MEGA':
                continue
            per[site][vidx][(co, i)].append((addr, code))
    names = resolve(ni.__file__, [o for s in per for o in sites[s][5]])
    # a value's code includes the sites its code emitted and their values,
    # down the tree (parent, parent's value)
    allbytes = collections.Counter()
    for (co, i), (op, n, b, copies) in stats.items():
        for c in copies:
            if c[3] >= 0:
                allbytes[c[3]] += len(c[1])
    children = collections.defaultdict(list)
    for sid, s in enumerate(sites):
        if len(s) > 10 and s[9] >= 0:
            children[(s[9], s[10])].append(sid)
    memo = {}

    def subtree(sid):
        if sid not in memo:
            memo[sid] = 0
            memo[sid] = allbytes[sid] + sum(
                subtree(c) for v in range(len(sites[sid][7]))
                for c in children.get((sid, v), ()))
        return memo[sid]
    rows = []
    for sid, vals in per.items():
        if len(vals) < 2:
            continue
        co, instr, op = sites[sid][0], sites[sid][1], sites[sid][2]
        order = sorted(vals)
        size = [sum(len(c) for cs in vals[v].values() for _, c in cs)
                for v in order]
        deep = [size[j] + sum(subtree(c) for c in children.get((sid, v), ()))
                for j, v in enumerate(order)]
        instrs = set(k for v in order for k in vals[v])
        cls = collections.Counter()
        opbytes = 0
        for k in instrs:
            b = sum(len(c) for v in order for _, c in vals[v].get(k, ()))
            if k == (co, instr):
                opbytes += b
            if any(k not in vals[v] for v in order):
                cls['differs'] += b
                continue
            full = [tuple(sorted(normalise(a, c, False) for a, c in vals[v][k]))
                    for v in order]
            noptr = [tuple(sorted(normalise(a, c, True) for a, c in vals[v][k]))
                     for v in order]
            if len(set(full)) == 1:
                cls['same'] += b
            elif len(set(noptr)) == 1:
                cls['consts'] += b
            else:
                cls['differs'] += b
        save = {}
        for K in (2, 3, 5):
            tail = size[K - 1:]
            save[K] = sum(tail) - max(tail) if len(tail) > 1 else 0
        save[1] = sum(size) - max(size)
        dsave = {}
        for K in (2, 3, 5):
            tail = deep[K - 1:]
            dsave[K] = sum(tail) - max(tail) if len(tail) > 1 else 0
        dsave[1] = sum(deep) - max(deep)
        rows.append(dict(id=sid, where=where(co, instr),
                         opcode=dis.opname[op], n=len(order), size=size,
                         total=sum(size), cls=cls, op=opbytes, save=save,
                         deep=deep, dsave=dsave,
                         who=requester(sites[sid][5], names),
                         vals=[sites[sid][7][v][0] for v in order],
                         runs=[sites[sid][7][v][3] for v in order]))
    T = sum(r['total'] for r in rows)
    print('\n== code after sites with two or more values (FINDINGS.md "Option D") ==')
    print('code emitted %.1f KB; %d sites with 2+ values compiled in the run '
          '(%s); their values\' code %.1f KB (%.1f%%)' % (
              total / 1024, len(rows), ', '.join(
                  '%d values: %d' % (k, v) for k, v in sorted(
                      collections.Counter(min(r['n'], 5) for r in rows)
                      .items())), T / 1024, pct(T, total)))
    cls = collections.Counter()
    for r in rows:
        cls.update(r['cls'])
    print('  of which: identical in every value %.1f KB, identical but for '
          '64-bit constants %.1f KB, differs or only in some values %.1f KB;'
          ' the site\'s own instruction %.1f KB' % (
              cls['same'] / 1024, cls['consts'] / 1024,
              cls['differs'] / 1024, sum(r['op'] for r in rows) / 1024))
    print('  upper bound saved, one shared copy from the K-th value: '
          + ', '.join('K=%d %.1f KB (%.1f%%)' % (
              K, sum(r['save'][K] for r in rows) / 1024,
              pct(sum(r['save'][K] for r in rows), total))
                      for K in (1, 2, 3, 5)))
    # a polymorphic site inside another's tree is counted in that tree
    poly = {r['id'] for r in rows}

    def nested(sid):
        p = sites[sid][9] if len(sites[sid]) > 10 else -1
        while p >= 0:
            if p in poly:
                return True
            p = sites[p][9] if len(sites[p]) > 10 else -1
        return False
    outer = [r for r in rows if not nested(r['id'])]
    print('  with the sites each value\'s code emitted, and theirs (%d '
          'outermost sites; %d inside another\'s tree): %.1f KB; upper bound '
          'saved ' % (len(outer), len(rows) - len(outer),
                      sum(sum(r['deep']) for r in outer) / 1024)
          + ', '.join('K=%d %.1f KB (%.1f%%)' % (
              K, sum(r['dsave'][K] for r in outer) / 1024,
              pct(sum(r['dsave'][K] for r in outer), total))
                      for K in (1, 2, 3, 5)))
    print('  runs per iteration (%d iterations) of the blocks of the K-th '
          'and later values: ' % iterations + ', '.join(
              'K=%d %.0f' % (K, sum(sum(max(x, 0) for x in r['runs'][K - 1:])
                                    for r in rows) / max(iterations, 1))
              for K in (1, 2, 3, 5)))
    byw = collections.defaultdict(lambda: [0, 0, 0])
    for r in rows:
        w = byw[r['who']]
        w[0] += 1
        w[1] += r['total']
        w[2] += r['save'][2]
    print('  by who asked (sites, KB, saved at K=2 KB):')
    for k, (n, b, s) in sorted(byw.items(), key=lambda x: -x[1][1])[:top]:
        print('    %4d %7.1f %7.1f  %s' % (n, b / 1024, s / 1024, k[:70]))
    print('\n%-34s %-16s %4s %20s %6s %6s %6s %6s  %s' % (
        'site', 'opcode', 'vals', 'B per value+tree', 'same', 'consts',
        'differs', 'K=2', 'who / values (runs)'))
    for r in sorted(rows, key=lambda r: -sum(r['deep']))[:top]:
        print('%-34s %-16s %4d %20s %6d %6d %6d %6d  %s' % (
            r['where'][:34], r['opcode'][:16], r['n'],
            ','.join(str(x) for x in r['deep'])[:20], r['cls']['same'],
            r['cls']['consts'], r['cls']['differs'], r['dsave'][2],
            r['who'][:60]))
        print('%95s %s' % ('', ', '.join(
            '%s:%s (%d)' % (d[0], str(d[1])[:20], n)
            for d, n in zip(r['vals'][:6], r['runs'][:6]))))
