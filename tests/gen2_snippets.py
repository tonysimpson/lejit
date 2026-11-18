#!/usr/bin/env python3
"""Snippet tests for the new translator (tools/metac/gen2): small C in,
the generated meta (or the refusal) out. The C is read after
Objects/abstract.c, in its scope, so it has CPython 3.7's types; a test's
functions are translated as a family whose metas Ni declares as the test
says. Needs gcc and CPython 3.7's source; no build.

    gen2_snippets.py [NAME...]

Exits 1 if any snippet gives another result, naming it.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'metac'))
import front  # noqa: E402
from gen2 import abstract, conventions, policy, scope  # noqa: E402
from gen2.policy import Likely, Restype  # noqa: E402

PY37 = os.environ.get('NI_PY37', os.path.expanduser('~/.pyenv/versions/3.7.17/bin/python3.7'))


class Case:
    """C, the functions whose metas Ni declares (roots), policy items,
    and what must come out: `want` in order in the named function's meta,
    `absent` nowhere in it, or `refused` in its refusal; `report` in its
    resume lines, `copy` in its run-time copies."""

    def __init__(self, name, c, roots, fn=None, items=None, want=(), absent=(),
                 refused=None, report=None, copy=()):
        self.name, self.c, self.roots = name, c, roots
        self.fn = fn or roots[0]
        self.items = items or {}
        self.want, self.absent, self.refused, self.report = want, absent, refused, report
        self.copy = copy


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
    Case('likely: a run-time branch with no item is refused', '''
PyObject *PySnip_NoLikely(PyObject *o, Py_ssize_t i) {
    if (i < 0) return PyObject_Repr(o);
    return PySequence_GetItem(o, i);
}''', ['PySnip_NoLikely'], refused="no 'likely' item for the run-time branch"),
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
    # ---- the old generator's bugs, the correct behaviour ----
    Case('G6: no resume starts inside a loop the meta runs (the meta runs none)', '''
PyObject *PySnip_G6(PyObject *o, Py_ssize_t n) {
    PyObject *r = PyObject_Repr(o);
    if (r == NULL) return NULL;
    while (n-- > 0) { if (o->ob_type->tp_as_number == NULL) break; }
    return r;
}''', ['PySnip_G6'], want=['cimpl_PySnip_G6_1'], absent=['Psyco_TypeSlot', 'while'],
         report='a loop with a run-time bound'),
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
]


def g10():
    """G10: an item the translator has no rule for is an error on load."""
    fam = scope.Family('snip', 'Objects/abstract.c', None, None, None)
    try:
        policy.load(front.target(PY37), fam, {'f': [('other', 'mutable_fields')]})
    except policy.PolicyError as e:
        return 'unknown policy item' in str(e), str(e)
    return False, 'loaded'


def run(cases):
    t = front.target(PY37)
    fam = scope.Family('snip', 'Objects/abstract.c', 'snip.h', None, None)
    extra = '\n'.join(c.c for c in cases)
    items = {k: v for c in cases for k, v in c.items.items()}
    loaded = policy.load(t, fam, items, extra)
    ast = front.parse(t, fam.cfile, extra)
    ni = conventions.ni_decls(abstract.ROOT, scope.NI_HEADERS)
    fails = 0
    for c in cases:
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
            if c.report and not any(c.report in r for r in reasons):
                problem = 'no resume for %r (resumes: %s)' % (c.report, reasons)
        if problem:
            fails += 1
            print('FAIL %s: %s' % (c.name, problem))
            if reasons:
                print('resumes: %s' % reasons)
            if meta:
                print(meta)
        else:
            print('ok   %s' % c.name)
    ok, msg = g10()
    print('%s G10: an unknown policy item is refused on load%s' % (
        'ok  ' if ok else 'FAIL', '' if ok else ': ' + msg))
    return fails + (not ok)


def main(argv):
    cases = [c for c in CASES if not argv or any(a in c.name for a in argv)]
    fails = run(cases)
    print('%d of %d snippets pass' % (len(cases) + 1 - fails, len(cases) + 1))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
