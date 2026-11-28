/*
 * Virtual lists, for short lists only (length <= VLIST_LENGTH_MAX, i.e. 3).
 * Layout: [iOB_TYPE], [iVAR_SIZE] = length, [LIST_ob_item] (unset while
 * virtual), then the items from VLIST_ITEMS. Lists are mutable, so they are
 * forced out of virtual time at any call boundary (INIT_SVIRTUAL_NOCALL) and
 * whenever they are indexed with a run-time index. compute_vlist emits
 * PyList_New and the item stores, then forgets the size, which becomes
 * mutable. The list tp_iter slot is redirected to a virtual list_iterator
 * (piterobject.c).
 *
 * Pieces lists: an exact list's slice, or a concatenation of slices, whole
 * lists and items, not made yet (psyco_computed_pieces, below).
 */
#include "plistobject.h"
#include "plongobject.h"
#include "piterobject.h"
#include "punicodeobject.h"
#include "psliceobject.h"
#include "../pycodegen.h"
#include "../initialize.h"

/***************************************************************/
/* virtual short lists.                                        */

/*DEFINEVAR vinfo_t* psyco_empty_list;*/
static source_virtual_t psyco_computed_vlist;

PSY_INLINE int
vlist_length(vinfo_t *v)
{
    int len = v->array->count - VLIST_ITEMS;
#if ALL_CHECKS
    vinfo_t *vlen = vinfo_getitem(v, iVAR_SIZE);
    extra_assert(vlen != NULL);
    extra_assert(len == CompileTime_Get(vlen->source)->value);
#endif
    return len;
}

static bool
compute_vlist(PsycoObject *po, vinfo_t *v)
{
    int length = vlist_length(v);
    vinfo_t *newobj;
    vinfo_t *ob_item;

    newobj = psyco_generic_call(po, PyList_New, CfCommonNewRefPyObject, "l",
                                (long)length);
    if (newobj == NULL)
        return false;

    /* write the storing of the objects in the list */
    if (length > 0) {
        int i;
        ob_item = psyco_get_field(po, newobj, LIST_ob_item);
        if (ob_item == NULL)
            goto fail;

        for (i = 0; i < length; i++) {
            vinfo_t *vi = v->array->items[VLIST_ITEMS + i];
            extra_assert(vi != NULL);
            if (!psyco_put_nth_field(po, ob_item, FPYREF(LIST_itemsarray), i,
                                     vi))
                goto fail2;
        }
        vinfo_decref(ob_item, po);
    }

    /* forget fields that were only relevant in virtual-time */
    vinfo_array_shrink(po, v, LIST_TOTAL);
    psyco_forget_field(po, v, VAR_size);

    /* move the resulting non-virtual Python object back into 'v' */
    vinfo_move(po, v, newobj);
    return true;

fail2:
    vinfo_decref(ob_item, po);
fail:
    vinfo_decref(newobj, po);
    return false;
}

PSY_INLINE vinfo_t *
list_maybe_compute(PsycoObject *po, vinfo_t *v)
{
    /* force a list out of virtual-time if it is too long.
       decref v in case of error. */
    if (vlist_length(v) > VLIST_LENGTH_MAX) {
        if (!compute_vinfo(v, po)) {
            /* error */
            vinfo_decref(v, po);
            v = NULL;
        }
    }
    return v;
}

PSY_INLINE vinfo_t *
PsycoList_NEW(int size)
{
    /* returns a virtual list. The caller should use list_maybe_compute()
       after initialization. */
    vinfo_t *r = vinfo_new(VirtualTime_New(&psyco_computed_vlist));
    r->array = array_new(VLIST_ITEMS + size);
    r->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)(&PyList_Type)));
    r->array->items[iVAR_SIZE] =
        vinfo_new(CompileTime_NewSk(sk_new(size, SkFlagFixed)));
    return r;
}

DEFINEFN
vinfo_t *
PsycoList_New(PsycoObject *po, int size, vinfo_t **source)
{
    vinfo_t *v;
    int i;
    vinfo_t *r = PsycoList_NEW(size);
    extra_assert(source != NULL);
    for (i = 0; i < size; i++) {
        v = source[i];
        vinfo_incref(v);
        r->array->items[VLIST_ITEMS + i] = v;
    }
    return list_maybe_compute(po, r);
}

