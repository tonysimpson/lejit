"""ni.build_info: its keys are there, it is read-only, and its features are
the NI_HAS_ values of the ni_features_gen.h the build compiled.

    python3.7 tests/build_info.py
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import testenv  # noqa: E402

sys.path.insert(0, testenv.NI_DIR)
import ni  # noqa: E402

KEYS = {'source_version', 'source', 'source_sha256', 'source_git_commit',
        'python', 'generator_python', 'ni_commit', 'build', 'trace',
        'codestats', 'sanitizer', 'features'}


def main():
    info = ni.build_info
    problems = []
    if set(info) != KEYS:
        problems.append('keys %s, expected %s' % (sorted(info), sorted(KEYS)))
    for mapping, name in ((info, 'build_info'), (info['features'], 'features')):
        try:
            mapping['x'] = 1
            problems.append('%s is writable' % name)
        except TypeError:
            pass
    if info['source_version'] != sys.version.split()[0]:
        problems.append('source_version %r, the interpreter is %s'
                        % (info['source_version'], sys.version.split()[0]))
    if info['build'] not in ('release', 'checked'):
        problems.append('build %r' % info['build'])
    headers = glob.glob(os.path.join(testenv.ROOT, 'build', 'temp.*', 'gen',
                                     'ni_features_gen.h'))
    if len(headers) != 1:
        problems.append('expected one build/temp.*/gen/ni_features_gen.h, found %d'
                        % len(headers))
    else:
        with open(headers[0]) as f:
            want = {k: int(v) for k, v in
                    re.findall(r'^#define (NI_HAS_\w+) (\d)$', f.read(), re.M)}
        if dict(info['features']) != want:
            problems.append('features %s, the header says %s'
                            % (dict(info['features']), want))
    for p in problems:
        print('FAIL ni.build_info: %s' % p)
    if not problems:
        print('ni.build_info: %d keys, %d features, as built' % (
            len(info), len(info['features'])))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
