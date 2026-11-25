"""Whether LeJIT runs the same constructs the same way on every CPython
version: a table of small constructs, each run under LeJIT, counting per
code object how often CPython's interpreter (ceval) ran it. A construct
whose code ceval runs on some versions at least twice as often as on
another (usually: there it runs compiled) is a version gap. The known
gaps are listed in parity_gaps.txt, next to this file.

    python tools/parity.py measure OUT.json
        this interpreter's counts (LeJIT built in place, either build)
    python tools/parity.py check DIR
        compare DIR/*.json (one per version) with parity_gaps.txt: fails on
        a gap that isn't listed, and on a listed gap that has closed
    python tools/parity.py test
        check's own test, on made-up counts

The counts come from a PEP 523 hook installed before LeJIT starts, which
LeJIT then passes every frame it doesn't run compiled. They are exact, so
the check gives the same answer on any machine. Times are left out: on
shared machines they vary by up to twice between runs, more than the
cost gaps there are.
"""
import json
import os
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
GAPS = os.path.join(HERE, 'parity_gaps.txt')

CASES = {}


def case(name, src):
    CASES[name] = src


case('loop', '''
def run():
    t = 0
    for i in range(200):
        t += i
''')
case('keyword call', '''
def g(a, b=1, *, c=2): return a + b + c
def run():
    for i in range(100): g(i, b=2, c=3)
''')
case('*args, **kwargs forwarding', '''
def g(a, b): return a + b
def fwd(*args, **kwargs): return g(*args, **kwargs)
def run():
    for i in range(100): fwd(i, b=2)
''')
case('closure', '''
def run():
    k = 3
    h = lambda x: x + k
    for i in range(100): h(i)
''')
case('super().m()', '''
class A:
    def m(self): return 1
class B(A):
    def m(self): return super().m() + 1
def run():
    b = B()
    for i in range(100): b.m()
''')
case('new instance, attribute store', '''
class O:
    def __init__(self, x): self.x = x
def run():
    for i in range(100): O(i).x = i
''')
case('with', '''
class CM:
    def __enter__(self): return self
    def __exit__(self, *a): return False
def run():
    c = CM()
    for i in range(100):
        with c: pass
''')
case('raise and catch', '''
def run():
    for i in range(100):
        try:
            raise KeyError(i)
        except KeyError:
            pass
''')
case('del attribute, del global', '''
class O: pass
def f(o):
    global gv
    o.a = 1
    del o.a
    gv = 1
    del gv
def run():
    o = O()
    for i in range(100): f(o)
''')
case('import in a function', '''
def f():
    import os.path
    from os import sep
    return sep
def run():
    for i in range(100): f()
''')
case('match class pattern', '''
class P:
    __match_args__ = ('x',)
    def __init__(self, x): self.x = x
def f(p):
    match p:
        case P(x=0): return 0
        case P(x): return x
        case _: return -1
def run():
    for i in range(100): f(P(i))
''')
case('genexpr into sum()', '''
def run():
    for i in range(10): sum(x * 2 for x in range(20))
''')
case('genexpr into any()', '''
def run():
    for i in range(10): any(x > 100 for x in range(20))
''')
case('genexpr into list()', '''
def run():
    for i in range(10): list(x for x in range(20))
''')
case('generator in a for loop', '''
def gen(n):
    for i in range(n):
        yield i
        yield -i
def run():
    for i in range(10):
        for x in gen(10): pass
''')
case('generator .send()', '''
def gen():
    t = 0
    while True:
        t += yield t
g = gen(); next(g)
def run():
    for i in range(100): g.send(i)
''')
case('yield from', '''
def inner(n):
    for i in range(n): yield i
def gen(n):
    yield from inner(n)
    yield from inner(n)
def run():
    for i in range(10):
        for x in gen(10): pass
''')
case('yield in try/finally', '''
def gen(n):
    try:
        for i in range(n): yield i
    finally:
        n = 0
def run():
    for i in range(10):
        for x in gen(10): pass
''')
case('yield in with', '''
class CM:
    def __enter__(self): return self
    def __exit__(self, *a): return False
def gen(n):
    with CM():
        for i in range(n): yield i
def run():
    for i in range(10):
        for x in gen(10): pass
''')
case('yield in except', '''
def gen(n):
    for i in range(n):
        try:
            raise KeyError
        except KeyError:
            yield i
def run():
    for i in range(10):
        for x in gen(5): pass
''')
case('generator with 70 locals', '''
def gen(n):
''' + ''.join('    v%d = %d\n' % (i, i) for i in range(70)) + '''
    for i in range(n): yield i + v0 + v69
def run():
    for i in range(5):
        for x in gen(10): pass
''')
case('coroutine awaited at once', '''
async def leaf(x): return x + 1
async def mid(x): return await leaf(x) + await leaf(x)
def run():
    for i in range(50):
        c = mid(i)
        try: c.send(None)
        except StopIteration: pass
''')
case('coroutine that pauses', '''
import types
@types.coroutine
def pause():
    yield
async def leaf(x):
    await pause()
    return x
async def mid(x):
    t = 0
    for i in range(3): t += await leaf(i)
    return t
def run():
    for i in range(20):
        c = mid(i)
        try:
            while True: c.send(None)
        except StopIteration: pass
''')
case('async with, left by return', '''
class ACM:
    async def __aenter__(self): return self
    async def __aexit__(self, *a): return False
async def body(x):
    async with ACM():
        return x
def run():
    for i in range(50):
        c = body(i)
        try: c.send(None)
        except StopIteration: pass
''')
case('async for', '''
class AIt:
    def __init__(self): self.n = 3
    def __aiter__(self): return self
    async def __anext__(self):
        self.n -= 1
        if self.n < 0: raise StopAsyncIteration
        return self.n
async def body():
    t = 0
    async for x in AIt(): t += x
    return t
def run():
    for i in range(30):
        c = body()
        try: c.send(None)
        except StopIteration: pass
''')
case('async generator', '''
async def agen():
    for i in range(3): yield i
async def body():
    return [x async for x in agen()]
def run():
    for i in range(20):
        c = body()
        try: c.send(None)
        except StopIteration: pass
''')
case('except*', '''
def f(i):
    try:
        raise ExceptionGroup("g", [KeyError(i)])
    except* KeyError:
        i = 0
    return i
def run():
    for i in range(30): f(i)
''')
case('calls locals()', '''
def f(a):
    b = a + 1
    return len(locals())
def run():
    for i in range(100): f(i)
''')
case('locals() in a list comprehension', '''
def f(a):
    xs = [len(locals()) for x in range(2)]
    t = 0
    for i in range(a): t += i
    return t
def run():
    for i in range(30): f(5)
''')
case('PEP 695 generic function', '''
def f[T](x: T) -> T: return x
def run():
    for i in range(100): f(i)
''')
case('class body', '''
def run():
    for i in range(30):
        class K:
            a = 1
''')

