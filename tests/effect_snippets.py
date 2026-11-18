#!/usr/bin/env python3
"""Snippet tests for the effect analysis (tools/metac/effects.py): small C
in, the flags that hold out. Needs gcc and no build; runs in a second.

    effect_snippets.py

Exits 1 if any snippet gives other flags, naming it.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'metac'))
import effects  # noqa: E402
from pycparser import c_parser  # noqa: E402

RULES = effects.Rules(
    seeds={'Include/object.h:Py_DECREF': 'decref',
           't.c:PyErr_SetObject': 'sets_error',
           't.c:PyEval_SaveThread': 'blocks'},
    assumptions={'t.c:lookup': {'no_python'}},
    external={'fabs': 'pure', 'strlen': 'pure'},
    no_del={'PyFloatObject'})

HEAD = '''
typedef struct { long ob_refcnt; void *ob_type; } PyObject;
typedef struct { PyObject ob_base; long ob_size; } PyListObject;
typedef struct { PyObject ob_base; double ob_fval; } PyFloatObject;
typedef struct { void (*call)(void); } Slots;
extern PyObject _Py_NoneStruct;
void __ni_vocab_Py_DECREF();
void (*hook)(void);
void PyErr_SetObject(PyObject *e) { hook(); }
void PyEval_SaveThread(void) { hook(); }
void lookup(void) { hook(); }
'''
LIBC = '''
# 1 "/usr/include/libc.h"
double fabs(double);
unsigned long strlen(const char *);
void *memset(void *, int, unsigned long);
# 1 "%s"
'''

ALL = set(effects.FLAGS)
NONE = set()
NOT_PURE = ALL - {'pure'}
LIBC_CALL = ALL - {'pure', 'readonly'}

# (what it shows, function, C, the flags that hold)
CASES = [
    ('pure', 'add', 'int add(int a, int b) { int x = a; x++; return x + b; }', ALL),
    ('pure: const data', 'k', 'static const int t[2] = {1, 2}; int k(int i) { return t[i]; }', ALL),
    ('G3: a read through a pointer is not pure', 'size',
     'long size(PyListObject *o) { return o->ob_size; }', NOT_PURE),
    ('G3: a read of a mutable global is not pure', 'get',
     'static int counter; int get(void) { return counter; }', NOT_PURE),
    ('readonly: a write through a pointer', 'set', 'void set(int *p) { *p = 1; }',
     ALL - {'readonly'}),
    ('readonly: a write to a global', 'bump', 'static int n; void bump(void) { n = 1; }',
     ALL - {'readonly'}),
    ('readonly: refcounts aside', 'incref',
     'PyObject *incref(PyObject *o) { o->ob_refcnt++; return o; }', ALL),
    ('no_python: a decref may run __del__', 'drop',
     'void drop(PyObject *o) { __ni_vocab_Py_DECREF(o); }', {'no_raise'}),
    ('no_python: a decref of a no-__del__ type or a singleton', 'drop2',
     'void drop2(PyFloatObject *f) { __ni_vocab_Py_DECREF(f);'
     ' __ni_vocab_Py_DECREF(&_Py_NoneStruct); }', ALL),
    ('no_block: releasing the GIL', 'sleep', 'void sleep(void) { PyEval_SaveThread(); }',
     ALL - {'pure', 'readonly', 'no_block'}),
    ('no_raise: setting an exception', 'fail',
     'void fail(PyObject *e) { PyErr_SetObject(e); }', {'no_python', 'no_block'}),
    ('an assumption holds whatever the body does', 'lookup', '', {'no_python'}),
    ('G1: a global hook', 'g1a', 'void g1a(void) { hook(); }', NONE),
    ('G1: a typedef\'d local', 'g1b',
     'typedef int (*binaryfunc)(int); int g1b(binaryfunc slot) { return slot(1); }', NONE),
    ('G1: (f)()', 'g1c', 'int g1c(int (*f)(void)) { return (f)(); }', NONE),
    ('G1: a struct slot', 'g1d', 'void g1d(Slots *s) { s->call(); }', NONE),
    ('a function\'s own name through *', 'two',
     'static int one(void) { return 1; } int two(void) { return (*one)(); }', ALL),
    ('a function named without a call is a callee', 'giver',
     'static void bad(void) { hook(); } void (*giver(void))(void) { return bad; }', NONE),
    ('G1: a callee in no file read', 'u', 'int ext(int); int u(void) { return ext(1); }',
     NONE),
    ('libc runs no Python', 'clear', LIBC + 'void clear(char *p) { memset(p, 0, 4); }',
     LIBC_CALL),
    ('libc: EXTERNAL pure', 'absval', LIBC + 'double absval(double x) { return fabs(x); }',
     ALL),
    ('libc: EXTERNAL pure, reading through a pointer', 'len',
     LIBC + 'unsigned long len(const char *s) { return strlen(s); }', NOT_PURE),
]

# through gcc -E: macro aliases are expanded, only the compiled #if arm counts
GCC_CASES = [
    ('G2: a macro alias is the function it names', 'caller',
     '#define PyObject_Length PyObject_Size\n'
     'int PyObject_Size(void) { hook(); return 0; }\n'
     'int caller(void) { return PyObject_Length(); }\n', NONE),
    ('G4: the compiled #if arm, whatever the file order', 'pick',
     '#ifdef NOT_DEFINED\nint pick(void) { return 1; }\n'
     '#else\nint pick(void) { hook(); return 0; }\n#endif\n', NONE),
    ('G4: the other way round', 'pick',
     '#ifndef NOT_DEFINED\nint pick(void) { return 1; }\n'
     '#else\nint pick(void) { hook(); return 0; }\n#endif\n', ALL),
]


def run(src, code, gcc):
    path = os.path.join(src, 't.c')
    text = HEAD + code.replace('%s', path)
    if gcc:
        with open(path, 'w') as f:
            f.write(text)
        text = subprocess.run(['gcc', '-E', path], stdout=subprocess.PIPE, check=True,
                              universal_newlines=True, timeout=60).stdout
    else:
        text = '# 1 "%s"\n' % path + text
    g = effects.Effects(src, RULES)
    g.add('t.c', c_parser.CParser().parse(text, path))
    g.solve()
    return g


def main():
    failed = 0
    with tempfile.TemporaryDirectory() as src:
        src = os.path.realpath(src)
        for gcc, cases in ((False, CASES), (True, GCC_CASES)):
            for what, name, code, want in cases:
                got = set(run(src, code, gcc).flags('t.c:' + name) or ())
                if got != want:
                    failed += 1
                    print('FAIL %s: %s holds %s, expected %s'
                          % (what, name, sorted(got), sorted(want)))
        g = run(src, CASES[0][2] + '\n' + CASES[2][2], False)
        if g.cf_flags('t.c:add') != ['CfPure', 'CfNoBlock'] or \
                g.cf_flags('t.c:size') != ['CfNoPython', 'CfNoBlock']:
            failed += 1
            print('FAIL Cf flags: add %s, size %s' % (g.cf_flags('t.c:add'),
                                                       g.cf_flags('t.c:size')))
    n = len(CASES) + len(GCC_CASES) + 1
    print('%d of %d effect snippets pass' % (n - failed, n))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
