"""Diagnostic only: times test_signal's two stress tests.

    python tests/cpython/signal_timing.py plain|lejit [repeats]

runs them on their own; tests/cpython/run.py also calls wrap() for its
test_signal runs when LE_SIGNAL_TIMING names a file to append to.
"""
import os, resource, sys, time

TESTS = ('test_stress_delivery_dependent', 'test_stress_delivery_simultaneous')
BUCKETS = (20e-6, 100e-6, 1e-3, 10e-3, 100e-3, 1.0)


def _report(out, mode, name, res, info, wall, cpu, ru0, ru1, events):
    gaps = []
    for i in range(1, len(events)):
        gaps.append((events[i][0] - events[i - 1][0], i))
    hist = [0] * (len(BUCKETS) + 1)
    for g, _ in gaps:
        k = 0
        while k < len(BUCKETS) and g >= BUCKETS[k]:
            k += 1
        hist[k] += 1
    top = sorted(gaps, reverse=True)[:5]
    in_handler = sum(e[1] for e in events)
    lines = ['%s %s %s N=%s reso=%s wall=%.2fs cpu=%.2fs handlers=%d '
             'handler_time=%.3fs vcsw=%d ivcsw=%d' % (
                 mode, name.split('_')[-1], res, info.get('N'),
                 '%.1fus' % (info['reso'] * 1e6) if 'reso' in info else '?',
                 wall, cpu, len(events), in_handler,
                 ru1.ru_nvcsw - ru0.ru_nvcsw, ru1.ru_nivcsw - ru0.ru_nivcsw),
             '    gaps between handlers <20us:%d <100us:%d <1ms:%d <10ms:%d '
             '<100ms:%d <1s:%d >=1s:%d' % tuple(hist)]
    for g, i in top:
        lines.append('    gap %.6fs before handler %d (sig %d, in %s)' % (
            g, i, events[i][2], events[i][3]))
    if events:
        lines.append('    last handler %.3fs after start, test ended %.3fs after'
                     ' start' % (events[-1][0] - info['t0'], wall))
    out.write('\n'.join(lines) + '\n')
    out.flush()


def wrap(mod, mode, out):
    """Wraps mod.StressTest's two tests to write their timings to out."""
    T = mod.StressTest
    info = {}
    events = []
    orig_measure = T.measure_itimer_resolution
    orig_setsig = T.setsig

    def measure(self):
        r = orig_measure(self)
        info['reso'] = r
        return r

    def decide(self, orig=T.decide_itimer_count):
        n = orig(self)
        info['N'] = n
        info['t_loop'] = time.perf_counter()
        return n

    def setsig(self, signum, handler):
        def timed(signum, frame=None):
            t = time.perf_counter()
            try:
                return handler(signum, frame)
            finally:
                where = '%s:%s' % (frame.f_code.co_name, frame.f_lineno) \
                    if frame is not None and not os.environ.get('LE_NO_FRAME') \
                    else '?'
                events.append((t, time.perf_counter() - t, signum, where))
        if handler.__name__ in ('handler', 'second_handler') and \
                handler.__code__.co_argcount == 2:
            return orig_setsig(self, signum, timed)
        return orig_setsig(self, signum, handler)

    T.measure_itimer_resolution = measure
    T.decide_itimer_count = decide
    T.setsig = setsig
    for name in TESTS:
        f = getattr(T, name)

        def run(self, f=f, name=name):
            info.clear()
            del events[:]
            res = 'ok'
            ru0 = resource.getrusage(resource.RUSAGE_SELF)
            info['t0'] = time.perf_counter()
            c0 = time.process_time()
            try:
                f(self)
            except BaseException as e:
                res = 'FAIL(%s)' % str(e).split(' :')[0]
                raise
            finally:
                wall = time.perf_counter() - info['t0']
                cpu = time.process_time() - c0
                ru1 = resource.getrusage(resource.RUSAGE_SELF)
                # the measuring handler's events come first: keep only the
                # test's own
                _report(out, mode, name, res, info, wall, cpu, ru0, ru1,
                        [e for e in events if e[0] >= info.get('t_loop', 0)])
        setattr(T, name, run)


if __name__ == '__main__':
    mode = sys.argv[1]
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if mode == 'lejit':
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))
        import lejit
        lejit.start()
    import io, unittest
    from test import test_signal
    wrap(test_signal, mode, sys.stdout)
    for _ in range(repeats):
        for name in TESTS:
            unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(
                test_signal.StressTest(name))
