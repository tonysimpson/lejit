"""Where Ni's emitted code goes, for one pyperformance benchmark.

Needs a build with the counters:

    NI_CODESTATS=1 python3.7 setup.py build_ext --inplace
    python3.7 tools/codesize.py richards          # tables
    python3.7 tools/codesize.py richards -n 30    # longer top lists
    python3.7 tools/codesize.py richards -d       # also identical copies
                                                  # (needs capstone)
    python3.7 tools/codesize.py richards -s       # also what differs between
                                                  # blocks at one merge point
    python3.7 tools/codesize.py richards -p       # also promotion sites
                                                  # (with -d: their copies;
                                                  # -j FILE: also as JSON)
    python3.7 tools/codesize.py richards -D       # also the code after
                                                  # sites with several values
                                                  # (doc/FINDINGS.md; capstone)

Runs the benchmark until compiling stops, then prints the code emitted per
opcode, how many times instructions were compiled (each promotion, respawn
or new merge-point state compiles the code after it again), and the
instructions and functions with the most code. Bytes are counted from the
start of one instruction to the start of the next (pycompiler.c,
codestats_begin): an instruction's error paths count with it; respawn and
promotion stubs, function headers and replays don't.
"""
import collections
import dis
import os
import sys

KINDS = ('analyse', 'call', 'pause', 'respawn', 'promotion')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tools'))


def main():
    args = sys.argv[1:]
    top = 15
    if '-n' in args:
        i = args.index('-n')
        top = int(args[i + 1])
        del args[i:i + 2]
    jsonfile = None
    if '-j' in args:
        i = args.index('-j')
        jsonfile = args[i + 1]
        del args[i:i + 2]
    want_dups = '-d' in args
    want_states = '-s' in args
    want_shared = '-D' in args
    want_report = '-p' in args
    want_promos = want_report or want_shared
    args = [a for a in args if a not in ('-d', '-s', '-p', '-D')]
    name = args[0]
    import pyperf37
    import ni
    if not hasattr(ni, '_codestats'):
        sys.exit('build with NI_CODESTATS=1 (see the docstring)')
    benches = pyperf37.load(os.path.join(ROOT, 'tools', 'pyperformance',
                                         'bm_%s.py' % name))
    ni._codestats()
    stats0 = ni._stats()
    promos0 = ni._promostats() if want_promos else None
    iterations = 0
    for _, timed in benches:
        last = -1
        for _ in range(50):          # until a run compiles nothing new
            timed(1)
            iterations += 1
            now = ni._stats()['code_bytes']
            if now == last:
                break
            last = now
    stats, lost, bufs, states = ni._codestats()
    promos1 = ni._promostats() if want_promos else None
    stats1 = ni._stats()
    total = sum(b for n, b in bufs.values())
    print('%-14s %8s %8s %6s %8s' % ('buffer kind', 'count', 'KB', '%', 'B/each'))
    for k, (n, b) in sorted(bufs.items(), key=lambda x: -x[1][1]):
        print('%-14s %8d %8.1f %5.1f%% %8.0f' % (k, n, b / 1024,
                                                 pct(b, total), b / max(n, 1)))
    print()
    report(stats, lost, total, top)
    dupsites = None
    if want_dups:
        dupsites = duplicates(stats, top)
    if want_states:
        state_diffs(states, top)
    if want_report:
        import promostats
        promostats.report(ni, stats0, stats1, promos0, promos1, bufs,
                          dupsites, top,
                          jsonfile)
    if want_shared:
        import promostats
        promostats.shared_after(ni, stats, promos1, total, top,
                                lambda a, c, p: normalise(a, c, p),
                                iterations)


def normalise(addr, code, pointers):
    """The copy's instructions as text, with branch and call targets (and,
    if 'pointers', 64-bit immediates such as per-site guard addresses)
    replaced, so copies at different addresses compare equal."""
    import capstone
    md = getattr(normalise, 'md', None)
    if md is None:
        md = normalise.md = capstone.Cs(capstone.CS_ARCH_X86,
                                        capstone.CS_MODE_64)
    out = []
    for insn in md.disasm(code, addr):
        text = insn.mnemonic + ' ' + insn.op_str
        if insn.mnemonic.startswith(('j', 'call', 'loop')) or 'rip' in text:
            text = insn.mnemonic + ' T'
        elif pointers:
            text = ' '.join('P' if w.rstrip(',').startswith('0x') and
                            len(w.rstrip(',')) > 10 else w
                            for w in text.split())
        out.append(text)
    return '\n'.join(out)