DEFINEFN
int
PsycoList_Load(vinfo_t *list)
{
    /* only for virtual lists */
    if (list->source == VirtualTime_New(&psyco_computed_vlist))
        return vlist_length(list);
    else
        return -1;
}

/***************************************************************/
/* pieces lists.                                               */

/* A list CPython would make from up to PIECES_MAX pieces: slices of exact
   lists (a source, its start and stop as PySlice_Unpack gives them, a step
   of 1 or -1), whole exact lists, or single items (step 0). The pieces read
   their sources when the list is made, not when it is taken, so it stays
   virtual only while no code can run that changes a list
   (PsycoList_ForcePieces, and pycompiler.c:keeps_pieces); its items are
   then CPython's. A source is one a local variable holds, so a piece keeps
   it alive no longer than CPython does. Stored into a slice of an exact
   list (plist_ass_subscript), its items go straight in; anywhere else it is
   made as one list. */
#define PIECES_MAX 4
#define PIECE_SRC 0
#define PIECE_START 1
#define PIECE_STOP 2
#define PIECE_STEP 3
#define PIECE_WORDS 4
#define PIECES_FIRST LIST_TOTAL
#define PIECE(v, k, f) \
    ((v)->array->items[PIECES_FIRST + (k) * PIECE_WORDS + (f)])

static source_virtual_t psyco_computed_pieces;

PSY_INLINE int
is_pieces(vinfo_t *v)
{
    return v->source == VirtualTime_New(&psyco_computed_pieces);
}

PSY_INLINE int
pieces_count(vinfo_t *v)
{
    return (v->array->count - PIECES_FIRST) / PIECE_WORDS;
}

PSY_INLINE long
piece_step(vinfo_t *v, int k)
{
    return CompileTime_Get(PIECE(v, k, PIECE_STEP)->source)->value;
}

/* the items being gathered for a list, borrowed by no one: each holds a
   new reference until the list or the slice assignment takes it */
static PyObject **gathered = NULL;
static Py_ssize_t gathered_count = 0, gathered_size = 0;

static void
gathered_clear(void)
{
    while (gathered_count > 0) Py_DECREF(gathered[--gathered_count]);
}

static int
gathered_room(Py_ssize_t n)
{
    Py_ssize_t size = gathered_size;
    PyObject **a;
    if (gathered_count + n <= gathered_size)
        return 0;
    while (size < gathered_count + n) size = size ? size * 2 : 64;
    a = PyMem_Realloc(gathered, size * sizeof(PyObject *));
    if (a == NULL) {
        gathered_clear();
        PyErr_NoMemory();
        return -1;
    }
    gathered = a;
    gathered_size = size;
    return 0;
}

/* list_subscript's slice of src (an exact list): its items appended */
static int
cimpl_gather(PyObject *src, Py_ssize_t start, Py_ssize_t stop, long step)
{
    Py_ssize_t n =
        PySlice_AdjustIndices(PyList_GET_SIZE(src), &start, &stop, step);
    PyObject **items = ((PyListObject *)src)->ob_item;
    if (gathered_room(n) < 0)
        return -1;
    for (; n > 0; n--, start += step) {
        Py_INCREF(items[start]);
        gathered[gathered_count++] = items[start];
    }
    return 0;
}

static int
cimpl_gather_item(PyObject *o)
{
    if (gathered_room(1) < 0)
        return -1;
    Py_INCREF(o);
    gathered[gathered_count++] = o;
    return 0;
}

/* what list_concat and list_slice make: PyList_New(n) holding them */
static PyObject *
cimpl_gathered_list(void)
{
    PyObject *r = PyList_New(gathered_count);
    if (r == NULL) {
        gathered_clear();
        return NULL;
    }
    if (gathered_count > 0)
        memcpy(((PyListObject *)r)->ob_item, gathered,
               gathered_count * sizeof(PyObject *));
    gathered_count = 0;
    return r;
}

/* list_ass_subscript's step-1 case, list_ass_slice, with the gathered items
   as the value. Of the same length as what they replace, they are swapped
   in and the old items decrefed from the last, as list_ass_slice decrefs
   them; otherwise list_ass_slice itself does it, resizing as CPython
   does. */