# the counting hook: frames LeJIT leaves to ceval, per code object
HOOK = r'''
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#ifndef HAVE_SET_EVAL_FRAME_FUNC
#include <frameobject.h>
#if defined(__has_include)
#if __has_include(<internal/pycore_pystate.h>)
#define Py_BUILD_CORE
#include <internal/pycore_pystate.h>
#endif
#endif
#endif

static PyObject *counts;

static PyObject *
code_of(void *frame)
{
#if !defined(HAVE_SET_EVAL_FRAME_FUNC)
    return (PyObject *)((PyFrameObject *)frame)->f_code;
#elif defined(HAVE_FRAME_GET_CODE)
    return (PyObject *)PyUnstable_InterpreterFrame_GetCode(frame);
#else
    /* the first interpreter frames: f_func, f_globals, f_builtins,
       f_locals, f_code */
    return ((PyObject **)frame)[4];
#endif
}

static void
record(PyObject *code)
{
    PyObject *n = PyDict_GetItem(counts, code);
    long c = n ? PyLong_AsLong(n) + 1 : 1;
    PyObject *v = PyLong_FromLong(c);
    if (v != NULL)
        PyDict_SetItem(counts, code, v);
    Py_XDECREF(v);
}

#ifdef HAVE_SET_EVAL_FRAME_FUNC
typedef PyObject *(*eval_fn)(PyThreadState *, void *, int);
static eval_fn original;
static PyObject *
counted(PyThreadState *tstate, void *frame, int throwflag)
{
    PyObject *exc, *val, *tb;
    PyErr_Fetch(&exc, &val, &tb);
    record(code_of(frame));
    PyErr_Restore(exc, val, tb);
    return original(tstate, frame, throwflag);
}
#else
static _PyFrameEvalFunction original;
static PyObject *
counted(PyFrameObject *frame, int throwflag)
{
    PyObject *exc, *val, *tb;
    PyErr_Fetch(&exc, &val, &tb);
    record(code_of(frame));
    PyErr_Restore(exc, val, tb);
    return original(frame, throwflag);
}
#endif

static PyObject *
install(PyObject *self, PyObject *unused)
{
    PyInterpreterState *interp = PyThreadState_Get()->interp;
#ifdef HAVE_SET_EVAL_FRAME_FUNC
    original = (eval_fn)_PyInterpreterState_GetEvalFrameFunc(interp);
    _PyInterpreterState_SetEvalFrameFunc(interp, (void *)counted);
#else
    original = interp->eval_frame;
    interp->eval_frame = counted;
#endif
    Py_INCREF(counts);
    return counts;
}

static PyMethodDef methods[] = {{"install", install, METH_NOARGS, NULL},
                                {NULL}};
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "parity_hook",
                                    NULL, -1, methods};

PyMODINIT_FUNC
PyInit_parity_hook(void)
{
    counts = PyDict_New();
    return counts ? PyModule_Create(&module) : NULL;
}
'''


