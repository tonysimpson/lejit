#!/usr/bin/env python3
"""Snippet tests for the new translator (tools/metac/gen2): small C in,
the generated meta (or the refusal) out. The C is read after
Objects/abstract.c, in its scope, so it has CPython 3.7's types (a
number test's after Objects/longobject.c or Objects/floatobject.c, with
the word and double representations, a tuple test's after
Objects/tupleobject.c with the known-length one); a test's functions are translated
as a family whose metas Ni declares as the test says. Needs gcc and
CPython 3.7's source; no build.

    gen2_snippets.py [NAME...]

Exits 1 if any snippet gives another result, naming it.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'metac'))
import front  # noqa: E402
from gen2 import abstract, conventions, policy, scope, types  # noqa: E402
from gen2.policy import Holds, IfKnown, Likely, Restype  # noqa: E402

PY37 = os.environ.get('NI_PY37', os.path.expanduser('~/.pyenv/versions/3.7.17/bin/python3.7'))


class Case:
    """C, the functions whose metas Ni declares (roots), policy items,
    and what must come out: `want` in order in the named function's meta,
    `absent` nowhere in it, or `refused` in its refusal; `report` in its
    resume lines, `copy` in its run-time copies (`absent_copy` not)."""

    def __init__(self, name, c, roots, fn=None, items=None, want=(), absent=(),
                 refused=None, report=None, copy=(), file='abstract', absent_copy=(),
                 methods=()):
        self.name, self.c, self.roots = name, c, roots
        self.file = 'bltin' if methods else file
        self.methods = methods      # builtins: [(Python name, C function, ml_flags)]
        self.fn = fn or roots[0]
        self.items = items or {}
        self.want, self.absent, self.refused, self.report = want, absent, refused, report
        self.copy, self.absent_copy = copy, absent_copy


CASES = [
    # ---- passes ----
    Case('liveness: an owned value is released after its last use', '''
PyObject *PySnip_Live(PyObject *o) {
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    PyObject *d = PyObject_Dir(r);
    Py_DECREF(r);
    return d;
}''', ['PySnip_Live'], want=['r = _t1;', 'if (r == NULL)', 'PyObject_Dir,', '"v", r);',
                           'd = _t2;', 'vinfo_decref(r, po);', 'return d;']),
    Case('binding times: a static scalar is meta when every caller passes one', '''
static PyObject *snip_bt(PyObject *o, int k) { return PyNumber_Add(o, o); }
PyObject *PySnip_Bt(PyObject *o) { return snip_bt(o, 3); }''', ['PySnip_Bt'],
         fn='snip_bt', want=['snip_bt(PsycoObject *po, vinfo_t *o, int k)']),
    Case('binding times: a run-time argument makes it dynamic', '''
static PyObject *snip_bt2(PyObject *o, Py_ssize_t k) { return PyNumber_Add(o, o); }
PyObject *PySnip_Bt2(PyObject *o, Py_ssize_t i) { return snip_bt2(o, i); }''', ['PySnip_Bt2'],
         fn='snip_bt2', want=['snip_bt2(PsycoObject *po, vinfo_t *o, vinfo_t *k)']),
    Case('promotion placement: a type read before any call is promoted at entry', '''
PyObject *PySnip_Promote(PyObject *o) {
    if (o->ob_type->tp_as_number == NULL) return PyObject_Repr(o);
    return PyNumber_Negative(o);
}''', ['PySnip_Promote'], want=['tp_o = Psyco_NeedType(po, o);', 'if (tp_o == NULL)',
                               '(tp_o->tp_as_number) == NULL']),
    Case('promotion placement: a type read after a call moves to the entry', '''
PyObject *PySnip_Late(PyObject *o, PyObject *w) {
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    if (w->ob_type->tp_as_number == NULL) return r;
    Py_DECREF(r);
    return PyNumber_Negative(w);
}''', ['PySnip_Late'], want=['tp_w = Psyco_NeedType(po, w);', 'PyObject_Repr,',
                            '(tp_w->tp_as_number) == NULL']),
    # ---- actions ----
    Case('fold: C of meta values is computed by the meta', '''
static PyObject *snip_fold(PyObject *o, const int k) {
    if (k + 1 == 3) return PyNumber_Negative(o);
    return PyNumber_Positive(o);
}
PyObject *PySnip_Fold(PyObject *o) { return snip_fold(o, 2); }''', ['PySnip_Fold'],
         fn='snip_fold', want=['if (((k + 1)) == 3)']),
    Case("map: a call of a CPython function Ni has a meta of calls the meta", '''
PyObject *PySnip_Map(PyObject *o, PyObject *w) { return PyObject_GetAttr(o, w); }''',
         ['PySnip_Map'], want=['PsycoObject_GetAttr(po, o, w)'], absent=['psyco_generic_call']),
    Case('residual: any other function is called at run time, with its flags', '''
PyObject *PySnip_Res(PyObject *o) { return PyObject_Dir(o); }''', ['PySnip_Res'],
         want=['psyco_generic_call(po, PyObject_Dir, CfCommonNewRefPyObject, "v", o)']),
    Case("meta call: a call of the family's own function is a call of its meta", '''
static PyObject *snip_callee(PyObject *o) { return PyNumber_Negative(o); }
PyObject *PySnip_Meta(PyObject *o) { return snip_callee(o); }''', ['PySnip_Meta'],
         want=['snip_callee(po, o)'], absent=['psyco_generic_call']),
    Case('guard: a slot is read through Psyco_TypeSlot; a change resumes', '''
PyObject *PySnip_Guard(PyObject *o) {
    PyNumberMethods *m = o->ob_type->tp_as_number;
    if (m && m->nb_negative) return m->nb_negative(o);
    return PyObject_Repr(o);
}''', ['PySnip_Guard'], want=['switch (Psyco_TypeSlot(po, tp_o, &m->nb_negative, (void **)&_t1))',
                             'case 1:', 'break;', 'case 0:', 'psyco_generic_call(po, PySnip_Guard',
                             'default:', 'return NULL;', 'Psyco_META1(po, _t1,'],
         report='a slot of a heap type changed'),
    Case('guard: PyType_IsSubtype reads the mro through Ni', '''
PyObject *PySnip_Sub(PyObject *v, PyObject *w) {
    if (PyType_IsSubtype(w->ob_type, v->ob_type)) return PyObject_Repr(w);
    return PyObject_Repr(v);
}''', ['PySnip_Sub'], want=['Psyco_TypeIsSubtype(po, tp_w, tp_v, &_t1)']),
    Case('raise: PyErr_Format at compile time, and the error return', '''
PyObject *PySnip_Raise(PyObject *o) {
    PyErr_Format(PyExc_TypeError, "no %.200s", o->ob_type->tp_name);
    return NULL;
}''', ['PySnip_Raise'], want=['PycException_SetFormat(po, PyExc_TypeError, "no %.200s", '
                             'tp_o->tp_name);', 'return NULL;']),
    Case('resume at once: nothing called yet, CPython\'s function itself', '''
PyObject *PySnip_Once(PyObject *o) {
    PyObject *a[2] = {o, o};
    return PyObject_Repr(a[0]);
}''', ['PySnip_Once'], want=['return psyco_generic_call(po, PySnip_Once,'],
         report='an array a'),
    Case('resume later: a copy of the rest, given what is live and owned', '''
PyObject *PySnip_Later(PyObject *o) {
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    PyObject *a[1] = {r};
    Py_DECREF(r);
    return PyObject_Repr(a[0]);
}''', ['PySnip_Later'], want=['psyco_generic_call(po, cimpl_PySnip_Later_1,',
                             '"v", r)', 'vinfo_decref(r, po);'], copy=['Py_INCREF(r);']),
    Case('restart (rule loops): every path runs a run-time loop', '''
PyObject *PySnip_Loop(PyObject *o, Py_ssize_t n) {
    Py_ssize_t i;
    for (i = 0; i < PyObject_Size(o); i++) { if (PyObject_Repr(o) == NULL) return NULL; }
    return PyObject_Repr(o);
}''', ['PySnip_Loop'], want=['return psyco_generic_call(po, PySnip_Loop,'],
         report='every path from here runs a loop with a run-time bound'),
    Case('likely: a run-time branch takes the policy\'s side; failed i < 0 knows i >= 0', '''
PyObject *PySnip_Likely(PyObject *o, Py_ssize_t i) {
    if (i < 0) return PyObject_Repr(o);
    return PySequence_GetItem(o, i);
}''', ['PySnip_Likely'], items={'PySnip_Likely': [Likely('i < 0', False)]},
         want=['integer_cmp_i(po, i, 0, Py_LT)', '== CC_ERROR', 'runtime_condition_f(po,',
               'assert_nonneg(i);']),
    Case('likely: a run-time branch with no item, nothing called yet: CPython\'s function', '''
PyObject *PySnip_NoLikely(PyObject *o, Py_ssize_t i) {
    if (i < 0) return PyObject_Repr(o);
    return PySequence_GetItem(o, i);
}''', ['PySnip_NoLikely'], want=['return psyco_generic_call(po, PySnip_NoLikely,'],
         report="no 'likely' item for the run-time branch"),
    Case('likely: a run-time branch with no item after a call is refused', '''
PyObject *PySnip_NoLikely2(PyObject *o, Py_ssize_t i) {
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    Py_DECREF(r);
    if (i < 0) return PyObject_Repr(o);
    return PySequence_GetItem(o, i);
}''', ['PySnip_NoLikely2'], refused="no 'likely' item for the run-time branch"),
    Case('restype: the result is asserted to be of the type', '''
PyObject *PySnip_Restype(PyObject *o) { return PySequence_Tuple(o); }''', ['PySnip_Restype'],
         items={'PySnip_Restype': [Restype('PyTuple_Type')]},
         want=['Psyco_AssertType(po, _t1, &PyTuple_Type);']),
    Case('restype: a precondition the C contradicts is refused', '''
PyObject *PySnip_BadRestype(PyObject *o) { Py_INCREF(o); return o; }''', ['PySnip_BadRestype'],
         items={'PySnip_BadRestype': [Restype('PyTuple_Type')]},
         refused='returns its parameter o with no test that it is a PyTuple_Type'),
    Case('ownership: a value owned on one path only is held in own_X', '''
PyObject *PySnip_Mixed(PyObject *o, PyObject *w) {
    PyObject *x = o;
    if (w->ob_type->tp_as_number == NULL) {
        x = PyObject_Repr(o);
        if (x == NULL) return NULL;
    }
    PyObject *r = PyObject_Repr(x);
    return r;
}''', ['PySnip_Mixed'], want=['own_x = x;', 'own_x = NULL;', 'vinfo_xdecref(own_x, po);']),
    Case('refuse: a construct with no rule refuses the function', '''
int PySnip_Refuse(PyObject *o) { return 1; }''', ['PySnip_Refuse'],
         refused='an int result needs its convention'),
    # ---- the numbers: the word and double representations, helpers, inline ----
    Case('word: the test is a guard; case 0 runs CPython\'s function', '''
PyObject *PySnip_Word(PyLongObject *a, PyLongObject *b) {
    if (Py_ABS(Py_SIZE(a)) <= 1 && Py_ABS(Py_SIZE(b)) <= 1)
        return PyLong_FromLong(MEDIUM_VALUE(a) + MEDIUM_VALUE(b));
    return PyNumber_Add((PyObject *)a, (PyObject *)b);
}''', ['PySnip_Word'], file='long',
         want=['switch (PsycoLong_MediumValue(po, a, &w_a))', 'case 1:', 'case 0:',
               'psyco_generic_call(po, PySnip_Word,', 'default:',
               'switch (PsycoLong_MediumValue(po, b, &w_b))', 'vinfo_decref(w_a, po);',
               '/* in [-2147483646, 2147483646]: no overflow */',
               'integer_add(po, w_a, w_b, false);', 'vinfo_decref(w_a, po);',
               'vinfo_decref(w_b, po);', 'PsycoLong_FROM_WORD('],
         report='a is not one word'),
    Case('word: arithmetic that may overflow its C type resumes', '''
PyObject *PySnip_WordOvf(PyLongObject *a, PyLongObject *b) {
    if (Py_ABS(Py_SIZE(a)) <= 1 && Py_ABS(Py_SIZE(b)) <= 1)
        return PyLong_FromLong(MEDIUM_VALUE(a) * MEDIUM_VALUE(b));
    return PyNumber_Add((PyObject *)a, (PyObject *)b);
}''', ['PySnip_WordOvf'], file='long', absent=['integer_mul'],
         report='may overflow its 32-bit C type'),
    Case('word: a cast to a wider type first, and the value kept in a variable', '''
PyObject *PySnip_WordWide(PyLongObject *a, PyLongObject *b) {
    if (Py_ABS(Py_SIZE(a)) <= 1 && Py_ABS(Py_SIZE(b)) <= 1) {
        stwodigits v = (stwodigits)(MEDIUM_VALUE(a)) * MEDIUM_VALUE(b);
        return PyLong_FromLongLong((long long)v);
    }
    return PyNumber_Add((PyObject *)a, (PyObject *)b);
}''', ['PySnip_WordWide'], file='long',
         want=['integer_mul(po, w_a, w_b, false);', 'v = _t', 'vinfo_incref(v);',
               'PsycoLong_FROM_WORD(v);', 'vinfo_decref(v, po);']),
    Case('word: read with no guard before it resumes', '''
PyObject *PySnip_NoWord(PyLongObject *a) {
    return PyLong_FromLong(MEDIUM_VALUE(a) + 1);
}''', ['PySnip_NoWord'], file='long',
         want=['return psyco_generic_call(po, PySnip_NoWord,'], report='has no word'),
    Case('word: the sign compared with 0 is Ni\'s sign, compared', '''
PyObject *PySnip_Sign(PyLongObject *a) {
    if (Py_SIZE(a) < 0) return PyNumber_Negative((PyObject *)a);
    return PyNumber_Positive((PyObject *)a);
}''', ['PySnip_Sign'], file='long', items={'PySnip_Sign': [Likely('Py_SIZE(a) < 0', False)]},
         want=['PsycoLong_Sign(po, a);', 'integer_cmp_i(po, _t', '0, Py_LT)',
               'runtime_condition_f(po,']),
    Case('word: a compare as a value is the condition\'s vinfo', '''
PyObject *PySnip_SignValue(PyLongObject *a) {
    int r = Py_SIZE(a) != 0;
    return PyBool_FromLong(r);
}''', ['PySnip_SignValue'], file='long',
         want=['PsycoLong_Sign(po, a);', 'integer_non_null(po, _t1);', 'psyco_vinfo_condition(po,',
               'PsycoBool_FromLong(po, r);'], absent=['runtime_condition']),
    Case('double: C on doubles is one helper, pure, never Python, never blocking (G12)', '''
PyObject *PySnip_Double(PyFloatObject *v) {
    return PyFloat_FromDouble(v->ob_fval * 2.0 + 1.0);
}''', ['PySnip_Double'], file='float',
         want=['PsycoFloat_AS_DOUBLE(po, v);', 'vinfo_incref(',
               'CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "d",',
               'PsycoFloat_FROM_DOUBLE('],
         copy=['static double cimpl_PySnip_Double(double v_ob_fval)',
               'return (v_ob_fval * 2.0) + 1.0;']),
    Case('double: a helper that raises returns a status, its double through an array (G12)', '''
PyObject *PySnip_Raises(PyFloatObject *v) {
    double b = v->ob_fval;
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, "snip");
        return NULL;
    }
    return PyFloat_FromDouble(1.0 / b);
}''', ['PySnip_Raises'], file='float',
         want=['array_new(1);', 'CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",',
               'array_release(', '->items[0];', 'PsycoFloat_FROM_DOUBLE('],
         absent=['CfPure', 'CfPyErrCheck'],
         copy=['static int cimpl_PySnip_Raises(double v_ob_fval, double *_r)',
               '{ *_r = 1.0 / b; return 0; }', 'return -1;']),
    Case('double: errno set and read in one helper, which is not pure (G12)', '''
PyObject *PySnip_Errno(PyFloatObject *v) {
    double r;
    errno = 0;
    r = fmod(v->ob_fval, 2.0);
    if (errno) {
        PyErr_SetString(PyExc_ValueError, "snip");
        return NULL;
    }
    return PyFloat_FromDouble(r);
}''', ['PySnip_Errno'], file='float', want=['CfCommonIntZeroOk | CfNoPython | CfNoBlock'],
         absent=['CfPure'], copy=['__errno_location', 'fmod(v_ob_fval, 2.0)']),
    Case('double: errno read with no set before it in the run stays out of a helper (G12)', '''
PyObject *PySnip_ErrnoOut(PyFloatObject *v) {
    if (errno) return PyObject_Repr((PyObject *)v);
    return PyFloat_FromDouble(v->ob_fval + 1.0);
}''', ['PySnip_ErrnoOut'], file='float', want=['return psyco_generic_call(po, PySnip_ErrnoOut,']),
    Case('inline: a function\'s name given to a static is that function in its body (G12)', '''
static PyObject *snip_fp(PyFloatObject *v, double (*func)(double)) {
    return PyFloat_FromDouble(func(v->ob_fval));
}
PyObject *PySnip_Fp(PyFloatObject *v) { return snip_fp(v, fabs); }''', ['PySnip_Fp'],
         file='float', want=['CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "d",'],
         absent=['snip_fp(po', '(long)(func)'], copy=['return fabs(v_ob_fval);']),
    Case('inline: a constant and a function given through two levels, (*f)(x) called', '''
static PyObject *snip_lvl2(PyObject *o, double (*f)(double), int k) {
    double x = PyFloat_AsDouble(o);
    if (x == -1.0 && PyErr_Occurred())
        return NULL;
    x = (*f)(x);
    if (k)
        x = x + 1.0;
    return PyFloat_FromDouble(x);
}
static PyObject *snip_lvl1(PyObject *o, double (*f)(double)) { return snip_lvl2(o, f, 0); }
PyObject *PySnip_Lvl(PyObject *o) { return snip_lvl1(o, sqrt); }''', ['PySnip_Lvl'],
         file='float', want=['= PsycoFloat_AsDouble(po, o);', '== NULL', 'cimpl_PySnip_Lvl,',
                             'PsycoFloat_FROM_DOUBLE('],
         absent=['snip_lvl1(po', 'snip_lvl2(po'],
         copy=['(*sqrt)(snip_lvl2_x)', 'if (0)']),
    Case('inline: a parameter the body assigns stays a fresh local, given a constant', '''
static PyObject *snip_set(PyFloatObject *v, double (*f)(double), int k) {
    k = k + 1;
    return PyFloat_FromDouble(f(v->ob_fval) + k);
}
PyObject *PySnip_Set(PyFloatObject *v) { return snip_set(v, fabs, 2); }''', ['PySnip_Set'],
         file='float', copy=['int snip_set_k = 2;', 'fabs(v_ob_fval) + snip_set_k'], absent=['snip_set(po']),
    Case('double: a static function on scalars is called in the helper as its copy', '''
static int snip_big(double x) {
    if (x > 1.0) {
        PyErr_SetString(PyExc_ValueError, "big");
        return 1;
    }
    return 0;
}
PyObject *PySnip_Static(PyFloatObject *v) {
    double x = v->ob_fval * 2.0;
    if (snip_big(x))
        return NULL;
    return PyFloat_FromDouble(x);
}''', ['PySnip_Static'], file='float',
         want=['CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",'], absent=['CfPure'],
         copy=['if (cimpl_snip_big', '(x))', 'return -1;']),
    Case('double: a static reading errno the run did not set stays out of its helper (G12)', '''
static int snip_err(double x) { return errno != 0; }
PyObject *PySnip_StaticErrno(PyFloatObject *v) {
    double x = v->ob_fval * 2.0;
    if (snip_err(x))
        return NULL;
    return PyFloat_FromDouble(x);
}''', ['PySnip_StaticErrno'], file='float', absent=['cimpl_snip_err']),
    Case('inline (G13): parameters by value, *p the caller\'s variable', '''
static int snip_take(PyObject **p, double *d) {
    PyObject *o = *p;
    if (!PyFloat_CheckExact(o)) {
        *p = NULL;
        return -1;
    }
    *d = PyFloat_AS_DOUBLE(o);
    return 0;
}
PyObject *PySnip_Inline(PyObject *v) {
    double x;
    if (snip_take(&v, &x) < 0)
        return v;
    return PyFloat_FromDouble(x + 1.0);
}''', ['PySnip_Inline'], file='float',
         want=['snip_take_o = v;', 'if (!(tp_v == (&PyFloat_Type)))', 'return NULL;',
               'PsycoFloat_AS_DOUBLE(po, snip_take_o);', 'x = _t',
               'PsycoFloat_FROM_DOUBLE('],
         absent=['snip_take(po', 'cimpl_snip_take']),
    Case('inline (G13): an argument is evaluated once', '''
static int snip_once(double a, double *d) {
    *d = a * a;
    return 0;
}
PyObject *PySnip_Once2(PyFloatObject *v) {
    double x;
    if (snip_once(v->ob_fval, &x) < 0)
        return NULL;
    return PyFloat_FromDouble(x);
}''', ['PySnip_Once2'], file='float',
         want=['PsycoFloat_AS_DOUBLE(po, v);', 'PsycoFloat_FROM_DOUBLE('],
         copy=['snip_once_a = v_ob_fval;', 'x = snip_once_a * snip_once_a;']),
    Case('field read: a field TYPES gives a descriptor of, for the struct the C casts to', '''
PyObject *PySnip_Field(PyObject *o) {
    Py_ssize_t n = Py_SIZE((PyTupleObject *)o);
    return PyLong_FromSsize_t(n);
}''', ['PySnip_Field'], want=['_t1 = psyco_get_field(po, o, FIX_size);', 'if (_t1 == NULL)',
                             'n = _t1;']),
    Case('field read: a cast to the struct an object begins with reads as its own', '''
PyObject *PySnip_Field2(PyListObject *a) {
    Py_ssize_t n = Py_SIZE(a);
    return PyLong_FromSsize_t(n);
}''', ['PySnip_Field2'], want=['psyco_get_field(po, a, VAR_size);']),
    Case('field read: a field with no descriptor resumes', '''
PyObject *PySnip_Field3(PyObject *o) {
    Py_ssize_t n = ((PyListObject *)o)->allocated;
    return PyLong_FromSsize_t(n);
}''', ['PySnip_Field3'], absent=['psyco_get_field'],
         report='the field allocated of a run-time value'),
    Case('bit test: a one-bit field read through its descriptor, the top bit compared', '''
PyObject *PySnip_Ready(PyObject *s) {
    if (PyUnicode_IS_READY(s)) return PyObject_Repr(s);
    return PyObject_Str(s);
}''', ['PySnip_Ready'], items={'PySnip_Ready': [Likely('PyUnicode_IS_READY(s)', True)]},
         want=['psyco_get_field(po, s, UNICODE_state);',
               'NI_BIT_PyASCIIObject__state__ready, Py_GE | COMPARE_UNSIGNED);',
               'runtime_condition_t(po,', 'PyObject_Repr,']),
    Case('bit test: with no likely item before any call, CPython\'s function runs (D111)', '''
PyObject *PySnip_Ready2(PyObject *s) {
    if (PyUnicode_IS_READY(s)) return PyObject_Repr(s);
    return PyObject_Str(s);
}''', ['PySnip_Ready2'], want=['return psyco_generic_call(po, PySnip_Ready2,'],
         absent=['UNICODE_state'], report="no 'likely' item for the run-time test"),
    Case('run-time choice: the likely arm, and CPython resumes where the other is taken', '''
PyObject *PySnip_Choice(PyObject *s) {
    if ((PyUnicode_IS_READY(s) ? 0 : PyObject_Size(s)) == -1)
        return NULL;
    return PyObject_Repr(s);
}''', ['PySnip_Choice'], items={'PySnip_Choice': [Likely('PyUnicode_IS_READY(s)', True)]},
         want=['_t3 = runtime_condition_t(po, _t2);', 'if (!_t3) {',
               'return psyco_generic_call(po, PySnip_Choice,', 'PyObject_Repr,'],
         absent=['PsycoObject_Size', 'PyObject_Size,'], report='the unlikely arm of'),
    Case('G11: a compare\'s operand is evaluated once', '''
PyObject *PySnip_G11(PyObject *o) {
    if (PyObject_Size(o) < 0) return NULL;
    return PyObject_Repr(o);
}''', ['PySnip_G11'], items={'PySnip_G11': [Likely('PyObject_Size(o) < 0', False)]},
         want=['_t1 = PsycoObject_Size(po, o);', 'integer_cmp_i(po, _t1, 0, Py_LT);',
               'vinfo_decref(_t1, po);'],
         absent=['_t2 = PsycoObject_Size', '_t3 = PsycoObject_Size']),
    # ---- tuples: known lengths, meta loops, items, virtual tuples ----
    Case('known length: a loop over a tuple\'s items runs when Ni compiles', '''
PyObject *PySnip_TLoop(PyTupleObject *a, PyObject *o) {
    Py_ssize_t i;
    for (i = 0; i < Py_SIZE(a); i++)
        { PyObject *t = PyObject_GetItem(o, a->ob_item[i]); if (t == NULL) return NULL; Py_DECREF(t); }
    Py_INCREF(o);
    return o;
}''', ['PySnip_TLoop'], file='tuple',
         want=['_t1 = PsycoTuple_Load(a);', 'if (_t1 < 0 || _t1 > 16) {',
               'return psyco_generic_call(po, PySnip_TLoop,', 'i = 0;', 'for (;;) {',
               'if (!(i < _t1))', 'break;',
               'PsycoObject_GetItem(po, o, PsycoTuple_GET_ITEM(a, i))', 'i++;',
               'vinfo_incref(o);', 'return o;'],
         report='the length of a is not known at compile time or is over 16'),
    Case('loop budget: a meta bound that is no loaded length is checked when Ni compiles', '''
PyObject *PySnip_TBudget(PyObject *o, Py_ssize_t n) {
    Py_ssize_t i;
    for (i = 0; i < n; i++)
        { PyObject *t = PyObject_GetItem(o, o); if (t == NULL) return NULL; Py_DECREF(t); }
    Py_INCREF(o);
    return o;
}''', ['PySnip_TBudget'], file='tuple', items={'PySnip_TBudget': [IfKnown('n')]},
         want=['if (!is_compiletime(n->source)) {', 'return psyco_generic_call(po, PySnip_TBudget,',
               'n_k = (Py_ssize_t)CompileTime_Get(n->source)->value;', 'if (n_k > 16) {',
               'for (;;) {', 'if (!(i < n_k))'],
         report='a loop of more than 16 turns'),
    Case('if_known: a parameter Ni does not know runs CPython\'s function', '''
PyObject *PySnip_TIfK(PyObject *o, Py_ssize_t n) {
    if (n == 1) return PyObject_Repr(o);
    return PyObject_Str(o);
}''', ['PySnip_TIfK'], file='tuple', items={'PySnip_TIfK': [IfKnown('n')]},
         want=['if (!is_compiletime(n->source)) {', '"vv", o, n);', 'if (n_k == 1) {',
               'PyObject_Repr,', 'PyObject_Str,'],
         absent=['Psyco_NeedType(po, n)'], report='n is not known at compile time'),
    Case('item promotion: item types a turn needs after a call, at the loop\'s entry', '''
PyObject *PySnip_TProm(PyTupleObject *a, PyObject *o) {
    Py_ssize_t i;
    for (i = 0; i < Py_SIZE(a); i++)
        if (PyObject_RichCompareBool(o, a->ob_item[i], Py_EQ) < 0) return NULL;
    Py_INCREF(o);
    return o;
}''', ['PySnip_TProm'], file='tuple', 
         items={'PySnip_TProm': [Likely('PyObject_RichCompareBool(o, a->ob_item[i], Py_EQ) < 0',
                                        False)]},
         want=['_t1 = PsycoTuple_Load(a);', 'tp_o = Psyco_NeedType(po, o);',
               'for (_t2 = 0; _t2 < _t1; _t2++) {',
               'if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(a, _t2)) == NULL) {', 'for (;;) {',
               'PsycoObject_RichCompareBool(po, o, PsycoTuple_GET_ITEM(a, i), 2)']),
    Case('item promotion: refused when a call comes before the loop (rule 1)', '''
PyObject *PySnip_TLate(PyTupleObject *a, PyObject *o) {
    Py_ssize_t i;
    { PyObject *t = PyObject_GetItem(o, o); if (t == NULL) return NULL; Py_DECREF(t); }
    for (i = 0; i < Py_SIZE(a); i++)
        if (PyObject_RichCompareBool(o, a->ob_item[i], Py_EQ) < 0) return NULL;
    Py_INCREF(o);
    return o;
}''', ['PySnip_TLate'], file='tuple',
         items={'PySnip_TLate': [Likely('PyObject_RichCompareBool(o, a->ob_item[i], Py_EQ) < 0',
                                        False)]},
         refused='the types of the items of a are needed after a call'),
    Case('item read: a run-time index reads the item array; the bounds test is one compare', '''
PyObject *PySnip_TItem(PyTupleObject *a, Py_ssize_t i) {
    if (i < 0 || i >= Py_SIZE(a)) {
        PyErr_SetString(PyExc_IndexError, "out of range");
        return NULL;
    }
    Py_INCREF(a->ob_item[i]);
    return a->ob_item[i];
}''', ['PySnip_TItem'], file='tuple',
         items={'PySnip_TItem': [Likely('i < 0 || i >= Py_SIZE(a)', False)]},
         want=['_t1 = psyco_get_field(po, a, FIX_size);',
               '_t2 = integer_cmp(po, i, _t1, Py_GE|COMPARE_UNSIGNED);', 'vinfo_decref(_t1, po);',
               '_t3 = runtime_condition_f(po, _t2);',
               'PycException_SetString(po, PyExc_IndexError,',
               'psyco_get_field_array(po, a, FPYREF(TUPLE_ob_item), i);'],
         absent=['PsycoTuple_Load']),
    Case('virtual tuple: built by the meta, its items stored, never NULL', '''
PyObject *PySnip_TNew(PyTupleObject *a) {
    Py_ssize_t i;
    PyTupleObject *np = (PyTupleObject *)PyTuple_New(Py_SIZE(a));
    if (np == NULL) return NULL;
    for (i = 0; i < Py_SIZE(a); i++) {
        PyObject *v = a->ob_item[i];
        Py_INCREF(v);
        np->ob_item[i] = v;
    }
    return (PyObject *)np;
}''', ['PySnip_TNew'], file='tuple',
         want=['_t1 = PsycoTuple_Load(a);', 'PsycoTuple_New(', 'v = PsycoTuple_GET_ITEM(a, i);',
               'vinfo_incref(v);', 'PsycoTuple_GET_ITEM(np, i) = v;', 'return np;'],
         absent=['np == NULL', 'PyTuple_New,']),
    Case('virtual tuple: a borrowed item stored is increfed by the next statement', '''
PyObject *PySnip_TPend(PyTupleObject *a) {
    Py_ssize_t i;
    PyTupleObject *np = (PyTupleObject *)PyTuple_New(Py_SIZE(a));
    if (!np) return NULL;
    for (i = 0; i < Py_SIZE(a); i++) {
        np->ob_item[i] = a->ob_item[i];
        Py_INCREF(np->ob_item[i]);
    }
    return (PyObject *)np;
}''', ['PySnip_TPend'], file='tuple',
         want=['PsycoTuple_GET_ITEM(np, i) = PsycoTuple_GET_ITEM(a, i);',
               'vinfo_incref(PsycoTuple_GET_ITEM(np, i));'], absent=['np == NULL', '!np']),
    Case('virtual tuple: a borrowed item stored and never increfed is refused', '''
PyObject *PySnip_TNoInc(PyTupleObject *a) {
    Py_ssize_t i;
    PyTupleObject *np = (PyTupleObject *)PyTuple_New(Py_SIZE(a));
    if (np == NULL) return NULL;
    for (i = 0; i < Py_SIZE(a); i++)
        np->ob_item[i] = a->ob_item[i];
    return (PyObject *)np;
}''', ['PySnip_TNoInc'], file='tuple', refused='does not incref'),
    Case('virtual tuple: more items than Ni builds runs CPython\'s function', '''
PyObject *PySnip_TBig(PyTupleObject *a, PyTupleObject *b) {
    Py_ssize_t i;
    PyTupleObject *np = (PyTupleObject *)PyTuple_New(Py_SIZE(a) + Py_SIZE(b));
    if (np == NULL) return NULL;
    for (i = 0; i < Py_SIZE(a); i++) {
        PyObject *v = a->ob_item[i];
        Py_INCREF(v);
        np->ob_item[i] = v;
    }
    return (PyObject *)np;
}''', ['PySnip_TBig'], file='tuple',
         want=['_t1 = PsycoTuple_Load(a);', 'PsycoTuple_Load(b);',
               '> CT_TUPLE_LOAD_SIZE_LIMIT) {', 'return psyco_generic_call(po, PySnip_TBig,',
               'PsycoTuple_New('],
         report='a PyTuple_Type of more than CT_TUPLE_LOAD_SIZE_LIMIT items'),
    Case('pointers: an offset into an object\'s items, moved and stored through', '''
PyObject *PySnip_TPtr(PyTupleObject *a) {
    Py_ssize_t i;
    PyObject **p, **src;
    PyTupleObject *np = (PyTupleObject *)PyTuple_New(Py_SIZE(a));
    if (np == NULL) return NULL;
    p = np->ob_item;
    src = a->ob_item;
    for (i = 0; i < Py_SIZE(a); i++) {
        *p = src[i];
        Py_INCREF(*p);
        p++;
    }
    return (PyObject *)np;
}''', ['PySnip_TPtr'], file='tuple',
         want=['p = 0;', 'src = 0;', 'PsycoTuple_GET_ITEM(np, p) = PsycoTuple_GET_ITEM(a, src + i);',
               'vinfo_incref(PsycoTuple_GET_ITEM(np, p));', 'p++;', 'return np;']),
    Case('switch: on a meta value, a switch of the meta', '''
static PyObject *snip_tsw(PyTupleObject *v, PyTupleObject *w, int op) {
    switch (op) {
    case Py_EQ: if (Py_SIZE(v) == Py_SIZE(w)) Py_RETURN_TRUE; Py_RETURN_FALSE;
    case Py_LT: if (Py_SIZE(v) < Py_SIZE(w)) Py_RETURN_TRUE; Py_RETURN_FALSE;
    default: Py_RETURN_NOTIMPLEMENTED;
    }
}
PyObject *PySnip_TSw(PyTupleObject *v, PyTupleObject *w) { return snip_tsw(v, w, Py_EQ); }''',
         ['PySnip_TSw'], fn='snip_tsw', file='tuple',
         want=['switch (op) {', 'case 2:', 'if (_t1 == _t2) {', 'return psyco_vi_Py_True();',
               'case 0:', 'default:', 'return psyco_vi_NotImplemented();']),
    Case('G6: a resume inside a meta loop runs CPython\'s code from the loop', '''
PyObject *PySnip_TG6(PyTupleObject *a, PyObject *o) {
    Py_ssize_t i;
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    for (i = 0; i < Py_SIZE(a); i++) {
        PyObject *b[1] = {a->ob_item[i]};
        PyObject *t = PyObject_GetItem(o, b[0]); if (t == NULL) { Py_DECREF(r); return NULL; } Py_DECREF(t);
    }
    return r;
}''', ['PySnip_TG6'], file='tuple', want=['cimpl_PySnip_TG6_1'],
         absent=['for (;;)', 'PsycoTuple_Load'], copy=['for (i = 0;'], report='an array b'),
    Case('G9: a loop taken back takes back the likely items its turns used', '''
PyObject *PySnip_TG9(PyTupleObject *a, Py_ssize_t k) {
    Py_ssize_t i, j = 0;
    for (i = 0; i < Py_SIZE(a); i++) {
        j = k < 0 ? PyObject_Size(a->ob_item[i]) : 0;
        PyObject *b[1] = {a->ob_item[i]};
    }
    return PyLong_FromSsize_t(j);
}''', ['PySnip_TG9'], file='tuple', items={'PySnip_TG9': [Likely('k < 0', False)]},
         refused="likely('k < 0', False) matches no run-time branch"),
    Case('G14: a store under a condition stays under it; a NULL test of a virtual folds', '''
PyObject *PySnip_TG14(PyObject *o, PyObject *w) {
    PyTupleObject *np = (PyTupleObject *)PyTuple_New(1);
    if (!np) return NULL;
    if (o->ob_type == &PyLong_Type) { Py_INCREF(o); np->ob_item[0] = o; }
    else { Py_INCREF(w); np->ob_item[0] = w; }
    return (PyObject *)np;
}''', ['PySnip_TG14'], file='tuple',
         want=['_t1 = 1;', 'PsycoTuple_New(_t1, NULL);', 'if (tp_o == (&PyLong_Type)) {',
               'vinfo_incref(o);',
               'PsycoTuple_GET_ITEM(np, 0) = o;', 'else {', 'vinfo_incref(w);',
               'PsycoTuple_GET_ITEM(np, 0) = w;', 'return np;'],
         absent=['np == NULL', '!np', 'CT_TUPLE_LOAD_SIZE_LIMIT']),
    Case('G19: a loop\'s entry promotions are its own (loops keyed by place)', '''
PyObject *PySnip_TG19(PyTupleObject *a, PyTupleObject *b, PyObject *o) {
    Py_ssize_t i;
    for (i = 0; i < Py_SIZE(a); i++)
        if (PyObject_RichCompareBool(o, a->ob_item[i], Py_EQ) < 0) return NULL;
    for (i = 0; i < Py_SIZE(b); i++)
        { PyObject *t = PyObject_Dir(b->ob_item[i]); if (t == NULL) return NULL; Py_DECREF(t); }
    Py_INCREF(o);
    return o;
}''', ['PySnip_TG19'], file='tuple',
         items={'PySnip_TG19': [Likely('PyObject_RichCompareBool(o, a->ob_item[i], Py_EQ) < 0',
                                        False)]},
         want=['Psyco_NeedType(po, PsycoTuple_GET_ITEM(a, _t', 'for (;;) {',
               'PsycoObject_RichCompareBool(', 'PsycoTuple_Load(b);', 'for (;;) {',
               'psyco_generic_call(po, PyObject_Dir,'],
         absent=['Psyco_NeedType(po, PsycoTuple_GET_ITEM(b,']),
    Case('G21: a virtual\'s size stays Py_ssize_t up to its limit test', '''
PyObject *PySnip_TG21(PyTupleObject *a, Py_ssize_t n) {
    Py_ssize_t i, j, size;
    PyObject **p;
    PyTupleObject *np;
    if (n < 0) n = 0;
    if (Py_SIZE(a) == 0 || n > PY_SSIZE_T_MAX / Py_SIZE(a)) return PyErr_NoMemory();
    size = Py_SIZE(a) * n;
    np = (PyTupleObject *)PyTuple_New(size);
    if (np == NULL) return NULL;
    p = np->ob_item;
    for (i = 0; i < n; i++)
        for (j = 0; j < Py_SIZE(a); j++) { *p = a->ob_item[j]; Py_INCREF(*p); p++; }
    return (PyObject *)np;
}''', ['PySnip_TG21'], file='tuple', items={'PySnip_TG21': [IfKnown('n')]},
         want=['Py_ssize_t _t3;', '_t3 = size;',
               'if (_t3 < 0 || _t3 > CT_TUPLE_LOAD_SIZE_LIMIT) {', 'PsycoTuple_New(_t3, NULL);'],
         absent=['int _t3;']),
    # ---- the old generator's bugs, the correct behaviour ----
    Case('G6: no resume starts inside a loop the meta runs (the meta runs none)', '''
PyObject *PySnip_G6(PyObject *o, Py_ssize_t n) {
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    while (n-- > 0) { if (o->ob_type->tp_as_number == NULL) break; }
    return r;
}''', ['PySnip_G6'], want=['cimpl_PySnip_G6_1'], absent=['Psyco_TypeSlot', 'while'],
         report='a loop with a run-time bound'),
    Case('G20: a copy from inside a block that returns has none of the code after it', '''
PyObject *PySnip_G20(PyObject *o, PyObject *w, Py_ssize_t n) {
    if (o->ob_type->tp_as_number != NULL) {
        PyObject *r = PyObject_Repr(o);
        if (r == NULL) return NULL;
        while (n-- > 0) { if (PyObject_Size(r) < 0) break; }
        return r;
    }
    return PyNumber_Negative(w);
}''', ['PySnip_G20'], want=['cimpl_PySnip_G20_1'], copy=['while', 'return r;'],
         absent_copy=['PyNumber_Negative']),
    Case("G7: a discarded bool result's call is still made", '''
PyObject *PySnip_G7a(PyObject *o, PyObject *k) {
    PyObject_SetItem(o, k, k);
    return PyObject_Repr(o);
}''', ['PySnip_G7a'], want=['PsycoObject_SetItem(po, o, k, k);']),
    Case("G7: a discarded call's result is released", '''
PyObject *PySnip_G7b(PyObject *o) {
    PyObject_Repr(o);
    return PyObject_Repr(o);
}''', ['PySnip_G7b'], want=['_t1 = psyco_generic_call(po, PyObject_Repr,',
                            'vinfo_xdecref(_t1, po);']),
    Case("G7: a compared call result is released", '''
PyObject *PySnip_G7c(PyObject *o) {
    if (PyObject_Size(o) < 0) return NULL;
    return PyObject_Repr(o);
}''', ['PySnip_G7c'], items={'PySnip_G7c': [Likely('PyObject_Size(o) < 0', False)]},
         want=['integer_cmp_i(po, _t1, 0, Py_LT)', 'vinfo_decref(_t1, po);', '== CC_ERROR']),
    Case('G8: after a compile-time raise, no run-time call is made', '''
PyObject *PySnip_G8(PyObject *o) {
    PyErr_SetString(PyExc_TypeError, "x");
    PyErr_Clear();
    return PyObject_Repr(o);
}''', ['PySnip_G8'], refused='a run-time call where an error is pending'),
    Case('G9: a statement taken back takes back the likely item it used', '''
PyObject *PySnip_G9(PyObject *o, Py_ssize_t i) {
    PyObject *a[1] = {o};
    Py_ssize_t j = i < 0 ? PyObject_Size(a[0]) : 0;
    return PyLong_FromSsize_t(j);
}''', ['PySnip_G9'], items={'PySnip_G9': [Likely('i < 0', False)]},
         refused="likely('i < 0', False) matches no run-time branch"),
    # ---- builtins: METH_FASTCALL arguments, the count's polyvariance ----
    Case("fastcall: the count checked is Ni's tuple length; outside its bounds, the C function",
         '''
static PyObject *snip_f1(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *a, *b;
    if (!_PyArg_UnpackStack(args, nargs, "f1", 2, 2, &a, &b)) return NULL;
    return PyNumber_Add(a, b);
}''', ['snip_f1'], methods=[('f1', 'snip_f1', 'METH_FASTCALL')],
         want=['snip_f1(PsycoObject *po, vinfo_t *m, vinfo_t *vargs)',
               '_t1 = PsycoTuple_Load(vargs);', 'if (_t1 < 2 || _t1 > 2) {',
               'return PsycoCFunction_CallFast(po, cfunc_snip_f1, METH_FASTCALL, m, vargs);',
               'a = PsycoTuple_GET_ITEM(vargs, 0);', 'b = PsycoTuple_GET_ITEM(vargs, 1);',
               'PsycoNumber_Add(po, a, b)'],
         report='a count of arguments outside 2..2'),
    Case('fastcall: each count is its own path; outputs past it keep their value', '''
static PyObject *snip_f2(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *a, *b = Py_None;
    if (!_PyArg_UnpackStack(args, nargs, "f2", 1, 2, &a, &b)) return NULL;
    return PyNumber_Add(a, b);
}''', ['snip_f2'], methods=[('f2', 'snip_f2', 'METH_FASTCALL')],
         want=['if (_t1 < 1 || _t1 > 2) {', 'if (_t1 == 1) {', 'a = PsycoTuple_GET_ITEM(vargs, 0);',
               'b = psyco_vi_None();', 'PsycoNumber_Add(po, a, b)', 'vinfo_decref(b, po);',
               'else {', 'b = PsycoTuple_GET_ITEM(vargs, 1);', 'PsycoNumber_Add(po, a, b)'],
         absent=['vinfo_decref(b, po);\n    return PsycoCFunction_CallFast']),
    Case("fastcall: 3.8's bounds test folds for each count; args[k] is the tuple's item", '''
static PyObject *snip_f3(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *a, *b;
    if (!((1 <= nargs && nargs <= 2) || _PyArg_CheckPositional("f3", nargs, 1, 2)))
        return NULL;
    a = args[0];
    if (nargs > 1) { b = args[1]; return PyNumber_Add(a, b); }
    return PyNumber_Negative(a);
}''', ['snip_f3'], methods=[('f3', 'snip_f3', 'METH_FASTCALL')],
         want=['if (_t1 < 1 || _t1 > 2) {', 'if (_t1 == 1) {', 'a = PsycoTuple_GET_ITEM(vargs, 0);',
               'PsycoNumber_Negative(po, a)', 'else {', 'b = PsycoTuple_GET_ITEM(vargs, 1);',
               'PsycoNumber_Add(po, a, b)'],
         absent=['_PyArg_CheckPositional', 'nargs']),
    Case('fastcall: the count read before it is checked runs the C function', '''
static PyObject *snip_f4(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 1) return NULL;
    return PyNumber_Negative(args[0]);
}''', ['snip_f4'], methods=[('f4', 'snip_f4', 'METH_FASTCALL')],
         want=['return PsycoCFunction_CallFast(po, cfunc_snip_f4, METH_FASTCALL, m, vargs);'],
         report='the count of arguments before it is checked'),
    Case('fastcall: an item read past the checked count is refused', '''
static PyObject *snip_f5(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *a;
    if (!((1 <= nargs && nargs <= 1) || _PyArg_CheckPositional("f5", nargs, 1, 1)))
        return NULL;
    a = args[1];
    return PyNumber_Negative(a);
}''', ['snip_f5'], methods=[('f5', 'snip_f5', 'METH_FASTCALL')],
         refused='read outside the checked count'),
    Case('fastcall: more counts than the polyvariance budget run the C function', '''
static PyObject *snip_f6(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *a, *b, *c, *d, *e, *f;
    if (!_PyArg_UnpackStack(args, nargs, "f6", 0, 6, &a, &b, &c, &d, &e, &f)) return NULL;
    return PyNumber_Negative(a);
}''', ['snip_f6'], methods=[('f6', 'snip_f6', 'METH_FASTCALL')],
         want=['return PsycoCFunction_CallFast(po, cfunc_snip_f6, METH_FASTCALL, m, vargs);'],
         absent=['PsycoTuple_Load'], report='budget polyvariance'),
    Case("fastcall: an item's type is promoted as a parameter's, where its callee needs it", '''
static PyObject *snip_f7_impl(PyObject *m, PyObject *o) {
    if (o->ob_type->tp_as_number == NULL) return NULL;
    return PyNumber_Negative(o);
}
static PyObject *snip_f7(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *o;
    if (!_PyArg_UnpackStack(args, nargs, "f7", 1, 1, &o)) return NULL;
    return snip_f7_impl(m, o);
}''', ['snip_f7'], methods=[('f7', 'snip_f7', 'METH_FASTCALL')],
         want=['o = PsycoTuple_GET_ITEM(vargs, 0);', 'tp_o = Psyco_NeedType(po, o);',
               'snip_f7_impl(po, m, o)']),
    Case("METH_O: a 0-or-1 result's bool is Ni's of that word", '''
static PyObject *snip_b1(PyObject *m, PyObject *o) {
    int r = PyObject_IsInstance(o, o);
    if (r < 0) return NULL;
    return PyBool_FromLong(r);
}''', ['snip_b1'], methods=[('b1', 'snip_b1', 'METH_O')],
         want=['PsycoObject_IsInstance(po, o, o)', 'if (r == NULL)',
               'PsycoBool_FromZeroOrOne(po, r)'], absent=['PsycoBool_FromLong']),
    Case("METH_O: a bool of an int not known to be 0 or 1 is Ni's general one", '''
static PyObject *snip_b3(PyObject *m, PyObject *o) {
    int r = PyObject_IsTrue(o);
    if (r < 0) return NULL;
    return PyBool_FromLong(r);
}''', ['snip_b3'], methods=[('b3', 'snip_b3', 'METH_O')],
         want=['PsycoBool_FromLong(po, r)'], absent=['FromZeroOrOne']),
    Case('error test: x == -1 alone is the error of a result that is never -1', '''
static PyObject *snip_h(PyObject *m, PyObject *o) {
    Py_hash_t x = PyObject_Hash(o);
    if (x == -1) return NULL;
    return PyLong_FromSsize_t(x);
}''', ['snip_h'], methods=[('h', 'snip_h', 'METH_O')],
         want=['x = _t1;', 'if (x == NULL) {', 'return NULL;', 'PsycoLong_FromSsize_t(po, x)'],
         absent=['integer_cmp']),
    Case("status: Ni's bool tested as the C tests 0/-1; NULL where Ni's meta takes it", '''
static PyObject *snip_s1(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *o, *k;
    if (!_PyArg_UnpackStack(args, nargs, "s1", 2, 2, &o, &k)) return NULL;
    if (PyObject_SetAttr(o, k, (PyObject *)NULL) != 0) return NULL;
    Py_RETURN_NONE;
}''', ['snip_s1'], methods=[('s1', 'snip_s1', 'METH_FASTCALL')],
         want=['if (!(PsycoObject_SetAttr(po, o, k, NULL))) {', 'return NULL;',
               'return psyco_vi_None();']),
    Case('fold: a variable known to hold NULL compared with NULL is known', '''
static PyObject *snip_n(PyObject *m, PyObject *o) {
    PyObject *d = NULL;
    if (d != NULL) return PyNumber_Negative(d);
    return PyNumber_Positive(o);
}''', ['snip_n'], methods=[('n', 'snip_n', 'METH_O')],
         want=['PsycoNumber_Positive(po, o)'], absent=['Negative', 'd != NULL']),
    Case("G11: a status compare's call is made once", '''
static PyObject *snip_g11(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *o, *k;
    if (!_PyArg_UnpackStack(args, nargs, "g11", 2, 2, &o, &k)) return NULL;
    if (!PyUnicode_Check(k)) return NULL;
    if (PyObject_SetAttr(o, k, k) != 0) return NULL;
    Py_RETURN_NONE;
}''', ['snip_g11'], methods=[('g11', 'snip_g11', 'METH_FASTCALL')],
         want=['tp_k = Psyco_NeedType(po, k);', 'if (!(PsycoObject_SetAttr(po, o, k, k))) {'],
         absent=['PsycoObject_SetAttr(po, o, k, k);']),
    Case("G8: where a status test found the error, no run-time call is made", '''
static PyObject *snip_g8(PyObject *m, PyObject *o) {
    if (PyObject_SetAttr(o, o, o) != 0) { PyObject_Repr(o); return NULL; }
    Py_RETURN_NONE;
}''', ['snip_g8'], methods=[('g8', 'snip_g8', 'METH_O')],
         refused='a run-time call where an error is pending'),
    Case('G15: a switch case falling into the next is refused', '''
static PyObject *snip_sw(PyObject *o, int k) {
    int n = 0;
    switch (k) {
    case 1: n = 1;
    case 2: return PyNumber_Negative(o);
    default: return NULL;
    }
}
PyObject *PySnip_G15(PyObject *o) { return snip_sw(o, 1); }''', ['PySnip_G15'], fn='snip_sw',
         refused='runs on into the next'),
    # ---- raw addresses (gen2/address.py), errors on one arm, Holds ----
    Case('address: moved by a meta offset, the bytes there read as a signed int', '''
static PyObject *snip_ad(const char *addr, Py_ssize_t off) {
    addr += off;
    return PyLong_FromLong(*(int *)addr);
}
PyObject *PySnip_Ad(PyObject *o) { return snip_ad((char *)o, 16); }''', ['PySnip_Ad'],
         fn='snip_ad',
         want=['_t1 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(int, 0)), 0 + (off));',
               'if (_t1 == NULL)', 'PsycoLong_FromLong(po, _t1);', 'vinfo_decref(_t1, po);'],
         absent=['integer_add']),
    Case('address: an unsigned read is UNSIGNED_ARRAY', '''
static PyObject *snip_au(const char *addr, Py_ssize_t off) {
    addr += off;
    return PyLong_FromUnsignedLong(*(unsigned short *)addr);
}
PyObject *PySnip_Au(PyObject *o) { return snip_au((char *)o, 16); }''', ['PySnip_Au'],
         fn='snip_au', want=['FMUT(UNSIGNED_ARRAY(unsigned short, 0)), 0 + (off))',
                             'PsycoLong_FromUnsignedLong(po,']),
    Case('address: the address as a value is the object\'s plus the offset', '''
static PyObject *snip_av(const char *addr, Py_ssize_t off) {
    addr += off;
    return PyUnicode_FromString((char *)addr);
}
PyObject *PySnip_Av(PyObject *o) { return snip_av((char *)o, 16); }''', ['PySnip_Av'],
         fn='snip_av', want=['_t1 = integer_add_i(po, addr, 0 + (off), false);',
                             'PyUnicode_FromString,', '"v", _t1);', 'vinfo_decref(_t1, po);']),
    Case('address: a float is no word Ni reads: it resumes', '''
static PyObject *snip_af(const char *addr, Py_ssize_t off) {
    addr += off;
    return PyFloat_FromDouble(*(float *)addr);
}
PyObject *PySnip_Af(PyObject *o) { return snip_af((char *)o, 16); }''', ['PySnip_Af'],
         fn='snip_af', report='a read of a float through addr'),
    Case('address: moved by a run-time amount, it resumes', '''
static PyObject *snip_ar(const char *addr, Py_ssize_t n) {
    addr += n;
    return PyLong_FromLong(*(int *)addr);
}
PyObject *PySnip_Ar(PyObject *o, Py_ssize_t n) { return snip_ar((char *)o, n); }''',
         ['PySnip_Ar'], fn='snip_ar', report='addr moved by a run-time amount'),
    Case('address: an object read holds no reference: its incref is need_reference, '
         'its NULL a run-time test, released there', '''
static PyObject *snip_ao(const char *addr, Py_ssize_t off) {
    PyObject *v;
    addr += off;
    v = *(PyObject **)addr;
    if (v == NULL) v = Py_None;
    Py_INCREF(v);
    return v;
}
PyObject *PySnip_Ao(PyObject *o) { return snip_ao((char *)o, 16); }''', ['PySnip_Ao'],
         fn='snip_ao', items={'snip_ao': [Likely('v == NULL', False)]},
         want=['FMUT(DEF_ARRAY(PyObject *, 0))', 'v = _t1;', 'integer_non_null(po, v);',
               'runtime_condition_f(po,', 'vinfo_decref(v, po);', 'v = psyco_vi_None();',
               'need_reference(po, v);', 'return v;'], absent=['vinfo_incref']),
    Case('likely: an item naming two branches says so', '''
static PyObject *snip_a2(const char *addr, Py_ssize_t off, int k) {
    PyObject *v;
    addr += off;
    v = *(PyObject **)addr;
    if (k) { if (v == NULL) v = Py_None; }
    else { if (v == NULL) v = Py_True; }
    Py_INCREF(v);
    return v;
}
PyObject *PySnip_A2(PyObject *o) { return snip_a2((char *)o, 16, 1); }''', ['PySnip_A2'],
         fn='snip_a2', items={'snip_a2': [Likely('v == NULL', False)]},
         refused='matches 2 branches of snip_a2, not 1'),
    Case('G8: an error raised on one arm: the rest of the function on that arm alone', '''
static PyObject *snip_ae(const char *addr, Py_ssize_t off) {
    PyObject *v;
    addr += off;
    v = *(PyObject **)addr;
    if (v == NULL) PyErr_SetString(PyExc_AttributeError, "x");
    Py_XINCREF(v);
    return v;
}
PyObject *PySnip_Ae(PyObject *o) { return snip_ae((char *)o, 16); }''', ['PySnip_Ae'],
         fn='snip_ae', items={'snip_ae': [Likely('v == NULL', False)]},
         want=['if (_t3) {', 'vinfo_decref(v, po);',
               'PycException_SetString(po, PyExc_AttributeError, "x");', 'return NULL;', '}',
               'need_reference(po, v);', 'return v;']),
    Case('G8: an error raised on one arm does not merge into the other', '''
static PyObject *snip_g8m(PyObject *o, int k) {
    if (k) PyErr_SetString(PyExc_TypeError, "x");
    return PyObject_Repr(o);
}
PyObject *PySnip_G8m(PyObject *o) { return snip_g8m(o, 1); }''', ['PySnip_G8m'],
         fn='snip_g8m', refused='a run-time call where an error is pending'),
    Case('G8: a value returned where an error is pending is refused', '''
PyObject *PySnip_G8r(PyObject *o) {
    PyErr_SetString(PyExc_TypeError, "x");
    Py_INCREF(o);
    return o;
}''', ['PySnip_G8r'], refused='returns a value where an error is pending'),
    Case('holds: a test the caller made, in a static it inlines, holds; the caller\'s '
         'parameter is the static\'s (no copy)', '''
static int snip_hchk(PyObject *o, PyObject **pres) {
    if (!PyLong_Check(o)) { PyErr_SetString(PyExc_TypeError, "x"); *pres = NULL; return 1; }
    return 0;
}
PyObject *PySnip_Holds(PyLongObject *o) {
    PyObject *res;
    if (snip_hchk((PyObject *)o, &res)) return res;
    return PyObject_Repr((PyObject *)o);
}''', ['PySnip_Holds'],
         items={'PySnip_Holds': [Holds('PyLong_Check(o)', 'the caller checks')]},
         want=['PyObject_Repr,'], absent=['PycException_SetString', 'snip_hchk_o', 'tp_o']),
    Case('holds: an item matching no branch is refused', '''
PyObject *PySnip_Holds0(PyObject *o) { return PyObject_Repr(o); }''', ['PySnip_Holds0'],
         items={'PySnip_Holds0': [Holds('PyLong_Check(o)', 'the caller checks')]},
         refused='matches 0 branches of PySnip_Holds0'),
    Case('likely: a run-time branch with no item on the statement\'s own call resumes '
         'from the statement', '''
PyObject *PySnip_SameStmt(PyObject *o) {
    if (PyObject_Hash(o) == 7) return PyObject_Repr(o);
    return PyObject_Str(o);
}''', ['PySnip_SameStmt'], want=['return psyco_generic_call(po, PySnip_SameStmt,'],
         absent=['Hash(po', 'PyObject_Hash,'], report="no 'likely' item for the run-time branch"),
]


def g10():
    """G10: an item the translator has no rule for is an error on load."""
    fam = scope.Family('snip', 'Objects/abstract.c', None, None, None)
    try:
        policy.load(front.target(PY37), fam, {'f': [('other', 'mutable_fields')]})
    except policy.PolicyError as e:
        return 'unknown policy item' in str(e), str(e)
    return False, 'loaded'


def builtins():
    """A module's builtins: which C function and flags from its method
    table, registered under those flags; flags Ni has no convention for
    are not attempted, and a METH_FASTCALL function whose arguments are
    not checked by a rule runs its C function."""
    import cslots
    import cpython_src
    fam = scope.Family('snipmath', 'Modules/mathmodule.c', None, 'pmath.c', 'snip_gen.h',
                       reps=('double',), builtins=('acos', 'atan2', 'pow'))
    t = front.target(PY37)
    src = cpython_src.find_source('3.7.17')
    import generate
    version, pyconfig = generate.target_info(PY37)
    text, report = abstract.generate(cslots.Target(src, version, PY37, pyconfig), fam)
    want = ['static PyCFunction cfunc_math_acos;',
            'static vinfo_t * math_acos(PsycoObject *po, vinfo_t *self, vinfo_t *args) {',
            'static void gen_mathmodule_init(PyObject *md) {',
            'cfunc_math_acos = Psyco_DefineModuleFn(md, "acos", METH_O, &math_acos);',
            'static vinfo_t * math_pow(PsycoObject *po, vinfo_t *module, vinfo_t *vargs) {',
            'return PsycoCFunction_CallFast(po, cfunc_math_pow, METH_FASTCALL, module, vargs);']
    lines = ['generated  math.acos', 'skipped    math.atan2',
             'ml_flags METH_VARARGS: no METH_ARGS entry', 'generated  math.pow',
             'restarts   math.pow', 'the argument array args as a value']
    missing = [w for w in want if w not in text] + [w for w in lines
                                                     if not any(w in r for r in report)]
    return not missing, "missing %s in %s" % (missing, report)


def fold_may_fail():
    """FoldMayFail: the str slots' contains, called while compiling when both
    strings are constants, its error left to run time."""
    import cslots
    import cpython_src
    import generate
    src = cpython_src.find_source('3.7.17')
    version, pyconfig = generate.target_info(PY37)
    text, report = abstract.generate(cslots.Target(src, version, PY37, pyconfig),
                                     scope.FAMILIES['unicode'])
    i = text.find('vinfo_t * PsycoUnicode_Contains(PsycoObject *po, vinfo_t *str, '
                  'vinfo_t *substr) {')
    want = ['if (is_compiletime(str->source) && is_compiletime(substr->source)) {',
            'int r = PyUnicode_Contains((PyObject *)CompileTime_Get(str->source)->value,'
            ' (PyObject *)CompileTime_Get(substr->source)->value);',
            'if (r >= 0)', 'return vinfo_new(CompileTime_New(r));', 'PyErr_Clear();',
            'psyco_generic_call(po, PyUnicode_Contains, CfCommonIntBoolOrError']
    at, missing = i, [] if i >= 0 else ['the meta']
    for w in want:
        k = text.find(w, at)
        if k < 0:
            missing.append(w)
            break
        at = k
    return not missing, 'missing (in order) %s' % missing


class Bind:
    """An argument binding (gen2/binding.py): BODY between a frame made
    and run, read after Python/ceval.c (its GETLOCAL and SETLOCAL); Ni's
    binder takes the code, the frame's locals, the arguments and their
    count, and a closure tuple's items; `absent` gives the C parameters it
    leaves out their values (by default no keywords)."""
    ABSENT = {'kwnames': 'NULL', 'kwcount': '0'}
    NI = [('PsycoObject *', 'po'), ('PyObject *', '_co'), ('vinfo_t **', 'f_localsplus'),
          ('vinfo_t **', 'args'), ('Py_ssize_t', 'argcount'), ('vinfo_t **', 'closure')]

    def __init__(self, name, c, want=(), absent=(), refused=None, kw=None):
        self.name, self.c = name, c
        self.want, self.absent, self.refused = want, absent, refused
        self.kw = dict(self.ABSENT, **(kw or {}))

    def text(self, k):
        return '''
PyObject *snip_bind%d(PyObject *_co, PyObject *globals, PyObject *const *args,
                     Py_ssize_t argcount, PyObject *const *kwnames, Py_ssize_t kwcount,
                     PyObject *closure) {
    PyCodeObject *co = (PyCodeObject *)_co;
    PyFrameObject *f;
    PyObject **fastlocals;
    PyObject *x, *y;
    Py_ssize_t i;
    f = _PyFrame_New_NoTrack(PyThreadState_GET(), co, globals, NULL);
    if (f == NULL) return NULL;
    fastlocals = f->f_localsplus;
%s
    return PyEval_EvalFrameEx(f, 0);
fail:
    Py_DECREF(f);
    return NULL;
}
''' % (k, self.c)


SETARG = 'x = args[%d]; Py_INCREF(x); SETLOCAL(%d, x);'
BINDS = [
    Bind("the frame's locals are Ni's array, its arguments' items vinfos, C on counts the"
         " binder's", 'for (i = 0; i < argcount; i++) { x = args[i]; Py_INCREF(x); SETLOCAL(i, x); }',
         want=['fastlocals = f_localsplus;', 'for ((i = 0); (i < argcount); i++) {',
               'x = args[i];', 'vinfo_incref(x);', 'tmp = fastlocals[i];', 'fastlocals[i] = x;',
               'vinfo_xdecref(tmp, po);', 'return 0;']),
    Bind('whether a slot holds a value is known when Ni compiles',
         'if (GETLOCAL(0) == NULL) { %s }' % SETARG % (0, 0),
         want=['if ((fastlocals[0] == NULL)) {', 'fastlocals[0] = x;']),
    Bind('restart: a call with no form is the real call, and nothing after it runs',
         'if (co->co_flags & CO_VARKEYWORDS) { y = PyDict_New(); if (y == NULL) goto fail;'
         ' SETLOCAL(0, y); }',
         want=['return -1; /* PyDict_New: the real call */'], absent=['y = ', 'fastlocals[0] = y']),
    Bind('restart (G8): an error being set is the real call; nothing after it runs',
         'if (argcount > 2) { PyErr_SetString(PyExc_TypeError, "no"); goto fail; }\n'
         + SETARG % (0, 0),
         want=['if ((argcount > 2)) {', 'return -1; /* PyErr_SetString: the real call */', '}',
               'x = args[0];'], absent=['goto fail']),
    Bind('restart: a goto out of the slice is the real call',
         'if (argcount > 2) goto fail;',
         want=['return -1; /* goto fail: the real call */']),
    Bind("absent: a parameter Ni leaves out has its value; a loop it bounds to 0 isn't made",
         'kwcount *= 2;\nfor (i = 0; i < kwcount; i++) { x = kwnames[i]; Py_INCREF(x);'
         ' SETLOCAL(i, x); }',
         want=['i = 0;', 'return 0;'], absent=['kwnames', 'kwcount', 'for (']),
    Bind("absent: where it runs, a read of what Ni doesn't pass is refused",
         'for (i = 0; i < kwcount; i++) { x = kwnames[i]; Py_INCREF(x); SETLOCAL(i, x); }',
         kw={'kwcount': '1'}, refused="reads an item of kwnames, which Ni's"),
    Bind("absent: a parameter Ni neither passes nor gives a value is refused where read",
         'if (globals == NULL) goto fail;', refused="reads globals, which Ni's"),
    Bind("G5: constants fold with C's semantics (logic gives 0 or 1, comparisons don't"
         " chain, division truncates)",
         'if (kwcount + 1 && 4 == 1) goto fail;\n'
         'if (!kwcount + 1 == 2) { %s }\n'
         'if ((kwcount - 7) / 2 == -3) { %s }\n'
         'if (1 < kwcount < 3) goto fail;' % (SETARG % (0, 0), SETARG % (1, 1)),
         want=['fastlocals[0] = x;', 'fastlocals[1] = x;',
               'return -1; /* goto fail: the real call */'],
         absent=['kwcount', 'if (']),
    Bind("virtual: a tuple of a meta size is Ni's, behind its limit, never NULL; its items"
         " stored", 'x = PyTuple_New(argcount); if (x == NULL) goto fail; SETLOCAL(0, x);\n'
         'for (i = 0; i < argcount; i++) { y = args[i]; Py_INCREF(y);'
         ' PyTuple_SET_ITEM(x, i, y); }',
         want=['_t1 = argcount;', 'if (_t1 < 0 || _t1 > 15)',
               'return -1; /* a PyTuple_Type of more than CT_TUPLE_LOAD_SIZE_LIMIT items',
               'x = PsycoTuple_New(_t1, NULL);', 'fastlocals[0] = x;',
               'PsycoTuple_GET_ITEM(x, i) = y;'], absent=['x == NULL']),
    Bind('G14: a store into a virtual under a condition stays under it',
         'x = PyTuple_New(2); SETLOCAL(0, x);\n'
         'if (argcount > 0) { y = args[0]; Py_INCREF(y); PyTuple_SET_ITEM(x, 0, y); }',
         want=['x = PsycoTuple_New(2, NULL);', 'fastlocals[0] = x;', 'if ((argcount > 0)) {',
               'PsycoTuple_GET_ITEM(x, 0) = y;', '}', 'return 0;'], absent=['_t1']),
    Bind('a tuple the C reads through its items is the array of its items',
         'for (i = 0; i < 2; i++) { x = PyTuple_GET_ITEM(closure, i); Py_INCREF(x);'
         ' SETLOCAL(i, x); }', want=['x = closure[i];', 'vinfo_incref(x);']),
    Bind('ownership (G7): a restart while a reference is held is refused',
         'x = args[0]; Py_INCREF(x); if (PyObject_IsTrue(x)) goto fail; SETLOCAL(0, x);',
         refused='a restart (PyObject_IsTrue) while x holds a reference'),
    Bind('ownership: a store of a local holding no reference is refused',
         'x = args[0]; SETLOCAL(0, x);', refused='stores x, which holds no reference'),
    Bind('ownership: paths meeting with different references are refused',
         'x = args[0]; if (argcount > 1) Py_INCREF(x); SETLOCAL(0, x);',
         refused='holding different references'),
    Bind('ownership: the end of the binding while a reference is held is refused',
         'x = args[0]; Py_INCREF(x);', refused='the end of the binding while x holds'),
    Bind('a local nothing reads is dropped with its assignments',
         'PyObject *kwdict;\nif (co->co_flags & CO_VARKEYWORDS) goto fail; else kwdict = NULL;',
         want=['return -1; /* goto fail', 'return 0;'], absent=['kwdict', 'else']),
    Bind('a return inside the binding is refused', 'if (argcount > 3) return NULL;',
         refused='a return inside the binding'),
    Bind('a goto inside the slice goes to its label, the paths merged there',
         'if (argcount == 0) goto done;\n%s\ndone: ;' % SETARG % (0, 0),
         want=['if ((argcount == 0)) {', 'goto done;', 'fastlocals[0] = x;', 'done:;',
               'return 0;']),
]


def bindings(cases):
    """The Bind cases: (failures, how many)."""
    from gen2 import binding
    from gen2.ctyping import Unit
    t = front.target(PY37)
    ast = front.parse(t, 'Python/ceval.c', ''.join(c.text(k) for k, c in enumerate(cases)))
    unit = Unit(ast)
    fails = 0
    for k, c in enumerate(cases):
        decl = conventions.NiDecl('_PsycoSnip_Bind%d' % k, 'int', c.NI)
        try:
            meta, why = binding.Binder(unit, unit.defs['snip_bind%d' % k], decl, c.kw).run(), None
        except binding.Refuse as e:
            meta, why = None, str(e)
        problem = None
        if c.refused is not None:
            if why is None or c.refused not in why:
                problem = 'want refused %r, got %s' % (c.refused, why or 'a binder')
        elif meta is None:
            problem = 'refused: %s' % why
        else:
            at = 0
            for w in c.want:
                j = meta.find(w, at)
                if j < 0:
                    problem = 'missing (in order) %r' % w
                    break
                at = j + len(w)
            body = meta.split('{', 1)[1]
            for a in c.absent:
                if a in body:
                    problem = 'has %r' % a
        if problem:
            fails += 1
            print('FAIL binding: %s: %s' % (c.name, problem))
            if meta:
                print(meta)
        else:
            print('ok   binding: %s' % c.name)
    return fails


class Op:
    """An opcode case (gen2/opcodes.py): BODY as a case of a switch on the
    opcode like the eval loop's, read after Python/ceval.c (its stack and
    dispatch macros); `effect` the stack effect dis would give, checked
    when set."""

    def __init__(self, name, c, want=(), absent=(), refused=None, effect=None):
        self.name, self.c = name, c
        self.want, self.absent, self.refused, self.effect = want, absent, refused, effect

    @staticmethod
    def text(cases):
        body = ''.join('case %d: {\n%s\n}\n' % (1000 + k, c.c) for k, c in enumerate(cases))
        return '''
void snip_eval(PyFrameObject *f, PyThreadState *tstate) {
    PyObject **stack_pointer, **fastlocals, **freevars, *consts, *names;
    PyCodeObject *co;
    const _Py_CODEUNIT *first_instr, *next_instr;
    int opcode, oparg;
    for (;;) {
    fast_next_opcode:
        switch (opcode) {
%s
        }
    error:
        ;
    }
}
''' % body


OPS = [
    Op('opcodes: a rotation pops the inputs and pushes them back, releasing none', '''
PyObject *top = TOP(); PyObject *second = SECOND();
SET_TOP(second); SET_SECOND(top); FAST_DISPATCH();''',
       want=['top = NTOP(1);', 'second = NTOP(2);', 'POP(top);', 'POP(second);',
             'PUSH(top);', 'PUSH(second);', 'goto fine;'], absent=['vinfo_decref'], effect=0),
    Op("opcodes: a meta's result replaces the inputs; the error leaves them on the stack", '''
PyObject *right = POP(); PyObject *left = TOP();
PyObject *res = PyNumber_Multiply(left, right);
Py_DECREF(left); Py_DECREF(right);
SET_TOP(res); if (res == NULL) goto error; DISPATCH();''',
       want=['PsycoNumber_Multiply(po, left, right);', 'if (res == NULL) {', 'break;',
             'POP(right);', 'POP(left);', 'vinfo_decref(right, po);',
             'vinfo_decref(left, po);', 'PUSH(res);'],
       absent=['vinfo_decref(res'], effect=-1),
    Op('opcodes: an incref of an input pushes a new reference, the input stays', '''
PyObject *top = TOP(); Py_INCREF(top); PUSH(top); FAST_DISPATCH();''',
       want=['vinfo_incref(top);', 'PUSH(top);'], absent=['POP('], effect=1),
    Op('opcodes: SETLOCAL takes the input, which is then popped', '''
PyObject *value = POP(); SETLOCAL(oparg, value); FAST_DISPATCH();''',
       want=['SETLOCAL(oparg, value);', 'POP(value);'], absent=['vinfo_decref'], effect=-1),
    Op("opcodes: an item of co_consts is Ni's constant, made where it is used", '''
PyObject *value = GETITEM(consts, oparg); Py_INCREF(value); PUSH(value); FAST_DISPATCH();''',
       want=['value = vinfo_new(CompileTime_New((long)GETCONST(oparg)));', 'PUSH(value);'],
       absent=['vinfo_incref'], effect=1),
    Op("opcodes: a name and None as arguments, each released after the call", '''
PyObject *owner = TOP();
PyObject *res = PyNumber_Power(owner, GETITEM(names, oparg), Py_None);
Py_DECREF(owner); SET_TOP(res); if (res == NULL) goto error; DISPATCH();''',
       want=['= vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));', '= psyco_vi_None();',
             'PsycoNumber_Power(po, owner, _t1, _t2);', 'vinfo_decref(_t1, po);',
             'vinfo_decref(_t2, po);', 'if (res == NULL)', 'POP(owner);'], effect=0),
    Op("opcodes: a meta's status kept in an int is its bool", '''
PyObject *v = POP(); PyObject *list = PEEK(oparg);
int err = PyList_Append(list, v); Py_DECREF(v);
if (err != 0) goto error; PREDICT(JUMP_ABSOLUTE); DISPATCH();''',
       want=['list = NTOP(oparg + 1);', 'err = PsycoList_Append(po, list, v);', 'if (!(err)) {',
             'break;', 'POP(v);', 'vinfo_decref(v, po);'],
       absent=['PRED_', 'vinfo_t *err'], effect=-1),
    Op('opcodes: an error after an input was stored elsewhere is refused', '''
PyObject *value = POP(); PyObject *x = PyNumber_Negative(value); SETLOCAL(oparg, value);
if (x == NULL) goto error; PUSH(x); DISPATCH();''', refused='was moved'),
    Op("opcodes: a case reading the frame is the interpreter's", '''
PyObject *x = f->f_locals; Py_INCREF(x); PUSH(x); DISPATCH();''', refused='core'),
    Op("opcodes: the stack pointer handed to a call is the interpreter's", '''
PyObject *res = call_function(&stack_pointer, oparg, NULL); PUSH(res); DISPATCH();''',
       refused='core'),
    Op("opcodes: a pushed value read back from the stack is refused", '''
PyObject *x = PyNumber_Negative(TOP()); if (x == NULL) goto error; PUSH(x);
PyObject *y = TOP(); Py_INCREF(y); PUSH(y); DISPATCH();''', refused='read back'),
    Op('opcodes: an input both released and pushed is consumed twice, refused', '''
PyObject *top = TOP(); Py_DECREF(top); STACKADJ(-1); PUSH(top); DISPATCH();''',
       refused='read with no value'),
    Op('opcodes: the stack effect is checked against dis.stack_effect', '''
PyObject *top = TOP(); Py_INCREF(top); PUSH(top); FAST_DISPATCH();''', effect=0,
       refused='dis.stack_effect says'),
]


def ops(cases):
    """The Op cases: failures."""
    from gen2 import opcodes
    t = front.target(PY37)
    ast = front.parse(t, 'Python/ceval.c', Op.text(cases))
    ni = conventions.ni_decls(abstract.ROOT, scope.NI_HEADERS)
    tr = abstract.Translator(scope.CEVAL, ast, ni, set(), {}, None)
    names = {'SNIP_%d' % k: 1000 + k for k in range(len(cases))}
    bodies = dict(opcodes.cases(ast, names, 'snip_eval'))
    fails = 0
    for k, c in enumerate(cases):
        name = 'SNIP_%d' % k
        try:
            meta, effects = opcodes.translate(tr, name, opcodes.case_function(name,
                                                                              bodies[name]))
            why = None
            if c.effect is not None:
                why = opcodes.check_effect(name, effects, {name: [c.effect] * len(
                    opcodes.OPARGS)})
                if why is None and c.refused is not None:
                    why = 'no problem: effects %s' % effects
                meta = None if why else meta
        except opcodes.Refuse as e:
            meta, why = None, str(e)
        problem = None
        if c.refused is not None:
            if why is None or c.refused not in why:
                problem = 'want refused %r, got %s' % (c.refused, why or 'a case')
        elif meta is None:
            problem = 'refused: %s' % why
        else:
            at = 0
            for w in c.want:
                j = meta.find(w, at)
                if j < 0:
                    problem = 'missing (in order) %r' % w
                    break
                at = j + len(w)
            for a in c.absent:
                if a in meta:
                    problem = 'has %r' % a
        if problem:
            fails += 1
            print('FAIL %s: %s' % (c.name, problem))
            if meta:
                print(meta)
        else:
            print('ok   %s' % c.name)
    return fails


FILES = {'abstract': ('Objects/abstract.c', ()), 'bltin': ('Objects/abstract.c', ()),
         'long': ('Objects/longobject.c', ('word', 'double')),
         'float': ('Objects/floatobject.c', ('double',)),
         'tuple': ('Objects/tupleobject.c', ('known_length',))}


def run(cases):
    t = front.target(PY37)
    ni = conventions.ni_decls(abstract.ROOT, scope.NI_HEADERS)
    fails = 0
    parsed = {}
    for f, (cfile, reps) in FILES.items():
        mine = [c for c in cases if c.file == f]
        if not mine:
            continue
        methods = [m for c in mine for m in c.methods]
        fam = scope.Family('snip', cfile, 'snip.h', None, None, (), reps,
                           tuple(py for py, _, _ in methods))
        extra = types.rule_text(reps) + '\n'.join(c.c for c in mine)
        if methods:
            # the builtins' module: one method table for every case's
            from gen2 import slots
            extra += '\nstatic PyMethodDef snip_methods[] = {%s {NULL, NULL}};\n' % ''.join(
                '{"%s", (PyCFunction)%s, %s, NULL},' % m for m in methods)
            extra += ('static struct PyModuleDef snipmodule = {PyModuleDef_HEAD_INIT, "snip",'
                      ' NULL, -1, snip_methods};\n' + slots.meth_text())
        items = {k: v for c in mine for k, v in c.items.items()}
        parsed[f] = (fam, front.parse(t, cfile, extra), policy.load(t, fam, items, extra))
    for c in cases:
        fam, ast, loaded = parsed[c.file]
        declared = {conventions.psyco_name(r) for r in c.roots}
        tr = abstract.Translator(fam, ast, ni, declared, loaded, None, own='<rules>')
        tr.run()
        meta = tr.metas.get(c.fn)
        why = tr.fails.get(c.fn)
        reasons = [r[2] for r in tr.resumes if r[0] == c.fn]
        problem = None
        if c.refused is not None:
            if why is None or c.refused not in why:
                problem = 'want refused %r, got %s' % (c.refused, why or 'a meta')
        elif meta is None:
            problem = 'refused: %s' % why
        else:
            at = 0
            for w in c.want:
                k = meta.find(w, at)
                if k < 0:
                    problem = 'missing (in order) %r' % w
                    break
                at = k + len(w)
            for a in c.absent:
                if a in meta:
                    problem = 'has %r' % a
            copies = ''.join(t for n, t in tr.copies.texts.items() if c.fn in n)
            for x in c.copy:
                if x not in copies:
                    problem = 'no %r in its copies' % x
            for x in c.absent_copy:
                if x in copies:
                    problem = 'has %r in its copies' % x
            if c.report and not any(c.report in r for r in reasons):
                problem = 'no resume for %r (resumes: %s)' % (c.report, reasons)
        if problem:
            fails += 1
            print('FAIL %s: %s' % (c.name, problem))
            if reasons:
                print('resumes: %s' % reasons)
            if meta:
                print(meta)
            for n, t in tr.copies.texts.items():
                if c.fn in n:
                    print(t)
        else:
            print('ok   %s' % c.name)
    ok, msg = g10()
    print('%s G10: an unknown policy item is refused on load%s' % (
        'ok  ' if ok else 'FAIL', '' if ok else ': ' + msg))
    ok2, msg = builtins()
    print('%s builtins: from the method table, registered under their flags%s' % (
        'ok  ' if ok2 else 'FAIL', '' if ok2 else ': ' + msg))
    ok3, msg = fold_may_fail()
    print('%s fold-may-fail: called while compiling on constants, its error left%s' % (
        'ok  ' if ok3 else 'FAIL', '' if ok3 else ': ' + msg))
    return fails + (not ok) + (not ok2) + (not ok3)


def main(argv):
    cases = [c for c in CASES if not argv or any(a in c.name for a in argv)]
    binds = [c for c in BINDS if not argv or any(a in 'binding: ' + c.name for a in argv)]
    opcs = [c for c in OPS if not argv or any(a in c.name for a in argv)]
    fails = run(cases) if cases or not (binds or opcs) else 0
    fails += bindings(binds) if binds else 0
    fails += ops(opcs) if opcs else 0
    n = (len(cases) + 3 if cases or not (binds or opcs) else 0) + len(binds) + len(opcs)
    print('%d of %d snippets pass' % (n - fails, n))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
