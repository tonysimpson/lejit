#!/usr/bin/env python3
"""Run every generator for one CPython: the entry point setup.py uses.

    generate.py --python EXE --out DIR [--src DIR] [--past-policy] [--no-cache]

EXE is the interpreter Ni is being built for; its version (sys.version) picks
the source, and its installed pyconfig.h is the configuration. The source is
--src, else NI_CPYTHON_SRC, else the release tarball from the cache
(cpython_src.py); its Include/patchlevel.h must name the same version. Each
generator writes one header into DIR, rewritten only when its text changes so
that an unchanged header doesn't rebuild ni.so. Any mismatch or generator
error exits non-zero with the reason, and removes DIR's old headers so
nothing stale can be built (report.txt stays, so that a difference from
the baseline can be accepted).

DIR/report.txt has a line per function a generator attempts, sorted, with
tab-separated columns: key, status, reason, location. The key doesn't name a
file, so that versions compare (status_diff.py): an exported function's
name (PyNumber_Add), a static slot's type and slot (PyFloat_Type.nb_add),
an opcode's name, a builtin's module.name, a virtual type's constructor as
virtual.NAME, and any other static's name, or its location where that name
is ambiguous. The status is generated, resumed (in part; the reason lists
each resume), restarted (the whole function, at once) or refused. The first
line, '# VERSION SOURCE-SHA256 GENERATOR-COMMIT', says what it was made
from. The key, status and reason of the other lines must equal those of the
checked-in baseline, expected/VERSION.txt: any difference is printed and
fails, so that a function sliding from generated to refused can't pass
silently. To accept the differences, copy the report over the baseline:
    cp DIR/report.txt tools/metac/expected/VERSION.txt

--past-policy is for a dry run on a version Ni isn't built for yet
(doc/PORT38.md): the policy table's problems are printed instead of
stopping, and every generator runs and writes its header, to be read and
compared, not built (setup.py never passes it).

--no-cache (or NI_NO_CACHE=1) neither reads nor writes the cache of parsed
files and effects (cache.py), which only saves time: the headers are the
same either way.

This script runs under a newer Python than the one being built for (the
vendored cases_generator needs 3.10); setup.py finds it with NI_GEN_PYTHON.
"""
import argparse
import difflib
import hashlib
import importlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

# generator module -> the header it writes
GENERATORS = [('gen2.abstract:build_long', 'plongobject_gen.h'),
              ('gen_ceval', 'pycompiler_gen.h'),
              ('gen_ceval:build_copies', 'pycompiler_copies_gen.h'),
              ('gen_layouts', 'layouts_gen.h'),
              ('gen_layouts:build_runtime', 'runtime_layouts_gen.h'),
              ('gen2.abstract', 'pabstract_gen.h'),
              ('gen2.abstract:build_tuple', 'ptupleobject_gen.h'),
              ('gen2.abstract:build_list', 'plistobject_gen.h'),
              ('gen2.abstract:build_dict', 'pdictobject_gen.h'),
              ('gen2.abstract:build_bytes', 'pbytesobject_gen.h'),
              ('gen2.abstract:build_unicode', 'punicodeobject_gen.h'),
              ('gen_abstract:build_structmember', 'pstructmember_gen.h'),
              ('gen_abstract:build_descr', 'pdescrobject_gen.h'),
              ('gen_abstract:build_object', 'pobject_gen.h'),
              ('gen2.abstract:build_bltin', 'pbltinmodule_gen.h'),
              ('gen2.abstract:build_math', 'pmathmodule_gen.h'),
              ('gen2.binding', 'psyfunc_gen.h'),
              ('gen2.abstract:build_float', 'pfloatslots_gen.h')]
# (a 'module:function' entry calls that function instead of build)
# and mechanisms.py writes mechanisms.HEADERS: the snippet for each of Ni's
# own mechanisms that the source's answers choose; gen_virtual.py
# writes gen_virtual.headers(): the field descriptors and, per virtual type
# policy_table.TYPES generates, its constructor and force function

