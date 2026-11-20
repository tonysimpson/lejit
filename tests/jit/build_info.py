"""ni.build_info is complete, read-only, and matches the build's features.

    python3.X tests/jit/build_info.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import common  # noqa: E402

sys.path.insert(0, common.NI_DIR)
import ni  # noqa: E402

KEYS = {'source_version', 'build', 'trace', 'codestats', 'sanitizer',
        'features'}


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
    header = os.path.join(common.build_temp(), 'gen', 'ni_features_gen.h')
    if not os.path.exists(header):
        problems.append('no %s' % os.path.relpath(header, common.ROOT))
    else:
        with open(header) as f:
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
