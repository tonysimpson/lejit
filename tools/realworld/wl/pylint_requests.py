"""pylint over the requests source tree (from its sdist), one process, no persistence.
The duplicate-code checker runs, but its messages are left out of the digest: which copy of a
repeated block it reports varies from run to run even with a fixed hash seed."""
import hashlib, io, os, sys
from pylint.lint import Run
from pylint.reporters.text import TextReporter

os.chdir(sys.argv[1])          # .../requests-2.34.2/src, so messages carry relative paths
buf = io.StringIO()
Run(['-j1', '--persistent=n', '--disable=import-error', '--score=n', 'requests'], reporter=TextReporter(buf), exit=False)
messages, cur = [], []
for line in buf.getvalue().splitlines():
    if line.startswith(('requests', '*' * 13)) and cur:
        messages.append('\n'.join(cur))
        cur = []
    cur.append(line)
messages.append('\n'.join(cur))
kept = [m for m in messages if '(duplicate-code)' not in m]
print('duplicate-code messages', len(messages) - len(kept), file=sys.stderr)
print('messages', len(kept))
print('report', hashlib.sha256('\n'.join(kept).encode()).hexdigest())
