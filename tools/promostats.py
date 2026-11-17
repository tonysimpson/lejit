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
PFLAGS = {0: 'word', 1: 'pyobj', 3: 'pyobj_mega', 7: 'static_type'}


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
            s = s[:4] + (0,) + s[5:7] + (vals, misses, s[9])  # stub: older
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
              parent) in sites:
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


def pct(a, b):
    return 100.0 * a / b if b else 0.0