static int
cimpl_gathered_ass_slice(PyObject *self, Py_ssize_t lo, Py_ssize_t hi)
{
    PyObject **items, *v;
    Py_ssize_t k;
    int r;
    PySlice_AdjustIndices(Py_SIZE(self), &lo, &hi, 1);
    if (hi < lo)
        hi = lo;
    if (hi - lo != gathered_count) {
        v = cimpl_gathered_list();
        if (v == NULL)
            return -1;
        r = PyList_SetSlice(self, lo, hi, v);
        Py_DECREF(v);
        return r;
    }
    items = ((PyListObject *)self)->ob_item + lo;
    for (k = 0; k < gathered_count; k++) {
        PyObject *old = items[k];
        items[k] = gathered[k];
        gathered[k] = old;
    }
    while (gathered_count > 0) Py_DECREF(gathered[--gathered_count]);
    return 0;
}

/* the same for one piece, a slice of src */
static int
cimpl_ass_slice_from(PyObject *self, Py_ssize_t lo, Py_ssize_t hi,
                     PyObject *src, Py_ssize_t start, Py_ssize_t stop,
                     long step)
{
    if (cimpl_gather(src, start, stop, step) < 0)
        return -1;
    return cimpl_gathered_ass_slice(self, lo, hi);
}

/* list_subscript's case for a step of -1 */
static PyObject *
cimpl_list_slice_back(PyObject *self, Py_ssize_t start, Py_ssize_t stop)
{
    if (cimpl_gather(self, start, stop, -1) < 0)
        return NULL;
    return cimpl_gathered_list();
}

static PyObject *
cimpl_list_slice(PyObject *self, Py_ssize_t start, Py_ssize_t stop);

/* make the pieces' items exist first: an item that is itself a virtual list
   gathers its own when made, so it is made before this list's gathering
   starts */
static bool
pieces_items_ready(PsycoObject *po, vinfo_t *v)
{
    int k, n = pieces_count(v);
    for (k = 0; k < n; k++)
        if (!compute_vinfo(PIECE(v, k, PIECE_SRC), po))
            return false;
    return true;
}

/* the run-time calls that gather v's items */
static bool
pieces_gather(PsycoObject *po, vinfo_t *v)
{
    int k, n = pieces_count(v);
    for (k = 0; k < n; k++) {
        vinfo_t *r;
        long step = piece_step(v, k);
        if (step == 0)
            r = psyco_generic_call(po, cimpl_gather_item,
                                   CfCommonIntZeroOk | CfNoPython | CfNoBlock,
                                   "v", PIECE(v, k, PIECE_SRC));
        else
            r = psyco_generic_call(
                po, cimpl_gather, CfCommonIntZeroOk | CfNoPython | CfNoBlock,
                "vvvl", PIECE(v, k, PIECE_SRC), PIECE(v, k, PIECE_START),
                PIECE(v, k, PIECE_STOP), step);
        if (r == NULL)
            return false;
    }
    return true;
}

/* making the items runs no Python code; the other pieces lists are left */
static bool
compute_pieces(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *newobj;
    char pieces = po->pr.pieces;
    po->pr.pieces = 0;
    if (pieces_count(v) == 1 && piece_step(v, 0) != 0) {
        /* one slice: what list_subscript does */
        newobj = psyco_generic_call(
            po,
            piece_step(v, 0) == 1 ? (void *)cimpl_list_slice
                                  : (void *)cimpl_list_slice_back,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vvv",
            PIECE(v, 0, PIECE_SRC), PIECE(v, 0, PIECE_START),
            PIECE(v, 0, PIECE_STOP));
    }
    else if (!pieces_items_ready(po, v) || !pieces_gather(po, v))
        newobj = NULL;
    else
        newobj = psyco_generic_call(
            po, cimpl_gathered_list,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "");
    po->pr.pieces = pieces;
    if (newobj == NULL)
        return false;
    vinfo_array_shrink(po, v, LIST_TOTAL);
    vinfo_move(po, v, newobj);
    Psyco_AssertType(po, v, &PyList_Type);
    return true;
}

/* a pieces list of n pieces, their words to be set */
static vinfo_t *
pieces_new(PsycoObject *po, int n)
{
    vinfo_t *r = vinfo_new(VirtualTime_New(&psyco_computed_pieces));
    r->array = array_new(PIECES_FIRST + n * PIECE_WORDS);
    r->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&PyList_Type));
    po->pr.pieces = 1;
    return r;
}

