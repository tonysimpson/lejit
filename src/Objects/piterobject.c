/*
 * Virtual sequence iterators. Layout: [iOB_TYPE], [iSEQITER_IT_INDEX] (made
 * run-time at once, so the compiler does not specialise on the loop counter),
 * [iSEQITER_IT_SEQ]. piter_iternext is a translation of iter_iternext
 * (IndexError or StopIteration ends it and drops it_seq) with a
 * shortcut for lists. it_seq is cached like an immutable field, so a
 * run-time iterator's is checked against memory (see piter_iternext).
 * List iterators (listiter_next) come here too: plistobject.c redirects
 * list's tp_iter to a virtual one of type list_iterator, whose layout is
 * seqiterobject's (checked by lejit.c:le_layouts). A list iterator ends at
 * the list's size, with no IndexError, and is finished by CPython's own
 * listiter_next, which leaves its exhausted state as CPython does.
 * seqiterobject is re-declared in the header because CPython keeps it private.
 */
#include "piterobject.h"
#include "plistobject.h"
#include "punicodeobject.h"
#include "../psyfunc.h"
/* for plist_getitem */
#include "../initialize.h"

static vinfo_t *
new_iter(PsycoObject *po, vinfo_t *seq, PyTypeObject *tp)
{
    vinfo_t *zero;
    vinfo_t *result = vinfo_new(VirtualTime_New(&psyco_computed_seqiter));
    result->array = array_new(SEQITER_TOTAL);
    result->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)tp));
    /* the iterator index is immediately run-time because it is
       very likely to be unpromoted to run-time anyway */
    zero = psyco_vi_Zero();
    result->array->items[iSEQITER_IT_INDEX] = make_runtime_copy(po, zero);
    vinfo_decref(zero, po);
    /*result->array->items[SEQITER_IT_INDEX] =
        vinfo_new(CompileTime_New(0));*/
    result->array->items[iSEQITER_IT_SEQ] = seq;
    return result;
}

DEFINEFN vinfo_t *
PsycoSeqIter_NEW(PsycoObject *po, vinfo_t *seq)
{
    return new_iter(po, seq, &PySeqIter_Type);
}

DEFINEFN vinfo_t *
PsycoListIter_NEW(PsycoObject *po, vinfo_t *list)
{
    return new_iter(po, list, &PyListIter_Type);
}

static bool
is_listiter(vinfo_t *v)
{
    return (PyTypeObject *)CompileTime_Get(v->array->items[iOB_TYPE]->source)
               ->value == &PyListIter_Type;
}

/* a list iterator at its list's end: CPython's listiter_next finishes it */
static void
cimpl_listiter_finish(PyObject *it)
{
    PyObject *r = Py_TYPE(it)->tp_iternext(it);
    extra_assert(r == NULL && !PyErr_Occurred());
    (void)r;
}

/* An exhausted iterator has dropped its sequence (it_seq = NULL). */
static void
cimpl_seqiter_drop_seq(seqiterobject *it)
{
    Py_CLEAR(it->it_seq);
}

static PyObject *
cimpl_listiter_exhausted(void)
{
    PyObject *empty = PyList_New(0), *it;
    if (empty == NULL)
        return NULL;
    it = PyObject_GetIter(empty);
    Py_DECREF(empty);
    if (it != NULL)
        cimpl_listiter_finish(it);
    return it;
}

static PyObject *
cimpl_seqiter_exhausted(void)
{
    PyObject *empty = PyTuple_New(0);
    seqiterobject *it;
    if (empty == NULL)
        return NULL;
    it = (seqiterobject *)PySeqIter_New(empty);
    Py_DECREF(empty);
    if (it != NULL)
        cimpl_seqiter_drop_seq(it);
    return (PyObject *)it;
}

static vinfo_t *
piter_stop(PsycoObject *po)
{
    PycException_SetVInfo(po, PyExc_StopIteration, psyco_vi_None());
    return NULL;
}

/* An IndexError or StopIteration from the sequence ends the iteration:
   true if the exception set in 'po' is one, false if not, -1 on error. */
static int
piter_ends(PsycoObject *po)
{
    /* runtime_NON_NULL_x consume the reference */
    int r = runtime_NON_NULL_t(po, PycException_Matches(po, PyExc_IndexError));
    if (r != false)
        return r;
    return runtime_NON_NULL_f(po,
                              PycException_Matches(po, PyExc_StopIteration));
}

