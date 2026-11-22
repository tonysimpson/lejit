"""lejit._core._build is complete and matches the build's features and
messages, and lejit.build_features() holds the same.

    python3.X tests/jit/build_info.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import common  # noqa: E402

sys.path.insert(0, common.ROOT)
import lejit  # noqa: E402

KEYS = {'source_version', 'build', 'trace', 'codestats', 'features',
        'messages', 'layouts'}
LAYOUTS = {'seqiterobject', 'rangeobject', 'rangeiterobject',
           'PyDictKeysObject'}


def main():
    info = lejit._core._build
    problems = []
    if set(info) != KEYS:
        problems.append('keys %s, expected %s' % (sorted(info), sorted(KEYS)))
    if info['source_version'] != sys.version.split()[0]:
        problems.append('source_version %r, the interpreter is %s'
                        % (info['source_version'], sys.version.split()[0]))
    if info['build'] not in ('release', 'checked'):
        problems.append('build %r' % info['build'])
    header = os.path.join(common.build_temp(), 'gen', 'le_features_gen.h')
    if not os.path.exists(header):
        problems.append('no %s' % os.path.relpath(header, common.ROOT))
    else:
        with open(header) as f:
            want = {k: int(v) for k, v in
                    re.findall(r'^#define (LE_HAS_\w+) (\d)$', f.read(), re.M)}
        if dict(info['features']) != want:
            problems.append('features %s, the header says %s'
                            % (dict(info['features']), want))
    header = os.path.join(common.build_temp(), 'gen', 'le_strings_gen.h')
    if not os.path.exists(header):
        problems.append('no %s' % os.path.relpath(header, common.ROOT))
    else:
        with open(header) as f:
            want = set(re.findall(r'^#define (LE_MSG_\w+) "', f.read(), re.M))
        if set(info['messages']) != want:
            problems.append('messages %s, the header has %s'
                            % (sorted(info['messages']), sorted(want)))
    if set(info['layouts']) != LAYOUTS or not all(
            isinstance(v, int) and v > 0 for v in info['layouts'].values()):
        problems.append('layouts %s' % info['layouts'])
    want = set(info['features'].items()) | set(info['messages'].items()) | {
        ('layout:' + k, v) for k, v in info['layouts'].items()}
    got = lejit.build_features()
    if not isinstance(got, frozenset) or got != want:
        problems.append('build_features() %s, lejit._core._build has %s'
                        % (sorted(got ^ want), len(want)))
    for p in problems:
        print('FAIL lejit._core._build: %s' % p)
    if not problems:
        print('lejit._core._build: %d keys, %d features, %d messages, %d layouts,'
              ' as built' % (len(info), len(info['features']),
                             len(info['messages']), len(info['layouts'])))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