static void
piece_set(vinfo_t *r, int k, vinfo_t *src, vinfo_t *start, vinfo_t *stop,
          long step)
{
    vinfo_incref(src);
    PIECE(r, k, PIECE_SRC) = src;
    if (start == NULL) {
        start = psyco_vi_Zero();
        stop = psyco_vi_Zero();
    }
    else {
        vinfo_incref(start);
        vinfo_incref(stop);
    }
    PIECE(r, k, PIECE_START) = start;
    PIECE(r, k, PIECE_STOP) = stop;
    PIECE(r, k, PIECE_STEP) = vinfo_new(CompileTime_New(step));
}

/* whether a local variable holds 'v' */
static int
held_by_local(PsycoObject *po, vinfo_t *v)
{
    int i;
    for (i = 0; i < po->pr.stack_base; i++)
        if (LOC_LOCALS_PLUS[i] == v)
            return 1;
    return 0;
}

/* the pieces 'v' would give as part of a concatenation, or -1 if it gives
   none: a pieces list's own, a short virtual list's items, an exact list
   whole */
static int
operand_pieces(PsycoObject *po, vinfo_t *v)
{
    int n;
    if (is_pieces(v))
        return pieces_count(v);
    n = PsycoList_Load(v);
    if (n >= 0)
        return n;
    if (Psyco_KnownType(v) == &PyList_Type && !is_virtualtime(v->source) &&
        held_by_local(po, v))
        return 1;
    return -1;
}

static int
operand_copy(PsycoObject *po, vinfo_t *r, int k, vinfo_t *v)
{
    int i, n;
    if (is_pieces(v)) {
        n = pieces_count(v);
        for (i = 0; i < n; i++)
            piece_set(r, k + i, PIECE(v, i, PIECE_SRC),
                      PIECE(v, i, PIECE_START), PIECE(v, i, PIECE_STOP),
                      piece_step(v, i));
        return k + n;
    }
    n = PsycoList_Load(v);
    if (n >= 0) {
        for (i = 0; i < n; i++)
            piece_set(r, k + i, v->array->items[VLIST_ITEMS + i], NULL, NULL,
                      0);
        return k + n;
    }
    {
        vinfo_t *lo = vinfo_new(CompileTime_New(0));
        vinfo_t *hi = vinfo_new(CompileTime_New(PY_SSIZE_T_MAX));
        piece_set(r, k, v, lo, hi, 1);
        vinfo_decref(hi, po);
        vinfo_decref(lo, po);
        return k + 1;
    }
}

/* a + b as pieces, if one is pieces and together they are few enough */
static vinfo_t *
pieces_concat(PsycoObject *po, vinfo_t *a, vinfo_t *b)
{
    int na, nb;
    vinfo_t *r;
    if (!is_pieces(a) && !is_pieces(b))
        return NULL;
    na = operand_pieces(po, a);
    nb = operand_pieces(po, b);
    if (na < 0 || nb < 0 || na + nb > PIECES_MAX)
        return NULL;
    r = pieces_new(po, na + nb);
    operand_copy(po, r, operand_copy(po, r, 0, a), b);
    return r;
}

static bool
force_walk(PsycoObject *po, vinfo_array_t *a, vinfo_t *keep, int depth)
{
    int i;
    for (i = 0; i < a->count; i++) {
        vinfo_t *v = a->items[i];
        if (v == NULL || v == keep)
            continue;
        if (is_pieces(v)) {
            if (!compute_vinfo(v, po))
                return false;
        }
        else if (v->array != NullArray && depth < NESTED_WEIGHT_END) {
            if (!force_walk(po, v->array, keep, depth + 1))
                return false;
        }
    }
    return true;
}

/* make every pieces list but 'keep' (NULL: all of them) */
DEFINEFN
bool
PsycoList_ForcePieces(PsycoObject *po, vinfo_t *keep)
{
    if (!po->pr.pieces)
        return true;
    po->pr.pieces = 0;
    if (!force_walk(po, &po->vlocals, keep, 0))
        return false;
    if (keep != NULL)
        po->pr.pieces = 1;
    return true;
}

/* whether a slice of o can be a piece: o is held by a local variable, so
   the piece keeps it no longer than CPython would */
static int
slice_makes_piece(PsycoObject *po, vinfo_t *o)
{
    return !is_virtualtime(o->source) && held_by_local(po, o);
}

