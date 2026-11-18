"""pytest plugin for tools/thirdparty: loaded with -p ni_tp_pytest.

Writes one JSON line per test phase to $NI_TP_RESULTS as it happens (and a
'start' line before each test), so a run killed by a crash or a timeout
still says which test it was in. Tests named in $NI_TP_DESELECT (one id per
line) are dropped, so a rerun can skip what has already run. Hypothesis is
made deterministic and given no deadline, since compiling makes first calls
slow. At the end, Ni's code-buffer count says that it really ran.
"""
import json, os, sys

_out = None


def _write(rec):
    if _out is not None:
        _out.write(json.dumps(rec) + '\n')
        _out.flush()


def pytest_configure(config):
    global _out
    path = os.environ.get('NI_TP_RESULTS')
    if path:
        _out = open(path, 'a')
    # pytest-cov's marker, which dateutil's conftest adds
    config.addinivalue_line('markers', 'no_cover: not measured')
    try:
        from hypothesis import settings, HealthCheck
    except ImportError:
        return
    settings.register_profile('ni_tp', settings(
        settings(), deadline=None, derandomize=True, database=None,
        suppress_health_check=list(HealthCheck)))
    settings.load_profile('ni_tp')


def session_record():
    ni = sys.modules.get('ni')
    return {'id': '<session>', 'when': 'session',
            'ni_buffers': ni._stats()['code_buffers'] if ni else None}


def pytest_unconfigure(config):
    global _out
    _write(session_record())
    if _out is not None:
        _out.close()
        _out = None


def pytest_collection_modifyitems(session, config, items):
    path = os.environ.get('NI_TP_DESELECT')
    if not path:
        return
    with open(path) as f:
        drop = set(line.rstrip('\n') for line in f)
    keep = [it for it in items if it.nodeid not in drop]
    config.hook.pytest_deselected(
        items=[it for it in items if it.nodeid in drop])
    items[:] = keep


def pytest_runtest_logstart(nodeid, location):
    _write({'id': nodeid, 'when': 'start'})


def _detail(report):
    if report.passed:
        return ''
    text = str(report.longrepr)
    return text if len(text) < 3000 else text[:1000] + '\n...\n' + text[-2000:]


def pytest_runtest_logreport(report):
    _write({'id': report.nodeid, 'when': report.when,
            'outcome': report.outcome, 'xfail': hasattr(report, 'wasxfail'),
            'detail': _detail(report)})


def pytest_collectreport(report):
    if report.failed:
        _write({'id': report.nodeid, 'when': 'collect', 'outcome': 'failed',
                'xfail': False, 'detail': _detail(report)})
