#!/usr/bin/env python3
"""Run every generator for one CPython: the entry point setup.py uses (D67).

    generate.py --python EXE --out DIR [--src DIR]

EXE is the interpreter Ni is being built for; its version (sys.version) picks
the source, and its installed pyconfig.h is the configuration. The source is
--src, else NI_CPYTHON_SRC, else the release tarball from the cache
(cpython_src.py); its Include/patchlevel.h must name the same version. Each
generator writes one header into DIR, rewritten only when its text changes so
that an unchanged header doesn't rebuild ni.so. Any mismatch or generator
error exits non-zero with the reason, and removes DIR's old headers so
nothing stale can be built.

This script runs under a newer Python than the one being built for (the
vendored cases_generator needs 3.10); setup.py finds it with NI_GEN_PYTHON.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# generator module -> the header it writes
GENERATORS = [('gen_long', 'plongobject_gen.h'),
              ('gen_float', 'pfloatobject_gen.h'),
              ('gen_ceval', 'pycompiler_gen.h'),
              ('gen_ceval:build_copies', 'pycompiler_copies_gen.h'),
              ('gen_layouts', 'layouts_gen.h'),
              ('gen_layouts:build_runtime', 'runtime_layouts_gen.h'),
              ('gen_abstract', 'pabstract_gen.h'),
              ('gen_abstract:build_tuple', 'ptupleobject_gen.h'),
              ('gen_abstract:build_list', 'plistobject_gen.h'),
              ('gen_abstract:build_dict', 'pdictobject_gen.h'),
              ('gen_abstract:build_bytes', 'pbytesobject_gen.h'),
              ('gen_call', 'psyfunc_gen.h')]
# (a 'module:function' entry calls that function instead of build)
# and mechanisms.py writes mechanisms.HEADERS: the snippet for each of Ni's
# own mechanisms that the source's answers choose (D75); gen_virtual.py
# writes gen_virtual.headers(): the field descriptors and, per virtual type
# policy_table.TYPES generates, its constructor and force function (D80)


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
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)
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
    if problems:
        return 'generate.py: the policy table (policy_table.py) does not' \
            ' match this source:\n  ' + '\n  '.join(problems)
    outputs = []
    for name, filename in GENERATORS:
        name, _, func = name.partition(':')
        mod = __import__(name)
        try:
            text, report = getattr(mod, func or 'build')(target)
        except cslots.GenError as e:
            return 'generate.py: %s: %s' % (name, e)
        outputs.append((filename, text))
        done = sum(1 for line in report if line.startswith('generated'))
        print('%-20s %d generated, %d not (CPython %s)'
              % (filename, done, len(report) - done, version))
        if args.verbose:
            print('\n'.join('  ' + line for line in report))
    import gen_virtual
    try:
        virtual = gen_virtual.build(target)
    except cslots.GenError as e:
        return 'generate.py: gen_virtual: %s' % e
    for filename, text, report in virtual:
        outputs.append((filename, text))
        print('%-20s %d generated (CPython %s)' % (filename, len(report), version))
        if args.verbose:
            print('\n'.join('  ' + line for line in report))
    import mechanisms
    try:
        chosen = mechanisms.build(target)
    except cslots.GenError as e:
        return 'generate.py: mechanisms: %s' % e
    for filename, text, report in chosen:
        outputs.append((filename, text))
        print('%-20s %s' % (filename, report[0]))
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
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