# headers whose report lines are not functions
NOT_FUNCTIONS = {'layouts_gen.h', 'runtime_layouts_gen.h',
                 'pycompiler_copies_gen.h', 'virtual_fields_gen.h'}


def target_info(python):
    """(version, pyconfig.h path) of the interpreter being built for."""
    code = ('import sys, sysconfig; print(sys.version.split()[0]);'
            ' print(sysconfig.get_config_h_filename())')
    r = subprocess.run([python, '-I', '-c', code], stdout=subprocess.PIPE,
                       universal_newlines=True, timeout=60)
    if r.returncode != 0:
        return None, None
    version, pyconfig = r.stdout.split('\n')[:2]
    return version, pyconfig


def generator_commit():
    """HEAD, with a hash of any uncommitted change to the generators or to
    Ni (whose declarations and snippets they read) after a '+'."""
    def git(*a):
        return subprocess.run(('git',) + a, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=60).stdout
    paths = ('--', 'tools/metac', 'ni', ':(exclude)tools/metac/expected')
    try:
        head = git('rev-parse', 'HEAD').decode().strip()
        dirty = git('diff', 'HEAD', *paths)
        for p in git('ls-files', '--others', '--exclude-standard', '-z',
                     *paths).split(b'\0'):
            if p:
                with open(os.path.join(ROOT, p.decode()), 'rb') as f:
                    dirty += p + f.read()
    except (OSError, subprocess.SubprocessError):
        return 'unknown'
    if not head:
        return 'unknown'
    return head + ('+' + hashlib.sha256(dirty).hexdigest()[:12] if dirty else '')


def functions(reports):
    """The report's lines, sorted; a GenError for a report line that can't
    be classified."""
    import cslots
    out = {}                  # (header, key, location) -> [status, reasons]
    for filename, report in reports:
        if filename in NOT_FUNCTIONS or filename.startswith('mechanism_'):
            continue
        for line in report:
            kind, key, loc, rest = (line.split(None, 3) + [''])[:4]
            e = out.setdefault((filename, key, loc), [None, []])
            if kind == 'generated':
                e[0] = e[0] or 'generated'
            elif kind == 'skipped':
                e[:] = ['refused', [rest]]
            elif kind == 'restarts':
                e[:] = ['restarted', [rest]]
            elif kind == 'resumes':
                if e[0] is None:
                    raise cslots.GenError('%s: %s resumes but was not generated'
                                          % (filename, loc))
                if e[0] == 'generated':
                    e[:] = ['resumed', []]
                if e[0] == 'resumed':
                    e[1].append(rest)
            else:
                raise cslots.GenError('%s: a report line of no known kind: %s'
                                      % (filename, line))
    places = {}
    for _, key, loc in out:
        places.setdefault(key, set()).add(loc)
    return sorted('\t'.join((key if len(places[key]) == 1 else loc, st,
                             '; '.join(why).replace('\t', ' '), loc))
                  for (_, key, loc), (st, why) in out.items())


def baseline_path(version):
    return os.path.join(HERE, 'expected', version + '.txt')


def compared(text):
    """The lines of a report or baseline that must agree: key, status and
    reason (the location may change, or be left out of a baseline)."""
    return ['\t'.join(ln.split('\t')[:3]) for ln in text.split('\n')
            if ln and not ln.startswith('#')]


def baseline_diff(version, report):
    """The report's differences from the baseline, as a diff ('' if none)."""
    path = baseline_path(version)
    if not os.path.exists(path):
        return 'no baseline %s' % os.path.relpath(path, ROOT)
    with open(path) as f:
        was = compared(f.read())
    return '\n'.join(difflib.unified_diff(was, compared(report), 'baseline',
                                           'report', lineterm=''))