def duplicates(stats, top):
    tot = dup1 = dup2 = 0
    byop = collections.Counter()
    bykind = collections.Counter()
    bysite1 = collections.Counter()  # dup bytes by promotion site (or -1)
    bysite = collections.Counter()   # the same, 64-bit constants aside
    bysame = collections.Counter()   # ... of which a copy from the same site
    rows = []
    for (co, i), (op, n, b, copies) in stats.items():
        tot += sum(len(c[1]) for c in copies)
        if len(copies) < 2:
            continue
        seen1, seen2 = set(), {}
        d2 = 0
        for addr, code, kind, site, *_ in copies:
            k1 = normalise(addr, code, False)
            k2 = normalise(addr, code, True)
            if k1 in seen1:
                dup1 += len(code)
                bykind[KINDS[kind]] += len(code)
                bysite1[site] += len(code)
            if k2 in seen2:
                d2 += len(code)
                bysite[site] += len(code)
                if site >= 0 and site in seen2[k2]:
                    bysame[site] += len(code)
            seen1.add(k1)
            seen2.setdefault(k2, set()).add(site)
        dup2 += d2
        byop[op] += d2
        rows.append((d2, co, i, op, len(copies), len(seen1), len(seen2)))
    print('\nidentical copies of one instruction, of %d KB counted:' % (tot / 1024))
    print('  same code, jump targets aside:           %6.1f KB %5.1f%%' % (
        dup1 / 1024, pct(dup1, tot)))
    print('  ... and 64-bit constants (guards) aside: %6.1f KB %5.1f%%' % (
        dup2 / 1024, pct(dup2, tot)))
    print('  identical copies by how the compiler was entered: ' +
          ', '.join('%s %.1f KB' % kv for kv in
                    sorted(((k, v / 1024) for k, v in bykind.items()),
                           key=lambda x: -x[1])))
    print('\n%-22s %8s' % ('opcode', 'dup KB'))
    for op, b in byop.most_common(top):
        print('%-22s %8.1f' % (dis.opname[op], b / 1024))
    print('\n%-34s %-18s %6s %8s %8s %6s' % (
        'instruction', 'opcode', 'copies', 'distinct', 'no-ptrs', 'dup KB'))
    for d2, co, i, op, n, k1, k2 in sorted(rows, key=lambda r: -r[0])[:top]:
        print('%-34s %-18s %6d %8d %8d %6.1f' % (
            '%s:%d @%d' % (co.co_name, co.co_firstlineno, i),
            dis.opname[op], n, k1, k2, d2 / 1024))
    return bysite1, bysite, bysame


def slot_name(co, i):
    if i < 3:
        return ('continuation', 'globals', 'inlining')[i]
    i -= 3
    names = co.co_varnames + co.co_cellvars + co.co_freevars
    return names[i] if i < len(names) else 'stack%d' % (i - len(names))