/* o[i:j] = value, a pieces list: its items gathered, then stored */
static vinfo_t *
pieces_ass_slice(PsycoObject *po, vinfo_t *o, vinfo_t *i, vinfo_t *j,
                 vinfo_t *value)
{
    vinfo_t *r;
    if (!PsycoList_ForcePieces(po, value))
        return NULL;
    /* making the items runs no Python code */
    po->pr.pieces = 0;
    if (pieces_count(value) == 1 && piece_step(value, 0) != 0)
        r = psyco_generic_call(
            po, cimpl_ass_slice_from,
            CfCommonIntZeroOk | CfNoPython | CfNoBlock, "vvvvvvl", o, i, j,
            PIECE(value, 0, PIECE_SRC), PIECE(value, 0, PIECE_START),
            PIECE(value, 0, PIECE_STOP), piece_step(value, 0));
    else if (!pieces_items_ready(po, value) || !pieces_gather(po, value))
        r = NULL;
    else
        r = psyco_generic_call(po, cimpl_gathered_ass_slice,
                               CfCommonIntZeroOk | CfNoPython | CfNoBlock,
                               "vvv", o, i, j);
    po->pr.pieces = 1;
    return r;
}

/***************************************************************/
/*** list objects meta-implementation                        ***/

DEFINEFN
vinfo_t *
plist_item(PsycoObject *po, vinfo_t *a, vinfo_t *i)
{
    condition_code_t cc;
    vinfo_t *vlen;
    vinfo_t *ob_item;
    vinfo_t *result;

    vlen = psyco_get_field(po, a, VAR_size);
    if (vlen == NULL)
        return NULL;

    cc = integer_cmp(po, i, vlen, Py_GE | COMPARE_UNSIGNED);
    vinfo_decref(vlen, po);
    if (cc == CC_ERROR)
        return NULL;

    if (runtime_condition_f(po, cc)) {
        PycException_SetString(po, PyExc_IndexError, LE_MSG_LIST_INDEX_RANGE);
        return NULL;
    }
    assert_nonneg(i);

    if (a->source == VirtualTime_New(&psyco_computed_vlist) &&
        is_compiletime(i->source)) {
        /* optimize virtual lists */
        vlist_length(a); /* for the assert()s */
        result =
            vinfo_getitem(a, VLIST_ITEMS + CompileTime_Get(i->source)->value);
        extra_assert(result != NULL);
        vinfo_incref(result);
        need_reference(po, result);
        return result;
    }

    ob_item = psyco_get_field(po, a, LIST_ob_item);
    if (ob_item == NULL)
        return NULL;

    result = psyco_get_field_array(po, ob_item, FPYREF(LIST_itemsarray), i);
    vinfo_decref(ob_item, po);
    return result;
}

static vinfo_t *
plist_ass_item(PsycoObject *po, vinfo_t *a, vinfo_t *i, vinfo_t *v)
{
    condition_code_t cc;
    vinfo_t *vlen;
    vinfo_t *ob_item;
    vinfo_t *old_value;
    bool ok;

    if (v == NULL) {
        /* XXX implement item deletion */
        return psyco_generic_call(po, PyList_Type.tp_as_sequence->sq_ass_item,
                                  CfCommonIntZeroOk, "vvl", a, i, (long)NULL);
    }

    /* the list may be a piece's source */
    if (!PsycoList_ForcePieces(po, NULL))
        return NULL;
    vlen = psyco_get_field(po, a, VAR_size);
    if (vlen == NULL)
        return NULL;

    cc = integer_cmp(po, i, vlen, Py_GE | COMPARE_UNSIGNED);
    vinfo_decref(vlen, po);
    if (cc == CC_ERROR)
        return NULL;

    if (runtime_condition_f(po, cc)) {
        PycException_SetString(po, PyExc_IndexError,
                               LE_MSG_LIST_ASSIGN_INDEX_RANGE);
        return NULL;
    }
    assert_nonneg(i);

    if (a->source == VirtualTime_New(&psyco_computed_vlist) &&
        is_compiletime(i->source) && !vinfo_would_be_recursive(a, v)) {
        /* optimize virtual lists */
        vlist_length(a); /* for the assert()s */
        vinfo_incref(v);
        vinfo_setitem(po, a, VLIST_ITEMS + CompileTime_Get(i->source)->value,
                      v);
        return VINFO_OK;
    }

    ob_item = psyco_get_field(po, a, LIST_ob_item);
    if (ob_item == NULL)
        return NULL;

    old_value = psyco_get_field_array(po, ob_item, LIST_itemsarray, i);
    ok = (old_value != NULL) &&
         psyco_put_field_array(po, ob_item, FPYREF(LIST_itemsarray), i, v);

    vinfo_decref(ob_item, po);
    if (ok)
        psyco_decref_v(po, old_value);
    vinfo_xdecref(old_value, po);

    return ok ? VINFO_OK : NULL;
}