def main(argv):
    err = generate(argv)
    if err:
        out = argv[argv.index('--out') + 1] if '--out' in argv else None
        import mechanisms
        import gen_virtual
        for filename in [f for _, f in GENERATORS] + mechanisms.HEADERS \
                + gen_virtual.headers() if out else ():
            path = os.path.join(out, filename)
            if os.path.exists(path):
                os.remove(path)
    return err


def generate(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--python', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--src')
    ap.add_argument('--past-policy', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)
    if args.no_cache:
        os.environ['NI_NO_CACHE'] = '1'
    if sys.version_info < (3, 10):
        return 'generate.py needs Python 3.10 or later to run (it is %s)' % (
            sys.version.split()[0])
    import cpython_src
    import cslots
    version, pyconfig = target_info(args.python)
    if version is None:
        return 'generate.py: cannot run %s' % args.python
    try:
        if args.src:
            cpython_src.check_version(args.src, version)
            src = args.src
        else:
            src = cpython_src.find_source(version)
    except cpython_src.SourceError as e:
        return 'generate.py: %s' % e
    target = cslots.Target(os.path.abspath(src), version, args.python,
                           pyconfig)
    import policy
    problems = policy.check(target.src)
    if problems and args.past_policy:
        print('policy table: %d problems against this source (--past-policy:'
              ' generating anyway, not for a build)' % len(problems))
        if args.verbose:
            print('\n'.join('  ' + p for p in problems))
    elif problems:
        return 'generate.py: the policy table (policy_table.py) does not' \
            ' match this source:\n  ' + '\n  '.join(problems)
    outputs, reports = [], []
    for name, filename in GENERATORS:
        name, _, func = name.partition(':')
        mod = importlib.import_module(name)
        try:
            text, report = getattr(mod, func or 'build')(target)
        except cslots.GenError as e:
            return 'generate.py: %s: %s' % (name, e)
        outputs.append((filename, text))
        reports.append((filename, report))
        if args.verbose:
            print('%s\n' % filename + '\n'.join('  ' + line for line in report))
    import gen_virtual
    try:
        virtual = gen_virtual.build(target)
    except cslots.GenError as e:
        return 'generate.py: gen_virtual: %s' % e
    for filename, text, report in virtual:
        outputs.append((filename, text))
        reports.append((filename, report))
        if args.verbose:
            print('%s\n' % filename + '\n'.join('  ' + line for line in report))
    import mechanisms
    try:
        chosen = mechanisms.build(target)
    except cslots.GenError as e:
        return 'generate.py: mechanisms: %s' % e
    for filename, text, report in chosen:
        outputs.append((filename, text))
        print('%-26s %s' % (filename, report[0]))
    try:
        lines = functions(reports)
    except cslots.GenError as e:
        return 'generate.py: %s' % e
    report = '\n'.join(['# %s %s %s' % (version, cpython_src.source_sha(target.src),
                                         generator_commit())] + lines) + '\n'
    outputs.append(('report.txt', report))
    counts = {}
    for ln in lines:
        st = ln.split('\t')[1]
        counts[st] = counts.get(st, 0) + 1
    print('report.txt                 %s (CPython %s)' % (', '.join(
        '%d %s' % (n, st) for st, n in sorted(counts.items())), version))
    os.makedirs(args.out, exist_ok=True)
    for filename, text in outputs:
        path = os.path.join(args.out, filename)
        if os.path.exists(path):
            with open(path) as f:
                if f.read() == text:
                    continue
        with open(path + '.tmp', 'w') as f:
            f.write(text)
        os.replace(path + '.tmp', path)
    # a header no generator writes would otherwise stay in a reused build
    # directory and could still be included
    written = {filename for filename, _ in outputs}
    for filename in os.listdir(args.out):
        if filename.endswith('_gen.h') and filename not in written:
            os.remove(os.path.join(args.out, filename))
    diff = baseline_diff(version, report)
    if diff:
        return ('generate.py: report.txt differs from its baseline; to accept'
                ' it:\n  cp %s %s\n%s'
                % (os.path.join(args.out, 'report.txt'),
                   os.path.relpath(baseline_path(version), ROOT), diff))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
