# Two threads compile and run new functions while each compile lets the
# GIL go at every allocation it makes (lejit._core._yield_while_compiling),
# so each thread runs inside the other's compiles.
import sys
import threading

SOURCE = '''
def f%d(n):
    t = 0
    for i in range(n):
        t += i * %d
    return [t, str(t)]
'''


def _fresh(k):
    ns = {}
    exec(SOURCE % (k, k), ns)
    return ns['f%d' % k]


def _compiles(first, n, out):
    for k in range(first, first + n):
        out.append(_fresh(k)(10))


def two_threads_compile(n):
    lejit = sys.modules.get('lejit')
    if lejit is not None:
        lejit._core._yield_while_compiling(True)
    try:
        outs = [[], []]
        threads = [threading.Thread(target=_compiles, args=(1000 * i, n, outs[i]))
                   for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
    finally:
        if lejit is not None:
            lejit._core._yield_while_compiling(False)
    return outs, [t.is_alive() for t in threads]


CASES = [
    ('two_threads_compile', [(20,)],
     {'checked': 'calls lejit._core._yield_while_compiling',
      'xfail': "compile state is global: the other thread's compile stops the "
               "process in codemanager.c:le_compile_thread_check",
      'compiles_each_call': 'it makes new functions on every call',
      'objects': "the other thread's allocations depend on timing"}),
]
