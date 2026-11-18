#!/usr/bin/env python3
"""Deduplicate the ASan, LSan and UBSan reports of a tools/sanitize.sh run.

Each child writes its reports to LOGDIR/<test>.<asan|ubsan>.<pid>
(tests/testenv.py). Reports are grouped by kind and their top frames;
each group is printed once with the tests that hit it. A leak counts only
if Ni is in one of the first three frames past the allocator (CPython's
own leaks are not ours), and a report a plain run also gives is listed
apart. The full text of each group's first report goes to
LOGDIR/report.txt.

    python3 tools/sanitize_report.py LOGDIR [--frames N]

Exit status 1 if there is a report with Ni only.
"""
import collections, glob, os, re, sys

FRAME = re.compile(r'^\s*#(\d+) 0x[0-9a-f]+ in (\S+) (\S+)')
FRAME_BARE = re.compile(r'^\s*#(\d+) 0x[0-9a-f]+ +\(([^)]*)\)')
ALLOCATOR = re.compile(r'^(malloc|calloc|realloc|__interceptor_\w+|'
                       r'_?Py(Mem|Object)_\w*|_PyObject_\w*|_PyMem_\w*|'
                       r'operator new)')


def frames(lines):
    """(function, location) for each frame of the first stack in lines."""
    out = []
    for line in lines:
        m = FRAME.match(line)
        if m:
            out.append((m.group(2), m.group(3)))
            continue
        m = FRAME_BARE.match(line)
        if m:
            out.append(('?', m.group(2)))
        elif out:
            break
    return out


def in_ni(loc):
    return loc.startswith(('ni/', './ni/', 'build/')) or '/ni.cpython' in loc


def split_asan(text):
    """ASan and LSan reports in one log: (kind, lines) each."""
    out = []
    cur = None
    for line in text.splitlines():
        m = re.match(r'==\d+==ERROR: (AddressSanitizer|LeakSanitizer): (.*)', line)
        if m:
            cur = [m.group(1) + ': ' + re.sub(r' on (unknown )?address.*', '', m.group(2))]
            out.append(cur)
            continue
        if cur is not None:
            cur.append(line)
    return out


def leaks(rep):
    """A LeakSanitizer report: one (kind, lines) per leak with Ni in one of
    the first three frames past the allocator (Ni may allocate through a
    constructor such as PyTuple_New)."""
    out = []
    block = None
    for line in rep[1:] + ['']:
        m = re.match(r'(Direct|Indirect) leak of \d+ byte', line)
        if m or not line.strip():
            if block:
                fr = [f for f in frames(block) if not ALLOCATOR.match(f[0])]
                if any(in_ni(loc) for _, loc in fr[:3]):
                    out.append(['LeakSanitizer: %s leak' % block[0].split()[0]] + block)
            block = [line] if m else None
        elif block is not None:
            block.append(line)
    return out


def split_ubsan(text):
    out = []
    cur = None
    for line in text.splitlines():
        m = re.match(r'(\S+:\d+:\d+): runtime error: (.*)', line)
        if m:
            msg = re.sub(r'0x[0-9a-f]+', 'ADDR', m.group(2))
            msg = re.sub(r'-?\d+', 'N', msg)
            cur = ['UBSan: %s: %s' % (m.group(1), msg), line]
            out.append(cur)
        elif cur is not None and (line.startswith(' ') or not line.strip()):
            cur.append(line)
        else:
            cur = None
    return out


def main():
    args = sys.argv[1:]
    nframes = 4
    if '--frames' in args:
        k = args.index('--frames')
        nframes = int(args[k + 1])
        del args[k:k + 2]
    logdir = args[0]
    groups = collections.OrderedDict()
    for path in sorted(glob.glob(os.path.join(logdir, '*.asan.*')) +
                       glob.glob(os.path.join(logdir, '*.ubsan.*'))):
        name = os.path.basename(path)
        tag, kind = re.match(r'(.*)\.(asan|ubsan)\.\d+$', name).groups()
        text = open(path, errors='replace').read()
        if kind == 'asan':
            reps = []
            for r in split_asan(text):
                reps += leaks(r) if r[0].startswith('LeakSanitizer') else [r]
        else:
            reps = split_ubsan(text)
        for rep in reps:
            fr = [f for f in frames(rep[1:]) if not ALLOCATOR.match(f[0])]
            key = (rep[0],) + tuple('%s %s' % f for f in fr[:nframes])
            g = groups.setdefault(key, {'tests': [], 'text': rep, 'path': path})
            if tag not in g['tests']:
                g['tests'].append(tag)
    # a report a plain run also gives is CPython's (or a test's deliberate
    # Py_FatalError in a subprocess), not Ni's
    ours = [(k, g) for k, g in groups.items()
            if not any(t.endswith('.plain') for t in g['tests'])]
    theirs = [(k, g) for k, g in groups.items() if (k, g) not in ours]
    with open(os.path.join(logdir, 'report.txt'), 'w') as f:
        for i, (key, g) in enumerate(ours + theirs, 1):
            f.write('=== %d: %s%s\nfirst in %s\ntests: %s\n\n' % (
                i, key[0], '' if (key, g) in ours else ' (also without Ni)',
                g['path'], ' '.join(g['tests'])))
            f.write('\n'.join(g['text']) + '\n\n')
    for i, (key, g) in enumerate(ours + theirs, 1):
        if i == len(ours) + 1:
            print('also without Ni:')
        tests = g['tests']
        print('%d. %s  [%d test%s: %s%s]' % (
            i, key[0], len(tests), '' if len(tests) == 1 else 's',
            ' '.join(tests[:4]), ' ...' if len(tests) > 4 else ''))
        for fr in key[1:]:
            print('     ' + fr)
    print('%d distinct reports with Ni only, %d also without (full text: %s)' % (
        len(ours), len(theirs), os.path.join(logdir, 'report.txt')))
    return 1 if ours else 0

if __name__ == '__main__':
    sys.exit(main())
