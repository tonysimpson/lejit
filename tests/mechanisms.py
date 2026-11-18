"""Mechanism detectors (tools/metac/mechanisms.py): run under the
generators' Python (3.10 or later, NI_GEN_PYTHON), not the one Ni is built
for.

    python3 tests/mechanisms.py

1. The detectors give the expected answers on each CPython source in
   EXPECTED, fetched into the cache like a build's (cpython_src.py).
   Every mechanism VARIANTS names must have an expected row.
2. Answers that VARIANTS doesn't map fail generation, through the build's
   own entry point, and remove the headers of the last good generation: a
   copy of the 3.7 source given a Python/instrumentation.c says tracing is
   both per thread and through sys.monitoring.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
METAC = os.path.join(ROOT, 'tools', 'metac')
sys.path.insert(0, METAC)
import cpython_src  # noqa: E402
import mechanisms  # noqa: E402

PY37 = os.environ.get('NI_PY37', os.path.expanduser(
    '~/.pyenv/versions/3.7.17/bin/python3.7'))

# version -> {mechanism: (variant, answers in VARIANTS' detector order)}
EXPECTED = {
    '3.7.17': {
        'entry': ('frame_eval_hook', ('frame_object', 'no', 'no', 'none')),
        'tracing': ('tstate_use_tracing', ('tstate', 'no')),
        'frames': ('frame_object', ('frame_object', 'tstate.frame')),
        'eval_breaker_runtime': ('ceval_loop_top', ('runtime', 'loop_top', 'no')),
        'eval_breaker': ('int32_address', ('runtime',)),
        'tstate_runtime': ('runtime_address', ('runtime_gilstate',)),
        'tstate': ('runtime_address', ('runtime_gilstate',)),
        'recursion': ('depth_up', ('depth_up', 'global', 'overflowed')),
        'exceptions': ('curexc_triple', ('triple', 'triple')),
        'store_gc': ('gc_refs', ('gc_refs',))},
    '3.8.20': {
        'entry': ('frame_eval_hook', ('frame_object', 'no', 'no', 'none')),
        'tracing': ('tstate_use_tracing', ('tstate', 'no')),
        'frames': ('frame_object', ('frame_object', 'tstate.frame')),
        'eval_breaker_runtime': ('ceval_loop_top_signals', ('runtime', 'loop_top', 'yes')),
        'eval_breaker': ('int32_address', ('runtime',)),
        'tstate_runtime': ('runtime_address', ('runtime_gilstate',)),
        'tstate': ('runtime_address', ('runtime_gilstate',)),
        'recursion': ('depth_up', ('depth_up', 'global', 'overflowed')),
        'exceptions': ('curexc_triple', ('triple', 'triple')),
        'store_gc': ('gc_next', ('gc_next',))},
    '3.11.11': {
        'entry': ('frame_eval_hook_interp',
                  ('interpreter_frame', 'no', 'no', 'yes')),
        'tracing': ('cframe_use_tracing', ('cframe', 'no')),
        'frames': ('interpreter_frame_cframe',
                   ('interpreter_frame', 'cframe.current_frame')),
        'eval_breaker_runtime': ('static_handler_copy', ('interpreter', 'static_function', 'yes')),
        'eval_breaker': ('int32_address', ('interpreter',)),
        'tstate_runtime': ('runtime_address', ('runtime_gilstate',)),
        'tstate': ('runtime_address', ('runtime_gilstate',)),
        'recursion': ('remaining_down', ('remaining_down', 'tstate', 'headroom')),
        'exceptions': ('curexc_triple_info_value', ('triple', 'value')),
        'store_gc': ('gc_next', ('gc_next',))},
    '3.12.9': {
        'entry': ('vectorcall', ('interpreter_frame', 'yes', 'yes', 'no')),
        'tracing': ('monitoring', ('none', 'yes')),
        'frames': ('interpreter_frame_cframe',
                   ('interpreter_frame', 'cframe.current_frame')),
        'eval_breaker_runtime': ('internal_handler_copy', ('interpreter', 'internal_function', 'yes')),
        'eval_breaker': ('int32_address', ('interpreter',)),
        'tstate_runtime': ('thread_local', ('thread_local',)),
        'tstate': ('thread_local', ('thread_local',)),
        'recursion': ('py_remaining_down', ('py_remaining_down', 'tstate', 'headroom')),
        'exceptions': ('current_exception', ('one', 'value')),
        'store_gc': ('gc_next', ('gc_next',))},
    '3.13.2': {
        'entry': ('vectorcall', ('interpreter_frame', 'yes', 'yes', 'no')),
        'tracing': ('monitoring', ('none', 'yes')),
        'frames': ('interpreter_frame',
                   ('interpreter_frame', 'tstate.current_frame')),
        'eval_breaker_runtime': ('handler_call', ('thread', 'api_function', 'no')),
        'eval_breaker': ('tstate_word', ('thread',)),
        'tstate_runtime': ('thread_local', ('thread_local',)),
        'tstate': ('thread_local', ('thread_local',)),
        'recursion': ('py_remaining_down', ('py_remaining_down', 'tstate', 'headroom')),
        'exceptions': ('current_exception', ('one', 'value')),
        'store_gc': ('gc_next', ('gc_next+ob_gc_bits',))},
}


def check_answers():
    bad = 0
    for version, expected in EXPECTED.items():
        missing = sorted(set(mechanisms.VARIANTS) - set(expected))
        if missing:
            print('FAIL %-8s no expected answers for %s' % (version, missing))
            bad += 1
        got = mechanisms.detect(cpython_src.find_source(version))
        for mech, (variant, answers) in expected.items():
            found = (got[mech][1], tuple(a for _, a, _ in got[mech][0]))
            ok = found == (variant, answers)
            bad += not ok
            print('%-4s %-8s %-8s %s' % ('ok' if ok else 'FAIL', version,
                                         mech, found[0]))
            if not ok:
                print('     expected %r, got %r' % ((variant, answers), found))
    return bad


def check_snippets():
    """Every variant VARIANTS names has its file; one not written yet stops
    the C compiler with a message saying so."""
    bad = 0
    for mech, (_, table) in mechanisms.VARIANTS.items():
        for variant in table.values():
            path = os.path.join(ROOT, 'ni', 'mechanisms', mech, variant + '.h')
            if not os.path.isfile(path):
                print('FAIL %s is missing' % os.path.relpath(path, ROOT))
                bad += 1
                continue
            with open(path) as f:
                text = f.read()
            if '#error' in text and 'not written yet' not in text:
                print('FAIL %s: #error without "not written yet"' % path)
                bad += 1
    print('%-4s every variant has a snippet file' % ('ok' if not bad else 'FAIL'))
    return bad


def check_unknown_fails(tmp):
    """Generation from a source whose answers aren't mapped must fail,
    say which mechanism and answers, and leave no headers behind."""
    real = cpython_src.find_source('3.7.17')
    fake = os.path.join(tmp, 'src')
    os.makedirs(fake)
    for name in os.listdir(real):
        if name != 'Python':
            os.symlink(os.path.join(real, name), os.path.join(fake, name))
    shutil.copytree(os.path.join(real, 'Python'), os.path.join(fake, 'Python'))
    with open(os.path.join(fake, 'Python', 'instrumentation.c'), 'w') as f:
        f.write('/* sys.monitoring, as far as the detector can tell */\n')
    out = os.path.join(tmp, 'gen')
    gen = [sys.executable, os.path.join(METAC, 'generate.py'),
           '--python', PY37, '--out', out]
    good = subprocess.run(gen + ['--src', real], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, universal_newlines=True,
                          timeout=600)
    if good.returncode != 0:
        print('FAIL the real 3.7 source did not generate:\n' + good.stdout)
        return 1
    r = subprocess.run(gen + ['--src', fake], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True,
                       timeout=600)
    left = sorted(os.listdir(out)) if os.path.isdir(out) else []
    said = "mechanism 'tracing': no snippet for these answers" \
        " (tracing_flag=tstate, monitoring=yes)"
    ok = r.returncode != 0 and said in r.stdout and not left
    print('%-4s unknown answers fail generation and remove the headers'
          % ('ok' if ok else 'FAIL'))
    if not ok:
        print('     exit %d, headers left %s, output:\n%s'
              % (r.returncode, left, r.stdout))
    return not ok


def main():
    if sys.version_info < (3, 10):
        sys.exit('run under the generators\' Python (3.10 or later)')
    bad = check_answers() + check_snippets()
    tmp = tempfile.mkdtemp(prefix='mechanisms-', dir=os.path.join(ROOT, 'build'))
    try:
        bad += check_unknown_fails(tmp)
    finally:
        shutil.rmtree(tmp)
    print('%d failed' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