DEFINEFN
vinfo_t *
psyco_plist_concat(PsycoObject *po, vinfo_t *a, vinfo_t *b)
{
    PyTypeObject *btp = Psyco_NeedType(po, b);
    if (btp == NULL)
        return NULL;

    if (btp == &PyList_Type && Psyco_KnownType(a) == &PyList_Type) {
        vinfo_t *r = pieces_concat(po, a, b);
        if (r != NULL)
            return r;
    }
    if (PyType_TypeCheck(btp, &PyList_Type)) {
        vinfo_t *r;
        int alen, blen;

        blen = PsycoList_Load(b);
        if (blen >= 0) {
            alen = PsycoList_Load(a);
            if (alen >= 0) {
                /* known source list lengths:
                   build a virtual list */
                vinfo_t *buffer[VLIST_LENGTH_MAX * 2];
                extra_assert(alen <= VLIST_LENGTH_MAX);
                extra_assert(blen <= VLIST_LENGTH_MAX);
                memcpy(buffer, a->array->items + VLIST_ITEMS,
                       alen * sizeof(vinfo_t *));
                memcpy(buffer + alen, b->array->items + VLIST_ITEMS,
                       blen * sizeof(vinfo_t *));
                return PsycoList_New(po, alen + blen, buffer);
            }
            else if (blen == 1) {
                /* XXX - fallback to an append ? */
            }
        }
        /* fallback - but we still know that the result is a list */
        r = psyco_generic_call(po, PyList_Type.tp_as_sequence->sq_concat,
                               CfCommonNewRefPyObject, "vv", a, b);
        if (r != NULL)
            Psyco_AssertType(po, r, &PyList_Type);
        return r;
    }

    /* fallback */
    return psyco_generic_call(po, PyList_Type.tp_as_sequence->sq_concat,
                              CfCommonNewRefPyObject, "vv", a, b);
}

DEFINEFN
bool
PsycoList_Append(PsycoObject *po, vinfo_t *v, vinfo_t *vitem)
{
    /* use the real PyList_Append */
    return psyco_generic_call(po, PyList_Append, CfCommonIntZeroOk, "vv", v,
                              vitem) != NULL;
}

/* list_subscript's slice case with step 1, after PySlice_Unpack */
static PyObject *
cimpl_list_slice(PyObject *self, Py_ssize_t start, Py_ssize_t stop)
{
    if (PySlice_AdjustIndices(Py_SIZE(self), &start, &stop, 1) <= 0)
        return PyList_New(0);
    return PyList_GetSlice(self, start, stop);
}

/* list_ass_subscript's slice case with step 1: list_ass_slice, which
   PyList_SetSlice is (value NULL deletes) */
static int
cimpl_list_ass_slice(PyObject *self, Py_ssize_t start, Py_ssize_t stop,
                     PyObject *value)
{
    PySlice_AdjustIndices(Py_SIZE(self), &start, &stop, 1);
    return PyList_SetSlice(self, start, stop, value);
}

/* list_subscript is also list.__getitem__ (METH_COEXIST), so
   super().__getitem__(i) brings it a subclass instance whose own slots run
   Python code; the generic meta would dispatch on those */