/* iterobject.c:iter_iternext. it_seq is cached in v->array as if it were
   immutable, so that the sequence's type stays known. A virtual iterator
   holds its sequence, or a compile-time NULL once exhausted. A
   run-time one may have been exhausted by anyone: a fresh load is
   tested for NULL, and a cached one against memory, a mismatch going to
   CPython's iter_iternext. Once NULL, it_seq never changes again. */
static vinfo_t *
piter_iternext(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *seq;
    vinfo_t *index;
    vinfo_t *result;
    PyTypeObject *tp;
    bool cached, list;

    /* a constant iterator's fields change under the compiled code */
    if (is_compiletime(v->source)) {
        PyObject *it = (PyObject *)CompileTime_Get(v->source)->value;
        return psyco_generic_call(po, Py_TYPE(it)->tp_iternext,
                                  CfCommonIterNext, "v", v);
    }
    cached =
        is_virtualtime(v->source) || vinfo_getitem(v, iSEQITER_IT_SEQ) != NULL;
    list = is_listiter(v);

    if (vinfo_getitem(v, iSEQITER_SPLIT_POS) != NULL) {
        seq = vinfo_getitem(v, iSEQITER_IT_SEQ);
        if (psyco_knowntobe(seq, 0))
            return piter_stop(po);
        return PsycoUnicode_SplitNext(po, v, seq);
    }
    seq = psyco_get_const(po, v, SEQITER_it_seq);
    if (seq == NULL)
        return NULL;
    if (psyco_knowntobe(seq, 0))
        return piter_stop(po);
    if (!is_virtualtime(v->source) && is_runtime(seq->source)) {
        condition_code_t cc;
        if (cached) {
            vinfo_t *now = psyco_get_field(po, v, FMUT(SEQITER_it_seq));
            if (now == NULL)
                return NULL;
            cc = integer_cmp(po, now, seq, Py_NE);
            vinfo_decref(now, po);
        }
        else
            cc = integer_non_null(po, seq);
        if (cc == CC_ERROR)
            return NULL;
        if (!cached)
            cc = INVERT_CC(cc);
        if (runtime_condition_f(po, cc)) {
            if (!cached) {
                vinfo_setitem(po, v, iSEQITER_IT_SEQ, psyco_vi_Zero());
                return piter_stop(po);
            }
            psyco_forget_field(po, v, SEQITER_it_seq);
            return psyco_generic_call(po,
                                      list ? PyListIter_Type.tp_iternext
                                           : PySeqIter_Type.tp_iternext,
                                      CfCommonIterNext, "v", v);
        }
    }

    tp = Psyco_NeedType(po, seq);
    if (tp == NULL)
        return NULL;

    index = psyco_get_field(po, v, SEQITER_it_index);
    if (index == NULL)
        return NULL;
    if (list && !is_virtualtime(v->source)) {
        /* an exhausted list iterator may keep its list (its index is -1
           where __setstate__(-1) put it) */
        condition_code_t cc = integer_cmp_i(po, index, 0, Py_LT);
        if (cc == CC_ERROR) {
            vinfo_decref(index, po);
            return NULL;
        }
        if (runtime_condition_f(po, cc)) {
            vinfo_decref(index, po);
            return psyco_generic_call(po, PyListIter_Type.tp_iternext,
                                      CfCommonIterNext, "v", v);
        }
    }
    assert_nonneg(index);

    if (PyType_IsSubtype(tp, &PyList_Type)) {
        /* If the sequence is a list, explicitly ignore a
           user-overridden __getitem__ slot, as CPython's
           list iterator does. */
        result = plist_item(po, seq, index);
    }
    else {
        result = PsycoSequence_GetItem(po, seq, index);
    }
    if (result == NULL && piter_ends(po) == true) {
        /* as CPython: clear the error, then drop the sequence */
        PycException_Clear(po);
        if (is_virtualtime(v->source)) {
            vinfo_t *zero = psyco_vi_Zero();
            bool ok = psyco_put_field(po, v, SEQITER_it_seq, zero);
            vinfo_decref(zero, po);
            if (!ok)
                goto fail;
        }
        else {
            if (psyco_generic_call(po,
                                   list ? (void *)cimpl_listiter_finish
                                        : (void *)cimpl_seqiter_drop_seq,
                                   CfNoReturnValue, "v", v) == NULL)
                goto fail;
            vinfo_setitem(po, v, iSEQITER_IT_SEQ, psyco_vi_Zero());
        }
        piter_stop(po);
    }
    else {
        vinfo_t *index_plus_1 = integer_add_i(po, index, 1, true);
        if (index_plus_1 == NULL ||
            !psyco_put_field(po, v, SEQITER_it_index, index_plus_1)) {
            vinfo_decref(result, po);
            result = NULL;
        }
        vinfo_xdecref(index_plus_1, po);
    }
fail:
    vinfo_decref(index, po);
    return result;
}