def build_hook(tmp):
    import ctypes
    import shlex
    import sysconfig
    src = os.path.join(tmp, 'parity_hook.c')
    with open(src, 'w') as f:
        f.write(HOOK)
    flags = []
    if hasattr(ctypes.pythonapi, '_PyInterpreterState_SetEvalFrameFunc'):
        flags.append('-DHAVE_SET_EVAL_FRAME_FUNC')
    if hasattr(ctypes.pythonapi, 'PyUnstable_InterpreterFrame_GetCode'):
        flags.append('-DHAVE_FRAME_GET_CODE')
    so = os.path.join(tmp, 'parity_hook' + sysconfig.get_config_var('EXT_SUFFIX'))
    subprocess.check_call(shlex.split(sysconfig.get_config_var('CC')) +
                          ['-O1', '-fPIC', '-shared', '-I',
                           sysconfig.get_paths()['include']] +
                          flags + [src, '-o', so])
    sys.path.insert(0, tmp)
    import parity_hook
    return parity_hook


def code_names(code):
    """The names of every code object in a case, but its module and run()."""
    stack, names = [code], set()
    while stack:
        co = stack.pop()
        names.add(co.co_name)
        stack += [k for k in co.co_consts if isinstance(k, types.CodeType)]
    return names - {'<module>', 'run'}


def measure(path):
    """{case: {'ceval': {code name: entries}, 'code': [code names]}} for
    this version, leaving out the cases it has no syntax for."""
    sys.path.insert(0, os.path.dirname(HERE))
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        counts = build_hook(tmp).install()
        import lejit
        lejit._core._start()
        for name, src in CASES.items():
            filename = '<%s>' % name
            try:
                code = compile(src, filename, 'exec')
            except SyntaxError:
                continue
            mod = types.ModuleType('case')
            exec(code, mod.__dict__)
            # compiled by now, if it ever will be
            for _ in range(3):
                mod.run()
            counts.clear()
            for _ in range(5):
                mod.run()
            seen = {}
            for co, n in list(counts.items()):
                if co.co_filename == filename:
                    seen[co.co_name] = seen.get(co.co_name, 0) + n
            out[name] = dict(ceval=seen, code=sorted(code_names(code)))
    with open(path, 'w') as f:
        json.dump({'python': '%d.%d' % sys.version_info[:2], 'cases': out}, f,
                  indent=1, sort_keys=True)


def vkey(v):
    return tuple(int(x) for x in v.split('.'))


def gaps_found(results):
    """{(construct, version): what ceval ran there} for every construct
    whose code ceval ran on that version at least twice as often as on
    another version that has the code. results: {version: cases}."""
    found = {}
    for name in sorted({n for cases in results.values() for n in cases}):
        have = [c[name] for c in results.values() if name in c]
        for v, cases in results.items():
            if name not in cases:
                continue
            worse = []
            for q, n in sorted(cases[name]['ceval'].items()):
                fewest = min([o['ceval'].get(q, 0) for o in have
                              if q in o['code']] or [n])
                if n >= 2 * fewest:
                    worse.append('%s %d times, fewest %d' % (q, n, fewest))
            if worse:
                found[(name, v)] = '; '.join(worse)
    return found