static vinfo_t *
plist_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
    /* a subclass's slots behind guards, as the generic meta
       reads them */
    void *slots[3];
    void *list_slots[3];
    int i;
    PyTypeObject *tp = Psyco_NeedType(po, o);
    if (tp == NULL)
        return NULL;
    slots[0] = &tp->tp_as_mapping->mp_subscript;
    slots[1] = &tp->tp_as_sequence->sq_item;
    slots[2] = &tp->tp_as_sequence->sq_length;
    list_slots[0] = PyList_Type.tp_as_mapping->mp_subscript;
    list_slots[1] = PyList_Type.tp_as_sequence->sq_item;
    list_slots[2] = PyList_Type.tp_as_sequence->sq_length;
    for (i = 0; i < 3; i++) {
        void *f;
        switch (Psyco_TypeSlot(po, tp, slots[i], &f)) {
            case 1:
                if (f == list_slots[i])
                    break;
                /* fall through */
            case 0:
                return psyco_generic_call(
                    po, PyList_Type.tp_as_mapping->mp_subscript,
                    CfCommonNewRefPyObject, "vv", o, key);
            default:
                return NULL;
        }
    }
    if (tp == &PyList_Type && slice_makes_piece(po, o)) {
        vinfo_t *i, *j, *r;
        int step;
        switch (PsycoSlice_UnpackStep(po, key, &i, &j, &step)) {
            case -1:
                return NULL;
            case 1:
                r = pieces_new(po, 1);
                piece_set(r, 0, o, i, j, step);
                vinfo_decref(j, po);
                vinfo_decref(i, po);
                return r;
        }
    }
    if (Psyco_KnownType(key) == &PySlice_Type) {
        vinfo_t *i, *j, *r;
        switch (PsycoSlice_Unpack(po, key, &i, &j)) {
            case -1:
                return NULL;
            case 1:
                r = psyco_generic_call(po, cimpl_list_slice,
                                       CfCommonNewRefPyObject, "vvv", o, i, j);
                vinfo_decref(j, po);
                vinfo_decref(i, po);
                if (r != NULL)
                    Psyco_AssertType(po, r, &PyList_Type);
                return r;
        }
    }
    return psyco_generic_subscript(po, o, key);
}

/* list_ass_subscript: a virtual slice is cimpl_list_ass_slice */
static vinfo_t *
plist_ass_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key, vinfo_t *value)
{
    PyTypeObject *ktp = Psyco_NeedType(po, key);
    if (ktp == NULL)
        return NULL;
    if (ktp == &PySlice_Type) {
        vinfo_t *i, *j, *r;
        switch (PsycoSlice_Unpack(po, key, &i, &j)) {
            case -1:
                return NULL;
            case 1:
                if (value != NULL && is_pieces(value) &&
                    Psyco_KnownType(o) == &PyList_Type &&
                    !is_virtualtime(o->source))
                    r = pieces_ass_slice(po, o, i, j, value);
                else if (value != NULL)
                    r = psyco_generic_call(po, cimpl_list_ass_slice,
                                           CfCommonIntZeroOk, "vvvv", o, i, j,
                                           value);
                else
                    r = psyco_generic_call(po, cimpl_list_ass_slice,
                                           CfCommonIntZeroOk, "vvvl", o, i, j,
                                           0L);
                vinfo_decref(j, po);
                vinfo_decref(i, po);
                return r;
        }
    }
    return psyco_generic_ass_subscript(po, o, key, value) ? VINFO_OK : NULL;
}

/* list_iter; a str.split() list held only by the caller is iterated
   without being made */
static vinfo_t *
plist_iter(PsycoObject *po, vinfo_t *v)
{
    if (PsycoUnicode_IsSplitList(v))
        return PsycoUnicode_SplitIter(po, v);
    return PsycoListIter_New(po, v);
}

/* listobject.c's sq_length */
static vinfo_t *
list_length(PsycoObject *po, vinfo_t *a)
{
    return psyco_get_field(po, a, VAR_size);
}

static void
gen_listobject_init(void)
{
    Psyco_DefineMeta(PyList_Type.tp_as_sequence->sq_length, list_length);
}

INITIALIZATIONFN
void
psy_listobject_init(void)
{
    PyMappingMethods *mm;
    PySequenceMethods *m = PyList_Type.tp_as_sequence;
    gen_listobject_init();
    Psyco_DefineMeta(m->sq_item, plist_item);
    Psyco_DefineMeta(m->sq_ass_item, plist_ass_item);
    Psyco_DefineMeta(m->sq_concat, psyco_plist_concat);

    mm = PyList_Type.tp_as_mapping;
    if (mm) {
        Psyco_DefineMeta(mm->mp_subscript, plist_subscript);
        Psyco_DefineMeta(mm->mp_ass_subscript, plist_ass_subscript);
    }

    /* iter(l) is a virtual list_iterator (piterobject.c) */
    if (PyList_Type.tp_iter != NULL)
        Psyco_DefineMeta(PyList_Type.tp_iter, &plist_iter);

    /* list object are mutable;
       they must be forced out of virtual-time across function calls */
    INIT_SVIRTUAL_NOCALL(psyco_computed_vlist, compute_vlist, NW_VLISTS);
    INIT_SVIRTUAL_NOCALL(psyco_computed_pieces, compute_pieces, NW_VLISTS);

    /*psyco_empty_list = PsycoList_NEW(0);*/
}
