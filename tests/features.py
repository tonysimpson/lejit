"""Feature detectors (tools/metac/features.py): run under the generators'
Python (3.10 or later, NI_GEN_PYTHON).

    python3 tests/features.py

Each source in EXPECTED, fetched into the cache like a build's
(cpython_src.py), gives the expected value for every feature. 3.7.13 and
3.7.17 are the same minor version but differ in the int-to-str digit limit,
backported in 3.7.14: a version number can't tell that, the source does.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'metac'))
import cpython_src  # noqa: E402
import features  # noqa: E402

V37 = {'PYDICT_NEWPRESIZED': 1, 'INT_MAX_STR_DIGITS': 1, 'VECTORCALL': 0,
       'POSONLYARGS': 0, 'AUDIT_HOOKS': 0, 'ARG_CHECK_POSITIONAL': 0,
       'TSTATE_ERRORS': 0, 'DICT_NEW_EMPTY_KEYS': 0,
       'FLOAT_ASDOUBLE_INDEX': 0, 'MAP_ADD_VALUE_ON_TOP': 0,
       'PYCORE_PYSTATE_H': 0, 'OPCODE_SETUP_LOOP': 1,
       'OPCODE_SETUP_EXCEPT': 1, 'OPCODE_BREAK_LOOP': 1,
       'OPCODE_CONTINUE_LOOP': 1, 'OPCODE_BEGIN_FINALLY': 0,
       'OPCODE_CALL_FINALLY': 0, 'OPCODE_POP_FINALLY': 0,
       'OPCODE_END_ASYNC_FOR': 0, 'OPCODE_ROT_FOUR': 0}
V38 = dict(V37, VECTORCALL=1, POSONLYARGS=1, AUDIT_HOOKS=1,
           ARG_CHECK_POSITIONAL=1, TSTATE_ERRORS=1, DICT_NEW_EMPTY_KEYS=1,
           FLOAT_ASDOUBLE_INDEX=1, MAP_ADD_VALUE_ON_TOP=1, PYCORE_PYSTATE_H=1,
           OPCODE_SETUP_LOOP=0, OPCODE_SETUP_EXCEPT=0, OPCODE_BREAK_LOOP=0,
           OPCODE_CONTINUE_LOOP=0, OPCODE_BEGIN_FINALLY=1,
           OPCODE_CALL_FINALLY=1, OPCODE_POP_FINALLY=1,
           OPCODE_END_ASYNC_FOR=1, OPCODE_ROT_FOUR=1)
EXPECTED = {
    '3.7.13': dict(V37, INT_MAX_STR_DIGITS=0),
    '3.7.17': V37,
    '3.8.20': V38,
}


def main():
    failed = 0
    names = {name for name, _ in features.FEATURES}
    for version, want in EXPECTED.items():
        if set(want) != names:
            print('FAIL %s: EXPECTED and FEATURES differ in %s'
                  % (version, sorted(set(want) ^ names)))
            failed += 1
            continue
        got = {name[len('NI_HAS_'):]: value for name, value, _, _ in
               features.detect(cpython_src.find_source(version))}
        bad = sorted(k for k in want if got[k] != want[k])
        for k in bad:
            print('FAIL %s: NI_HAS_%s is %d, expected %d' % (version, k, got[k], want[k]))
        failed += len(bad)
        if not bad:
            print('ok %s: %d features' % (version, len(got)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
