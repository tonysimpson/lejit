"""Django test runner for tools/thirdparty, chosen by ni_tp_django_settings.

The same records as ni_tp_pytest ($NI_TP_RESULTS, $NI_TP_DESELECT), from
unittest's result callbacks; ids are unittest's test ids.
"""
import atexit, json, os, unittest
from django.test.runner import DiscoverRunner
from ni_tp_pytest import session_record

_out = open(os.environ['NI_TP_RESULTS'], 'a') if 'NI_TP_RESULTS' in os.environ else None
if _out is not None:
    atexit.register(_out.close)


def _write(rec):
    if _out is not None:
        _out.write(json.dumps(rec) + '\n')
        _out.flush()


def _flatten(suite):
    for t in suite:
        if isinstance(t, unittest.TestSuite):
            yield from _flatten(t)
        else:
            yield t


class Result(unittest.TextTestResult):
    def _rec(self, test, outcome, err=None, xfail=False):
        detail = self._exc_info_to_string(err, test) if err else ''
        if len(detail) > 3000:
            detail = detail[:1000] + '\n...\n' + detail[-2000:]
        _write({'id': test.id(), 'when': 'call', 'outcome': outcome,
                'xfail': xfail, 'detail': detail})

    def startTest(self, test):
        _write({'id': test.id(), 'when': 'start'})
        super().startTest(test)

    def addSuccess(self, test):
        super().addSuccess(test)
        self._rec(test, 'passed')

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._rec(test, 'failed', err)

    def addError(self, test, err):
        super().addError(test, err)
        self._rec(test, 'error', err)

    def addSubTest(self, test, subtest, err):
        super().addSubTest(test, subtest, err)
        if err is not None:
            self._rec(test, 'failed', err)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._rec(test, 'skipped')

    def addExpectedFailure(self, test, err):
        super().addExpectedFailure(test, err)
        self._rec(test, 'skipped', xfail=True)

    def addUnexpectedSuccess(self, test):
        super().addUnexpectedSuccess(test)
        self._rec(test, 'passed', xfail=True)


class Runner(DiscoverRunner):
    def get_resultclass(self):
        return Result

    def run_suite(self, *args, **kwargs):
        try:
            return super().run_suite(*args, **kwargs)
        finally:
            _write(session_record())

    def build_suite(self, *args, **kwargs):
        suite = super().build_suite(*args, **kwargs)
        path = os.environ.get('NI_TP_DESELECT')
        if not path:
            return suite
        with open(path) as f:
            drop = set(line.rstrip('\n') for line in f)
        return unittest.TestSuite(t for t in _flatten(suite)
                                  if t.id() not in drop)