static bool
compute_seqiter(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *seq;
    vinfo_t *index;
    vinfo_t *newobj;

    index = vinfo_getitem(v, iSEQITER_IT_INDEX);
    if (index == NULL)
        return false;

    seq = vinfo_getitem(v, iSEQITER_IT_SEQ);
    if (seq == NULL)
        return false;

    if (psyco_knowntobe(seq, 0)) {
        /* exhausted: its index is never read again */
        newobj = psyco_generic_call(po,
                                    is_listiter(v) ? cimpl_listiter_exhausted
                                                   : cimpl_seqiter_exhausted,
                                    CfCommonNewRefPyObject, "");
        if (newobj == NULL)
            return false;
        psyco_forget_field(po, v, SEQITER_it_index);
        vinfo_array_shrink(po, v, SEQITER_TOTAL);
        vinfo_move(po, v, newobj);
        return true;
    }

    newobj = psyco_generic_call(
        po, is_listiter(v) ? PyObject_GetIter : PySeqIter_New,
        CfCommonNewRefPyObject, "v", seq);
    if (newobj == NULL)
        return false;

    /* Put the current index into the seq iterator.
       This is done by putting the value directly in the
       seqiterobject structure; it could be done by calling
       PyIter_Next() n times but obviously that's not too
       good a solution */
    if (!psyco_knowntobe(index, 0)) {
        if (!psyco_put_field(po, newobj, SEQITER_it_index, index)) {
            vinfo_decref(newobj, po);
            return false;
        }
    }

    /* Remove the SEQITER_IT_INDEX entry from v->array because it
       is a mutable field now, and could be changed at any time by
       anybody .*/
    psyco_forget_field(po, v, SEQITER_it_index);
    vinfo_array_shrink(po, v, SEQITER_TOTAL);

    vinfo_move(po, v, newobj);
    return true;
}

DEFINEVAR source_virtual_t psyco_computed_seqiter;

/* compiled code's step of a generator resumes it directly
   (psyfunc.c:psyco_gen_step) */
static vinfo_t *
pgen_iternext(PsycoObject *po, vinfo_t *v)
{
    return psyco_gen_step(po, v);
}

/* and send() the same way; coroutines share its C function */
static PyCFunction cfunc_gen_send;

static vinfo_t *
pgen_send(PsycoObject *po, vinfo_t *self, vinfo_t *arg)
{
    PyTypeObject *tp = Psyco_NeedType(po, self);
    if (tp == NULL)
        return NULL;
    if (tp == &PyGen_Type)
        return psyco_gen_send(po, self, arg, cfunc_gen_send);
    return psyco_generic_call(po, cfunc_gen_send, CfCommonNewRefPyObject, "vv",
                              self, arg);
}

INITIALIZATIONFN
void
psy_iterobject_init(void)
{
    Psyco_DefineMeta(PySeqIter_Type.tp_iternext, &piter_iternext);
    Psyco_DefineMeta(PyListIter_Type.tp_iternext, &piter_iternext);
    Psyco_DefineMeta(PyGen_Type.tp_iternext, &pgen_iternext);
    cfunc_gen_send =
        Psyco_DefineMethodFn(&PyGen_Type, "send", METH_O, &pgen_send);

    /* iterator object are mutable;
       they must be forced out of virtual-time across function calls */
    INIT_SVIRTUAL_NOCALL(psyco_computed_seqiter, compute_seqiter, 1);
}