def state_diffs(states, top):
    """For each merge point that started more than one block, the vlocals
    slots whose state differed between those blocks."""
    bymp = collections.defaultdict(list)
    aligned = 0
    for co, instr, slots, align_rejects in states:
        bymp[(co, instr)].append(slots)
        aligned += align_rejects > 0
    hist = collections.Counter()
    for sigs in bymp.values():
        n = len(sigs)
        hist[n if n < 4 else 4 if n < 8 else 8 if n < 16 else 16] += 1
    print('\nblocks per merge point: ' + ', '.join(
        '%s: %d' % ({4: '4-7', 8: '8-15', 16: '16+'}.get(k, str(k)), hist[k])
        for k in sorted(hist)) + '; max %d' % max(
            [len(s) for s in bymp.values()] or [0]))
    print('blocks started where a snapshot matched but for the 16-byte '
          'alignment test (KNOWN_BUGS C1): %d' % aligned)
    blame = collections.Counter()
    rows = []
    for (co, instr), sigs in bymp.items():
        if len(sigs) < 2:
            continue
        width = max(len(s) for s in sigs)
        varying = []
        for i in range(width):
            vals = collections.Counter(s[i] if i < len(s) else '<none>'
                                       for s in sigs)
            if len(vals) > 1:
                varying.append((slot_name(co, i), vals))
                blame[slot_name(co, i) + ' in ' + co.co_name] += len(sigs) - 1
        rows.append((len(sigs), co, instr, varying))
    print('\n%d merge points started %d blocks; %d started more than one' % (
        len(bymp), len(states), len(rows)))
    print('\nslots that differ most often (extra blocks they appear in):')
    for k, n in blame.most_common(top):
        print('  %5d  %s' % (n, k))
    print()
    for n, co, instr, varying in sorted(rows, key=lambda r: -r[0])[:top]:
        print('%s:%d @%d: %d blocks' % (co.co_name, co.co_firstlineno,
                                        instr, n))
        for name, vals in varying[:8]:
            shown = ', '.join('%s x%d' % (v[:60], c)
                              for v, c in vals.most_common(4))
            print('    %-14s %s' % (name, shown))


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def report(stats, lost, total, top):
    counted = sum(e[2] for e in stats.values())
    compiles = sum(e[1] for e in stats.values())
    print('code emitted %d KB; %d KB (%.0f%%) counted per instruction, '
          'the rest is stubs, headers and proxies; %d spans lost to '
          'buffer switches' % (total / 1024, counted / 1024,
                               pct(counted, total), lost))
    print('%d instructions in %d code objects, compiled %d times '
          '(%.1f copies each)\n' % (
              len(stats), len({co for co, _ in stats}), compiles,
              compiles / max(len(stats), 1)))

    byop = collections.defaultdict(lambda: [0, 0])
    for (co, i), (op, n, b, _) in stats.items():
        byop[op][0] += n
        byop[op][1] += b
    print('%-22s %8s %6s %8s %8s' % ('opcode', 'KB', '%', 'compiles',
                                       'B/compile'))
    for op, (n, b) in sorted(byop.items(), key=lambda x: -x[1][1])[:top]:
        print('%-22s %8.1f %5.1f%% %8d %8.0f' % (
            dis.opname[op], b / 1024, pct(b, counted), n, b / max(n, 1)))

    copies = collections.Counter()
    copybytes = collections.Counter()
    for (op, n, b, _) in stats.values():
        k = n if n < 4 else 4 if n < 10 else 10 if n < 50 else 50
        copies[k] += 1
        copybytes[k] += b
    print('\ntimes compiled   instructions   KB      %')
    for k in sorted(copies):
        label = {4: '4-9', 10: '10-49', 50: '50+'}.get(k, str(k))
        print('%-16s %12d %6.1f %5.1f%%' % (label, copies[k],
                                           copybytes[k] / 1024,
                                           pct(copybytes[k], counted)))

    byfn = collections.defaultdict(lambda: [0, 0, 0])
    for (co, i), (op, n, b, _) in stats.items():
        f = byfn[co]
        f[0] += b
        f[1] = max(f[1], n)
        f[2] += 1
    print('\n%-34s %8s %6s %10s' % ('function', 'KB', '%', 'max copies'))
    for co, (b, n, k) in sorted(byfn.items(), key=lambda x: -x[1][0])[:top]:
        print('%-34s %8.1f %5.1f%% %10d' % (
            '%s:%d' % (co.co_name, co.co_firstlineno), b / 1024,
            pct(b, counted), n))

    print('\n%-34s %-20s %8s %6s %8s' % ('instruction', 'opcode', 'KB',
                                         'copies', 'B/copy'))
    rows = sorted(stats.items(), key=lambda x: -x[1][2])[:top]
    for (co, i), (op, n, b, _) in rows:
        line = co.co_firstlineno
        for off, ln in dis.findlinestarts(co):
            if off > i:
                break
            line = ln
        print('%-34s %-20s %8.1f %6d %8.0f' % (
            '%s:%d @%d' % (co.co_name, line, i), dis.opname[op], b / 1024,
            n, b / max(n, 1)))


if __name__ == '__main__':
    main()