def read_gaps(path):
    """[(construct, versions, reason)] from lines 'construct | versions |
    reason', versions written '3.11+', '3.7-3.10' or '3.7, 3.11+'."""
    listed = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                listed.append(tuple(x.strip() for x in line.split('|', 2)))
    return listed


def expand(vers, versions):
    got = set()
    for part in vers.split(','):
        part = part.strip()
        if part.endswith('+'):
            got |= {v for v in versions if vkey(v) >= vkey(part[:-1])}
        elif '-' in part:
            lo, hi = part.split('-')
            got |= {v for v in versions if vkey(lo) <= vkey(v) <= vkey(hi)}
        elif part in versions:
            got.add(part)
    return got


def compare(results, listed):
    """The lines to show, and whether the results match the list."""
    found = gaps_found(results)
    expected = set()
    for name, vers, reason in listed:
        expected |= {(name, v) for v in expand(vers, results)}
    lines, ok = [], True
    for key in sorted(found.keys() | expected,
                      key=lambda k: (k[0], vkey(k[1]))):
        if key in found and key in expected:
            lines.append('known   %s on %s: %s' % (key + (found[key],)))
        elif key in found:
            lines.append('NEW     %s on %s: %s' % (key + (found[key],)))
            ok = False
        else:
            lines.append('CLOSED  %s on %s' % key)
            ok = False
    return lines, ok


def check(d):
    results = {}
    for f in sorted(os.listdir(d)):
        if f.endswith('.json'):
            with open(os.path.join(d, f)) as fh:
                r = json.load(fh)
            results[r['python']] = r['cases']
    lines, ok = compare(results, read_gaps(GAPS))
    print('versions: %s' % ' '.join(sorted(results, key=vkey)))
    print('\n'.join(lines))
    if not ok:
        print('''
NEW: on that version ceval runs code of the construct (at least twice as
often as) where another version runs it compiled. Fix it, or add the gap
to tools/parity_gaps.txt with its reason.
CLOSED: a gap tools/parity_gaps.txt lists has gone. Remove it from there.''')
    return ok


def test():
    def r(ceval, code=('f', 'gen')):
        return dict(ceval=dict(ceval), code=list(code))
    results = {
        '3.7': {'a': r({'gen': 50}), 'b': r({}), 'c': r({'f': 30}),
                'd': r({'f': 5})},
        '3.11': {'a': r({}), 'b': r({}), 'c': r({'f': 10}), 'd': r({'f': 5})},
        '3.12': {'a': r({}), 'b': r({'f': 1}), 'c': r({'f': 10}),
                 'd': r({'f': 5}, code=['gen']), 'e': r({})},
    }
    listed = [('a', '3.7', 'x'), ('b', '3.12+', 'y'), ('c', '3.7-3.10', 'z')]
    # matches the list; d runs in ceval everywhere, so isn't a gap
    lines, ok = compare(results, listed)
    assert ok, lines
    # a gap that isn't listed
    lines, ok = compare(results, listed[1:])
    assert not ok and 'NEW     a on 3.7: gen 50 times, fewest 0' in lines, lines
    # a listed gap that has closed
    results['3.12']['b'] = r({})
    lines, ok = compare(results, listed)
    assert not ok and 'CLOSED  b on 3.12' in lines, lines
    # one that is under twice as often isn't a gap
    results['3.12']['b'] = r({'f': 1})
    results['3.7']['c'] = r({'f': 19})
    lines, ok = compare(results, listed)
    assert not ok and 'CLOSED  c on 3.7' in lines, lines
    # every listed line names a construct this script runs, and versions
    for name, vers, reason in read_gaps(GAPS):
        assert name in CASES, 'parity_gaps.txt: no construct %r' % name
        assert expand(vers, ['3.7', '3.10', '3.11', '3.15']), vers
        assert reason, name
    print('parity.py test: ok')


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'measure':
        measure(sys.argv[2])
    elif cmd == 'check':
        sys.exit(0 if check(sys.argv[2]) else 1)
    elif cmd == 'test':
        test()
    else:
        sys.exit(__doc__)
