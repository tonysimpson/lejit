"""Run one pyperformance benchmark warm under Ni for a while, for a profiler:

    perf record -F 4999 -g -o richards.data \
        python3.7 tools/profile_bench.py richards 4
    perf report -i richards.data --no-children --sort sym -g none

Needs kernel.perf_event_paranoid <= 2 (Debian's default is 3:
sudo sysctl kernel.perf_event_paranoid=1, until reboot). Emitted code shows
as [JIT]. Use a release build.
"""
import os
import sys
import time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pyperf37
import ni
name, secs = sys.argv[1], float(sys.argv[2])
benches = pyperf37.load(os.path.join(ROOT, 'tools', 'pyperformance', 'bm_%s.py' % name))
_, timed = benches[0]
timed(3)                    # compile
t = time.perf_counter()
n = 0
while time.perf_counter() - t < secs:
    timed(1); n += 1
print(name, n, 'iterations')
