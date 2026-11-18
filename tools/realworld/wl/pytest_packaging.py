"""pytest over packaging's own test suite (from its sdist). Prints the outcome of every test."""
import hashlib, io, sys
import pytest

tests = sys.argv[1]
outcomes = []


class Record:
    def pytest_runtest_logreport(self, report):
        if report.when == 'call' or report.outcome != 'passed':
            outcomes.append('%s %s %s' % (report.nodeid, report.when, report.outcome))


real = sys.stdout
sys.stdout = io.StringIO()
try:
    code = pytest.main(['-q', '-p', 'no:cacheprovider', '--hypothesis-seed=0', tests], plugins=[Record()])
finally:
    sys.stdout = real
outcomes.sort()
counts = {}
for o in outcomes:
    counts[o.rsplit(' ', 1)[1]] = counts.get(o.rsplit(' ', 1)[1], 0) + 1
print('exit', int(code), 'outcomes', sorted(counts.items()))
print('tests', hashlib.sha256('\n'.join(outcomes).encode()).hexdigest())
