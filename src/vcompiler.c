/*
 * psyco_compile() makes the merge-point decision. It compares the current
 * state with the snapshots stored at this merge point
 * (dispatcher.c:psyco_compatible):
 *   - exact match:   emit unification moves and jump to the existing code;
 *   - partial match: un-promote the differing constants to run-time and retry;
 *   - no match:      start new code (or a coding pause if the buffer is full),
 *                    unless the merge point already has MP_BLOCK_CAP blocks:
 *                    then forget the ints' known values and the constants
 *                    that differ between its blocks, and retry.
 * A coding pause is a stub that compiles the continuation only when execution
 * first reaches it.
 *
 * Field access (psyco_internal_getfld/putfld) folds loads from compile-time
 * objects in C and caches results in vinfo->array, except the type of a
 * constant whose class can be assigned, which is checked.
 *
 * Code buffers, machine code, stub records and the references constants
 * held belong to the owner whose code is compiled (codemanager.h), and are
 * released when its code object dies; a buffer is returned borrowed.
 */
#include "vcompiler.h"
#include "Python/pycompiler.h"
#include "Objects/pobject.h"
#include "Objects/pboolobject.h"
#include "Objects/pfloatobject.h"
#include "Objects/plongobject.h"
#include "codemanager.h"
#include "dispatcher.h"
#include "mergepoints.h"
#include "pycodegen.h"
#include "v_mem_mov.h"
#include <idispatcher.h>

DEFINEVAR const long psyco_zero = 0;
DEFINEVAR source_virtual_t psyco_vsource_not_important;

/*****************************************************************/

BLOCKALLOC_IMPLEMENTATION(vinfo, vinfo_t, 8192)
BLOCKALLOC_IMPLEMENTATION(sk, source_known_t, 4096)

/*****************************************************************/

DEFINEFN
vinfo_array_t *
array_grow1(vinfo_array_t *array, int ncount)
{
    int i = array->count;
    extra_assert(ncount > i);
    if (i == 0)
        array = PyMem_MALLOC(offsetof(vinfo_array_t, items) +
                             ncount * sizeof(vinfo_t *));
    else
        array = PyMem_REALLOC(array, offsetof(vinfo_array_t, items) +
                                         ncount * sizeof(vinfo_t *));
    if (array == NULL || LE_FAIL(LE_FAIL_ARRAY))
        OUT_OF_MEMORY();
    array->count = ncount;
    while (i < ncount) array->items[i++] = NULL;
    return array;
}

DEFINEFN
void
vinfo_array_shrink(PsycoObject *po, vinfo_t *vi, int ncount)
{
    vinfo_array_t *array = vi->array;
    int i = array->count;
    if (i <= ncount)
        return;

    while (i > ncount) {
        vinfo_t *v1 = array->items[--i];
        if (v1 != NULL) {
            array->items[i] = NULL;
            vinfo_decref(v1, po);
        }
    }
    if (ncount == 0) {
        PyMem_FREE(array);
        array = NullArray;
    }
    else {
        array = PyMem_REALLOC(array, offsetof(vinfo_array_t, items) +
                                         ncount * sizeof(vinfo_t *));
        if (array == NULL)
            OUT_OF_MEMORY();
        array->count = ncount;
    }
    vi->array = array;
}

/*****************************************************************/

DEFINEFN
void
sk_release(source_known_t *sk)
{
    /* Code may hold the object's address, or that of anything reached from
       it, so the owner the constant was made under holds it until its code
       dies (sk_new). Another owner's code can have used it only if the
       constant came to it as a callee's argument, and a callee holds every
       constant it was given (psyfunc.c:compile_callee). The owner, not the
       source_known_t, holds it so that every reference an owner has is in
       its set, where the cycle scan sees it (codemanager.c). */
    sk_delete(sk);
}

DEFINEFN
vinfo_t *
vinfo_copy(vinfo_t *vi)
{
    vinfo_t *result = vinfo_new_skref(vi->source);
    if ((result->type_hint = vi->type_hint) != NULL)
        sk_incref(result->type_hint);
    result->array = vi->array;
    if (result->array->count > 0) {
        result->array = array_new(result->array->count);
        duplicate_array(result->array, vi->array);
    }
    return result;
}

DEFINEFN
void
vinfo_release(vinfo_t *vi, PsycoObject *po)
{
    switch (gettime(vi->source)) {
        case RunTime:
            if (po != NULL) {
                if (has_rtref(vi->source)) {
                    /* write Py_DECREF() when releasing the last reference to
                       a run-time vinfo_t holding a reference to a Python
                       object */
                    if (is_maybe_null(vi->source))
                        psyco_xdecref_rt(po, vi);
                    else
                        psyco_decref_rt(po, vi);
                }
                RTVINFO_RELEASE(vi->source);
            }
            break;

        case CompileTime:
            sk_decref(CompileTime_Get(vi->source));
            break;

#if HAVE_CCREG
        case VirtualTime:
            if (po != NULL) {
                int i;
                for (i = 0; i < HAVE_CCREG; i++)
                    if (vi == po->ccregs[i])
                        po->ccregs[i] = NULL;
            }
            break;
#endif
    }

    /* must be after the switch because psyco_decref_rt() did use the
       array to extract any available type information to speed up Py_DECREF().
     */
    if (vi->array != NullArray)
        array_delete(vi->array, po);
    if (vi->type_hint != NULL)
        sk_decref(vi->type_hint);

#if HAVE_CCREG && ALL_CHECKS
    /* only virtual-time vinfos are allowed in po->ccreg */
    if (po != NULL) {
        int i;
        for (i = 0; i < HAVE_CCREG; i++) extra_assert(vi != po->ccregs[i]);
    }
#endif
    psyco_llfree_vinfo(vi);
}

DEFINEFN
void
vinfo_move(PsycoObject *po, vinfo_t *vtarget, vinfo_t *vsource)
{
    Source src = vsource->source;

#if ALL_CHECKS
    extra_assert(!is_virtualtime(src));
    if (is_compiletime(src)) {
        /* a compile-time vinfo may only hold compile-time subitems */
        int j;
        for (j = 0; j < vtarget->array->count; j++)
            extra_assert(vtarget->array->items[j] == NULL ||
                         is_compiletime(vtarget->array->items[j]->source));
    }
#endif

    extra_assert(vsource->array == NullArray);
    vtarget->source = src;
    if (is_runtime(src))
        RTVINFO_MOVE(src, vtarget);
    extra_assert(vsource->refcount == 1);
    if (vsource->type_hint != NULL)
        sk_decref(vsource->type_hint);
    psyco_llfree_vinfo(vsource);
    clear_tmp_marks(vtarget->array);
    psyco_simplify_array(vtarget->array, po);
}

DEFINEFN
bool
vinfo_would_be_recursive(vinfo_t *vi, vinfo_t *newitem)
{
    vinfo_array_t *array;
    int i;
    if (newitem == vi)
        return true;
    array = newitem->array;
    i = array->count;
    while (i--)
        if (array->items[i] != NULL)
            if (vinfo_would_be_recursive(vi, array->items[i]))
                return true;
    return false;
}

DEFINEFN
void
clear_tmp_marks(vinfo_array_t *array)
{
    /* clear all 'tmp' fields in the array, recursively */
    int i = array->count;
    while (i--)
        if (array->items[i] != NULL) {
            array->items[i]->tmp = NULL;
            if (array->items[i]->array != NullArray)
                clear_tmp_marks(array->items[i]->array);
        }
}

#if ALL_CHECKS
DEFINEFN
void
assert_cleared_tmp_marks(vinfo_array_t *array)
{
    /* assert that all 'tmp' fields are NULL */
    int i = array->count;
    while (i--)
        if (array->items[i] != NULL) {
            extra_assert(array->items[i]->tmp == NULL);
            if (array->items[i]->array != NullArray)
                assert_cleared_tmp_marks(array->items[i]->array);
        }
}

static bool
array_contains(vinfo_array_t *array, vinfo_t *vi)
{
    bool result = false;
    int i = array->count;
    while (i--)
        if (array->items[i] != NULL) {
            if (array->items[i] == vi)
                result = true;
            if (array->items[i]->array != NullArray) {
                if (is_compiletime(array->items[i]->source))
                    extra_assert(!array_contains(array->items[i]->array, vi));
                else if (array_contains(array->items[i]->array, vi))
                    result = true;
            }
        }
    return result;
}

DEFINEFN
void
assert_array_contains_nonct(vinfo_array_t *array, vinfo_t *vi)
{
    /* check that 'vi' appears at least once in 'array', and
       never appears as subitem of a compile-time vinfo_t */
    extra_assert(array_contains(array, vi));
}

static void
coherent_array(vinfo_array_t *source, PsycoObject *po, int found[],
               bool allow_any)
{
    int i = source->count;
    while (i--)
        if (source->items[i] != NULL) {
            Source src = source->items[i]->source;
            extra_assert(allow_any || is_compiletime(src));
            switch (gettime(src)) {
                case RunTime:
                    /* test that we don't have an unreasonably high value
                       although it might still be correct in limit cases */
                    extra_assert(getstack(src) < RunTime_StackMax / 2);
                    RTVINFO_CHECK(po, source->items[i], found);
                    break;
                case CompileTime:
                case VirtualTime:
                    break;
                default:
                    psyco_fatal_msg("gettime() corrupted");
            }
#if HAVE_CCREG
            if (psyco_vsource_cc(src) != CC_ALWAYS_FALSE) {
                int index = INDEX_CC(psyco_vsource_cc(src));
                extra_assert(po->ccregs[index] == source->items[i]);
                found[REG_TOTAL + index] = 1;
            }
#endif
            if (source->items[i]->array != NullArray)
                coherent_array(source->items[i]->array, po, found,
                               !is_compiletime(src));
        }
}

static void
hack_refcounts(vinfo_array_t *source, int delta, int mvalue)
{
    int i = source->count;
    while (i--)
        if (source->items[i] != NULL) {
            long rc = source->items[i]->refcount;
            rc = ((rc + delta) & 0xFFFF) + (rc & 0x10000);
            source->items[i]->refcount = rc;
            if ((rc & 0x10000) == mvalue) {
                source->items[i]->refcount ^= 0x10000;
                if (source->items[i]->array != NullArray)
                    hack_refcounts(source->items[i]->array, delta, mvalue);
            }
        }
}

static vinfo_t *
nonnull_refcount(vinfo_array_t *source)
{
    int i = source->count;
    while (i--)
        if (source->items[i] != NULL) {
            if (source->items[i]->refcount != 0x10000) {
                fprintf(stderr, "nonnull_refcount: item %d\n", i);
                return source->items[i];
            }
            if (source->items[i]->array != NullArray) {
                vinfo_t *result = nonnull_refcount(source->items[i]->array);
                if (result != NULL) {
                    fprintf(stderr, "nonnull_refcount: in array item %d\n", i);
                    return result;
                }
            }
        }
    return NULL;
}

DEFINEFN
void
psyco_assert_coherent1(PsycoObject *po, bool full)
{
    union {
        vinfo_array_t a;
        char room[offsetof(vinfo_array_t, items) + 3 * sizeof(vinfo_t *)];
    } extra_refs;
    vinfo_array_t *debug_extra_refs = &extra_refs.a;
    int found[REG_TOTAL + HAVE_CCREG];
    int i;
    vinfo_t *err;
    for (i = 0; i < REG_TOTAL + HAVE_CCREG; i++) found[i] = 0;
    debug_extra_refs->count = 3;
    debug_extra_refs->items[0] =
        po->pr.exc; /* normally private to pycompiler.c,*/
    debug_extra_refs->items[1] =
        po->pr.val; /* but this is for debugging only */
    debug_extra_refs->items[2] = po->pr.tb; /* run-time after END_FINALLY */
    coherent_array(&po->vlocals, po, found, true);
    coherent_array(debug_extra_refs, po, found, true);
    if (full) {
        RTVINFO_CHECKED(po, found);
#if HAVE_CCREG
        for (i = 0; i < HAVE_CCREG; i++)
            if (!found[REG_TOTAL + i])
                extra_assert(po->ccregs[i] == NULL);
#endif
        hack_refcounts(&po->vlocals, -1, 0);
        hack_refcounts(debug_extra_refs, -1, 0);
        err = nonnull_refcount(&po->vlocals);
        hack_refcounts(debug_extra_refs, +1, 0x10000);
        hack_refcounts(&po->vlocals, +1, 0x10000);
        extra_assert(!err); /* see nonnull_refcounts() */
    }
}
#endif /* ALL_CHECKS */

DEFINEFN
void
duplicate_array(vinfo_array_t *target, vinfo_array_t *source)
{
    /* make a depth copy of an array.
       Same requirements as psyco_duplicate().
       Do not use for arrays of length 0. */
    int i;
    for (i = 0; i < source->count; i++) {
        vinfo_t *sourcevi = source->items[i];
        if (sourcevi == NULL)
            target->items[i] = NULL;
        else if (sourcevi->tmp != NULL) {
            target->items[i] = sourcevi->tmp;
            target->items[i]->refcount++;
        }
        else {
            vinfo_t *targetvi = vinfo_copy(sourcevi);
            targetvi->tmp = NULL;
            target->items[i] = sourcevi->tmp = targetvi;
        }
    }
    target->count = source->count;

    /*return true;

   fail:
    while (i--)
      if (items[i] != NULL)
        {
          items[i]->tmp = NULL;
          target->items[i]->decref(NULL);
        }
        return false;*/
}

DEFINEFN
PsycoObject *
psyco_duplicate(PsycoObject *po)
{
    /* Requires that all 'tmp' marks in 'po' are cleared.
       In the new copy all 'tmp' marks will be cleared. */

    PsycoObject *result = PsycoObject_New(po->vlocals.count);
    psyco_assert_coherent(po);
    assert_cleared_tmp_marks(&po->vlocals);
    duplicate_array(&result->vlocals, &po->vlocals);

    /* set the register pointers of 'result' to the new vinfo_t's */
    DUPLICATE_PROCESSOR(result, po);

    /* the rest of the data is copied with no change */
    result->respawn_cnt = po->respawn_cnt;
    result->respawn_proxy = po->respawn_proxy;
    result->mp_serial = po->mp_serial;
    result->code = po->code;
    result->codelimit = po->codelimit;
    pyc_data_duplicate(&result->pr, &po->pr);

    assert_cleared_tmp_marks(&result->vlocals);
    psyco_assert_coherent(result);
    return result;
}

DEFINEFN void
PsycoObject_Delete(PsycoObject *po)
{
#if LE_CODESTATS
    psyco_codestats_delete(po);
#endif
    pyc_data_release(&po->pr);
    deallocate_array(&po->vlocals, NULL);
    PyMem_FREE(po);
}

DEFINEFN
bool
psyco_limit_nested_weight(PsycoObject *po, vinfo_array_t *array, int nw_index,
                          signed char nw_end)
{
    signed char nw;
    int i;
    for (i = array->count; i--;) {
        vinfo_t *vi = array->items[i];
        if (vi != NULL) {
            nw = nw_end;
            if (is_virtualtime(vi->source)) {
                source_virtual_t *sv = VirtualTime_Get(vi->source);
                nw -= sv->nested_weight[nw_index];
                if (nw <= 0) {
                    /* maximum reached, force out of virtual-time */
                    if (!sv->compute_fn(po, vi))
                        return false;
                    /* vi->array may be modified by compute_fn() */
                    continue;
                }
            }
            if (vi->array != NullArray)
                if (!psyco_limit_nested_weight(po, vi->array, nw_index, nw))
                    return false;
        }
    }
    return true;
}

DEFINEFN
long
direct_read_vinfo(vinfo_t *vi, char *data)
{
    int sindex;
    if (vi == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "undefined value");
        return -1;
    }
    switch (gettime(vi->source)) {
        case RunTime:
            sindex = getstack(vi->source);
            return *(long *)(data + sindex);

        case CompileTime:
            return CompileTime_Get(vi->source)->value;

        default:
            Py_FatalError("Psyco: virtual-time direct_read_vinfo");
            return 0;
    }
}

DEFINEFN
PyObject *
direct_xobj_vinfo(vinfo_t *vi, char *data)
{
    int sindex;
    PyObject *o = NULL;
    if (vi != NULL) {
        switch (gettime(vi->source)) {
            case RunTime:
                sindex = getstack(vi->source);
                o = *(PyObject **)(data + sindex);
                break;

            case CompileTime:
                o = (PyObject *)CompileTime_Get(vi->source)->value;
                break;

            case VirtualTime:
                if (VirtualTime_Get(vi->source)->direct_compute_fn == NULL)
                    Py_FatalError("Psyco: value not directly computable");
                return VirtualTime_Get(vi->source)
                    ->direct_compute_fn(vi, data);
        }
        Py_XINCREF(o);
    }
    return o;
}

/*****************************************************************/

PSY_INLINE vinfo_t *
field_read(PsycoObject *po, vinfo_t *vi, long offset, vinfo_t *vindex,
           defield_t df, bool newref)
{
    vinfo_t *result = v_mem_mov(po, NULL, vi, vindex, offset, FIELD_SIZE2(df),
                                ((long)df & FIELD_UNSIGNED) == 0);
    if ((long)df & FIELD_NONNEG) {
        assert_nonneg(result);
    }
    if (newref && FIELD_HAS_REF(df)) {
        /* the container 'vi' could be freed while the
           field 'result' is still in use */
        need_reference(po, result);
    }
    return result;
}

DEFINEFN
vinfo_t *
psyco_internal_getfld(PsycoObject *po, int findex, defield_t df, vinfo_t *vi,
                      long offset)
{
    bool newref = !((long)df & FIELD_INTL_NOREF);
    vinfo_t *vf;
    if (is_virtualtime(vi->source)) {
        vf = vinfo_getitem(vi, findex);
        if (vf != NULL)
            goto done;
        if (!compute_vinfo(vi, po))
            return NULL;
    }
    if ((long)df & FIELD_MUTABLE) {
        extra_assert(newref);
        return field_read(po, vi, offset, NULL, df, newref);
    }
    vf = vinfo_getitem(vi, findex);
    if (vf != NULL)
        goto done;

    if (is_runtime(vi->source)) {
        vf = field_read(po, vi, offset, NULL, df, newref);
    }
    else if (findex == iOB_TYPE && offset == FIELD_C_OFFSET(OB_type) &&
             !((long)df & FIELD_ARRAY) && Psyco_ConstantClassMutable(vi)) {
        /* the type of a constant whose class can be assigned: checked at run
           time, not read */
        PyTypeObject *tp;
        if (Psyco_ConstantType(po, vi, &tp) < 0)
            return NULL;
        Py_INCREF(tp);
        vf = vinfo_new(CompileTime_NewSk(sk_new((long)tp, SkFlagPyObj)));
    }
    else {
        long result;
        long sk_flag = 0;
        char *ptr = (char *)(CompileTime_Get(vi->source)->value);
        ptr += offset;
        switch (FIELD_SIZE2(df)) {
            case 0:
                if ((long)df & FIELD_UNSIGNED)
                    result = *((unsigned char *)ptr);
                else
                    result = *((signed char *)ptr);
                break;
            case 1:
                if ((long)df & FIELD_UNSIGNED)
                    result = *((unsigned short *)ptr);
                else
                    result = *((signed short *)ptr);
                break;
            case 2:
                if ((long)df & FIELD_UNSIGNED)
                    result = *((unsigned int *)ptr);
                else
                    result = *((int *)ptr);
                break;
            default:
                result = *((long *)ptr);
                if ((long)df & FIELD_PYOBJ_REF) {
                    extra_assert(result);
                    extra_assert(newref);
                    Py_INCREF((PyObject *)result);
                    sk_flag = SkFlagPyObj;
                }
                break;
        }
        vf = vinfo_new(CompileTime_NewSk(sk_new(result, sk_flag)));
    }

    if (((long)df & FIELD_ARRAY) && newref)
        return vf;
    CHECK_FIELD_INDEX(findex);
    vinfo_setitem(po, vi, findex, vf);

done:
    if (newref)
        vinfo_incref(vf);
    return vf;
}

/* a constant index small enough to be a field of its own: an int whose
   item's offset is a 32-bit displacement (psyco_get_nth_field). A larger
   one, only in bounds for an array of over 128 MB, goes to v_mem_mov as an
   index, which puts it in a register. */
#define NTH_FIELD_INDEX(i) ((i) >= -(1L << 24) && (i) < (1L << 24))

DEFINEFN
vinfo_t *
psyco_get_field_array(PsycoObject *po, vinfo_t *vi, defield_t df,
                      vinfo_t *vindex)
{
    long offset = FIELD_C_OFFSET(df);
    if (!compute_vinfo(vindex, po))
        return NULL;

    extra_assert((long)df & FIELD_ARRAY);
    if (is_compiletime(vindex->source) &&
        NTH_FIELD_INDEX(CompileTime_Get(vindex->source)->value)) {
        return psyco_get_nth_field(po, vi, df,
                                   CompileTime_Get(vindex->source)->value);
    }
    else {
        if (!compute_vinfo(vi, po))
            return NULL;
        return field_read(po, vi, offset, vindex, df, true);
    }
}

DEFINEFN
bool
psyco_internal_putfld(PsycoObject *po, int findex, defield_t df, vinfo_t *vi,
                      long offset, vinfo_t *value)
{
    if (is_virtualtime(vi->source)) {
        vinfo_t *vf = vinfo_getitem(vi, findex);
        if (vf != NULL) {
            /* can only set field virtually if a value was
               previously found */
            vinfo_incref(value);
            vinfo_setitem(po, vi, findex, value);
            return true;
        }
        if (!compute_vinfo(vi, po))
            return false;
    }
    extra_assert((long)df & FIELD_MUTABLE);
    if (v_mem_mov(po, value, vi, NULL, offset, FIELD_SIZE2(df),
                  ((long)df & FIELD_UNSIGNED) == 0) == NULL) {
        return false;
    }
    if (FIELD_HAS_REF(df)) {
        /* 'value' is a PyObject* that wants to hold a reference */
        if (vinfo_getitem(vi, findex) == value) {
            /* special case: writing a value that is already
               virtually there.  This is common when promoting
               objects out of virtual-time.  In this case, we try
               to transfer the reference to the new memory
               location.  If this succeeds, the original 'value'
               must be removed from 'vi', because it no longer
               holds the reference and might become invalid if
               its new no-longer-virtual container object is
               deleted too early.

               This causes the 'value' to be reloaded from the
               memory location the next time it is used, but in a
               lot of cases it avoids a Py_INCREF()/Py_DECREF()
               pair, which costs more. */
            if (decref_create_new_lastref(po, value)) {
                vinfo_setitem(po, vi, findex, NULL);
            }
        }
        else {
            /* common case */
            decref_create_new_ref(po, value);
        }
    }
    return true;
}

DEFINEFN
bool
psyco_put_field_array(PsycoObject *po, vinfo_t *vi, defield_t df,
                      vinfo_t *vindex, vinfo_t *value)
{
    long offset = FIELD_C_OFFSET(df);
    if (!compute_vinfo(vindex, po))
        return false;

    extra_assert((long)df & FIELD_ARRAY);
    if (is_compiletime(vindex->source) &&
        NTH_FIELD_INDEX(CompileTime_Get(vindex->source)->value)) {
        return psyco_put_nth_field(
            po, vi, df, CompileTime_Get(vindex->source)->value, value);
    }
    else {
        if (!compute_vinfo(vi, po))
            return false;
        if (v_mem_mov(po, value, vi, vindex, offset, FIELD_SIZE2(df),
                      ((long)df & FIELD_UNSIGNED) == 0) == NULL) {
            return false;
        }
        if (FIELD_HAS_REF(df)) {
            /* 'value' is a PyObject* that wants to
               hold a reference */
            decref_create_new_ref(po, value);
        }
        return true;
    }
}

DEFINEFN
void
psyco_assert_field(PsycoObject *po, vinfo_t *vi, defield_t df, long value)
{
    long sk_flag = 0;
    extra_assert(!((long)df & FIELD_MUTABLE));

    if (is_compiletime(vi->source)) {
#if ALL_CHECKS
        /* check assertion at compile-time */
        vinfo_t *vf = psyco_get_field(po, vi, df);
        extra_assert(CompileTime_Get(vf->source)->value == value);
        vinfo_decref(vf, po);
#endif
    }
    else {
        if (FIELD_HAS_REF(df)) {
            Py_INCREF((PyObject *)value);
            sk_flag = SkFlagPyObj;
        }
        CHECK_FIELD_INDEX(df);
        vinfo_setitem(po, vi, FIELD_INDEX(df),
                      vinfo_new(CompileTime_NewSk(sk_new(value, sk_flag))));
    }
}

/*****************************************************************/

typedef struct {
    parked_state_t state;
    resume_fn_t resume_fn;
    void *jump_to_fix;
} coding_pause_t;

static code_t *
do_resume_coding_body(coding_pause_t *cp);
/* a stub function: 'value' (RSI) is used only by promotions */
static code_t *
do_resume_coding(coding_pause_t *cp, long value)
{
    code_t *r;
    le_compile_guard_t g;
    LE_COMPILE_BEGIN(LE_ENTER_PAUSE);
    psyco_guard_push(&g);
    if (sigsetjmp(g.env, 0) != 0)
        psyco_guard_retry(&g);
    r = do_resume_coding_body(cp);
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_PAUSE);
    return psyco_code_moved(r);
}

static code_t *
do_resume_coding_body(coding_pause_t *cp)
{
    /* called when entering a coding_pause (described by 'cp'). The compile
       consumes the state, so it starts from a copy, and the parked one is
       kept until it is done, for a compile that runs out of memory to start
       again from. */
    vinfo_t *unused;
    code_t *target =
        (cp->resume_fn)(psyco_unpark(&cp->state, &unused), cp + 1);

    /* then fix the jump to point to 'target' */
    change_cond_jump_target(cp->jump_to_fix, target);

    /* the only way in was that jump: the stub is never entered again */
    psyco_park_release(&cp->state);
    return target;
}

/* a pause never reached when its owner dies */
static void
coding_pause_release(void *closure)
{
    psyco_park_release(&((coding_pause_t *)closure)->state);
}

/* Prepare a 'coding pause', i.e. a stub (idispatcher.h) that will be
   entered only if the execution actually reaches it to go on with
   compilation.
   'po' is the PsycoObject corresponding to the stub.
   'jmpcondition' should not be CC_ALWAYS_FALSE.
   The (possibly conditional) jump to the stub is written at 'po->code'.
   When the execution reaches the stub, 'resume_fn' is called, the original
   jump is repointed to the newly compiled code and the record is freed. */
DEFINEFN
code_t *
psyco_coding_pause(PsycoObject *po, condition_code_t jmpcondition,
                   resume_fn_t resume_fn, void *extra, size_t extrasize)
{
    coding_pause_t *cp;
    code_t *stub, *end;

    /* the trampoline's call clobbers the flags: a condition
       still live in them, such as a virtual bool just compared, is saved in a
       register first, in the calling code. A conditional pause jumps on the
       flags, so there the only live condition must be the one it jumps on. */
    if (jmpcondition == CC_ALWAYS_TRUE) {
        code_t *code = po->code;
        NEED_CC();
        po->code = code;
    }

    /* the stub's record holds a coding_pause_t, then the 'extra' data */
    cp = (coding_pause_t *)psyco_new_stub(
        &do_resume_coding, sizeof(coding_pause_t) + extrasize, &stub);
    psyco_resolved_cc(po, jmpcondition); /* jmpcondition is true if we follow
                                            the branch */
    extra_assert(!HAS_CCREG(po));
    cp->resume_fn = resume_fn;
    if (extrasize > 0) /* 'extra' may be NULL */
        memcpy(cp + 1, extra, extrasize);
    psyco_stub_set_release(cp, &coding_pause_release);

    /* write the jump to the stub */
    cp->jump_to_fix = conditional_jump_to(po, stub, jmpcondition);
    LE_RELOC_SLOT_END(&cp->jump_to_fix);
    end = po->code;
    psyco_park(&cp->state, po, NULL);
    return end;
}

/* for psyco_coding_pause(): a resume function that simply resumes compilation.
 */
static code_t *
psyco_resume_compile(PsycoObject *po, void *extra)
{
    mergepoint_t *mp =
        psyco_exact_merge_point(po->pr.merge_points, po->pr.next_instr);
    /* check that we are not compiling recursively, or at least not too deeply
     */
    extra_assert(psyco_code_open_count() < OPEN_EXTENTS_MAX);
    return (code_t *)psyco_compile_code(po, mp)->codestart;
}

#if LE_CODESTATS
/* The state each new block at a merge point starts from:
   (code object, instruction, one string per vlocals slot). R is a
   run-time value, C/F a constant (F: fixed, i.e. promoted), V a virtual
   one by its computer, with the known fields in brackets. */
DEFINEVAR PyObject *psyco_codestats_states = NULL;

static void
cs_sig(char **p, char *end, vinfo_t *vi, int depth)
{
    int i;
    if (*p >= end - 64)
        return;
    if (vi == NULL) {
        *p += sprintf(*p, "-");
        return;
    }
    if (is_runtime(vi->source))
        *p += sprintf(*p, "R");
    else if (is_compiletime(vi->source)) {
        source_known_t *sk = CompileTime_Get(vi->source);
        *p += sprintf(*p, "%c%lx",
                      (sk->refcount1_flags & SkFlagFixed) ? 'F' : 'C',
                      (unsigned long)sk->value);
    }
    else
        *p += sprintf(*p, "V%lx",
                      (unsigned long)VirtualTime_Get(vi->source)->compute_fn);
    if (vi->array != NullArray && depth < 4) {
        *p += sprintf(*p, "[");
        for (i = 0; i < vi->array->count; i++) {
            if (i)
                *p += sprintf(*p, ",");
            cs_sig(p, end, vi->array->items[i], depth + 1);
        }
        *p += sprintf(*p, "]");
    }
}

/* leaves the thread's error indicator as it found it, as pycompiler.c's
   counters do */
static void
cs_state(PsycoObject *po)
{
    PyObject *slots, *t, *et, *ev, *etb;
    int i;
    char buf[4096];
    PyErr_Fetch(&et, &ev, &etb);
    if (psyco_codestats_states == NULL &&
        (psyco_codestats_states = PyList_New(0)) == NULL)
        OUT_OF_MEMORY();
    slots = PyList_New(po->vlocals.count);
    if (slots == NULL)
        OUT_OF_MEMORY();
    for (i = 0; i < po->vlocals.count; i++) {
        char *p = buf;
        cs_sig(&p, buf + sizeof(buf), po->vlocals.items[i], 0);
        PyList_SET_ITEM(slots, i, PyUnicode_FromStringAndSize(buf, p - buf));
    }
    t = Py_BuildValue("(OiNi)", po->pr.co, po->pr.next_instr, slots,
                      psyco_cs_align_rejects);
    if (t == NULL || PyList_Append(psyco_codestats_states, t) < 0)
        OUT_OF_MEMORY();
    Py_DECREF(t);
    PyErr_Restore(et, ev, etb);
}
#endif

/* The cap per merge point: once this many
   blocks have been compiled at a merge point, a state that matches none of
   them is generalised (dispatcher.c:psyco_forget_values) and compared
   again, so it joins a general block instead of starting one more. The
   count changes over time, but a respawn replay never passes a merge
   point: it starts from the snapshot of the block, taken after this
   decision, and the generalisation emits no respawn point. 0 disables. */
#ifndef MP_BLOCK_CAP
#define MP_BLOCK_CAP 6
#endif
static bool
mp_capped(mergepoint_t *mp)
{
    return MP_BLOCK_CAP > 0 && mp != NULL && mp->blocks >= MP_BLOCK_CAP;
}

/* counts what kinds of virtual values the locals of a new block at 'mp'
   hold, for dispatcher.c:made_real_when_dead */
static void
mp_count_virtuals(PsycoObject *po, mergepoint_t *mp)
{
    int i, n = le_code_nlocalsplus(po->pr.co);
    if (po->pr.is_inlining)
        return;
    for (i = 0; i < n && INDEX_LOC_LOCALS_PLUS + i < po->vlocals.count; i++) {
        vinfo_t *vi = LOC_LOCALS_PLUS[i];
        source_virtual_t *sv;
        if (vi == NULL || !is_virtualtime(vi->source))
            continue;
        sv = VirtualTime_Get(vi->source);
        if (sv == &psyco_computed_long || sv == &psyco_computed_bool ||
            sv == &psyco_computed_float)
            mp->virtual_numbers++;
        else
            mp->virtual_objects++;
    }
}

static void
mp_new_block(PsycoObject *po, mergepoint_t *mp)
{
    if (mp == NULL)
        return;
    mp->blocks++;
    /* only up to the cap, so every block that makes values real here sees
       the same counts */
    if (mp->blocks <= MP_BLOCK_CAP)
        mp_count_virtuals(po, mp);
    /* blocks made from now on make dead virtual locals real here
       (dispatcher.c:psyco_make_dead_virtuals_real) */
    if (mp->blocks == MP_BLOCK_CAP && mp->real_from == 0)
        mp->real_from = psyco_next_serial + 1;
    if (mp->blocks == MP_BLOCK_CAP && !po->pr.is_inlining) {
        /* a code object with a merge point at its cap compiles many copies
           of its code: blocks made from now on inline no calls, whose
           bodies would be copied too (psyfunc.c:psyco_call_pyfunc) */
        le_code_extra_t *x = le_code_extra_get(po->pr.co);
        if (x != NULL && x->inline_end == 0)
            x->inline_end = psyco_next_serial + 1;
    }
    po->mp_serial = po->respawn_proxy->serial;
#if LE_CODESTATS
    cs_state(po);
#endif
}

/* Main compiling function. Emit machine code corresponding to the state
   'po'. The compiler produces its code into 'code' and the return value is
   the end of the written code. 'po' is freed. */
DEFINEFN
code_t *
psyco_compile(PsycoObject *po, mergepoint_t *mp, bool continue_compilation)
{
    vcompatible_t *cmp;
    BEGIN_CODE
    XMM_SPILL();
    END_CODE
    cmp = mp == NULL ? NULL : psyco_compatible(po, &mp->entries);

    /*psyco_assert_cleared_tmp_marks(&po->vlocals);  -- not needed -- */

    if (cmp != NULL && cmp->diff == NullArray) /* exact match, jump there */
    {
        CodeBufferObject *oldcodebuf;
        return psyco_unify(po, cmp, &oldcodebuf);
    }
    else {
        if (po->codelimit - po->code <= BUFFER_MARGIN && cmp == NULL) {
            /* Running out of space in this buffer. */

            /* Instead of going on we stop now and make ready to
               start the new buffer later, when the execution actually
               reaches this point. This forces the emission of code to
               pause at predicible intervals. Among other advantages it
               prevents long or infinite loops from exploding the memory
               while the user sees no progression in the execution of
               her program.
             */
            psyco_compatible_keep_facts = false;
            return psyco_coding_pause(po, CC_ALWAYS_TRUE,
                                      &psyco_resume_compile, NULL, 0);
        }

        /* Code compiled for a promoted value goes on under its code
           object's owner from a merge point whose state that owner holds
           all of, in an extent of its own, so that it doesn't keep the
           value's code alive: other values' code jumps in here too. The
           owner stays until the promotion's compile ends. */
        if (mp != NULL) {
            le_owner_t *root = psyco_owner_root_holding(&po->vlocals);
            if (root != NULL) {
                psyco_owner = root;
                psyco_code_continue(&po->code, &po->codelimit);
            }
        }

        /* Enough space left, continue in the same buffer. */
        {
            CodeBufferObject *codebuf;
            codebuf =
                psyco_proxy_code_buffer(po, mp != NULL ? &mp->entries : NULL);
            po->code = insn_code_label(codebuf->codestart);
        }

        if (cmp != NULL) /* partial match */
        {
            /* cmp->diff points to an array of vinfo_ts: make them run-time */
            int i;
            for (i = cmp->diff->count; i--;)
                psyco_unfix(po, cmp->diff->items[i]);
            psyco_stabilize(cmp);
            /* start over (maybe we have already seen this new state) */
            return psyco_compile(po, mp, continue_compilation);
        }

        /* a second block at this merge point: large int constants become
           run-time objects first. Replay-safe for the same
           reasons as the cap. */
        if (mp != NULL && mp->blocks > 0 && psyco_unpromote_large_ints(po))
            return psyco_compile(po, mp, continue_compilation);

        if (mp_capped(mp) && psyco_forget_values(po, &mp->entries))
            return psyco_compile(po, mp, continue_compilation);

        /* a promotion's compare at its own instruction ends here: what is
           compiled next runs instructions (dispatcher.c) */
        psyco_compatible_keep_facts = false;
        mp_new_block(po, mp);
        if (continue_compilation)
            return NULL; /* I won't actually compile myself, let the caller
                            know */

        /* call the entry point function which performs the actual compilation
         */
        return GLOBAL_ENTRY_POINT(po);
    }
}

/* Join points. A split site
   (pycompiler.c, split_after_global) ends its block, and the blocks started
   after it at one instruction are listed like a merge point's ('ge',
   mergepoints.c:psyco_join_point). A branch respawned from the site's
   checks (its block is a respawned one) usually ends in the state the main
   path had there, so it is compared as at a merge point: it jumps into a
   compatible block, constants that differ made run-time, and the known
   fields of run-time values that the block doesn't have are dropped
   (psyco_compatible_join), instead of compiling its own copy of the code
   up to the next merge point. Any other path only jumps into an exact
   match: un-promoting or dropping there would make the main path lose
   facts. Both rewrite the state's small ints first (canonical_int), as at
   a merge point. Merge points are never inside an inlined frame, join
   points can be: the caller's code object and position there are fixed
   constants (psyfunc.c:psyco_save_inline_po), never made run-time.
   Replay-safe as merge points are: a replay never reaches the end of its
   block, a joined block's respawn points replay from its own snapshot,
   and un-promotion and unification emit no respawn point. */
#if ALL_CHECKS
DEFINEFN long psyco_stat_joins[4];
#endif

static code_t *
compile_join(PsycoObject *po, global_entries_t *ge, bool branch, bool first)
{
    vcompatible_t *cmp;
    CodeBufferObject *codebuf;
    BEGIN_CODE
    XMM_SPILL();
    END_CODE
    cmp = ge == NULL ? NULL : psyco_compatible_join(po, ge, branch);
    /* code that is no group's compiles its own block rather than keep a
       group's alive by jumping into it */
    if (cmp != NULL && psyco_owner_would_hold(cmp->matching)) {
        psyco_stabilize(cmp);
        cmp = NULL;
    }

    if (cmp != NULL && cmp->diff == NullArray) {
        CodeBufferObject *oldcodebuf;
        if (first)
            LE_STAT(psyco_stat_joins[branch ? 0 : 3]++);
        return psyco_unify(po, cmp, &oldcodebuf);
    }
    if (cmp != NULL && !branch) {
        psyco_stabilize(cmp);
        cmp = NULL;
    }
    if (branch && first)
        LE_STAT(psyco_stat_joins[cmp != NULL ? 1 : 2]++);
    codebuf = psyco_proxy_code_buffer(po, ge);
    po->code = insn_code_label(codebuf->codestart);
    if (cmp != NULL) {
        int i;
        for (i = cmp->diff->count; i--;) psyco_unfix(po, cmp->diff->items[i]);
        psyco_stabilize(cmp);
        return compile_join(po, ge, branch, false);
    }
    return NULL;
}

DEFINEFN
code_t *
psyco_compile_join(PsycoObject *po, global_entries_t *ge)
{
    bool branch = po->respawn_proxy != NULL &&
                  po->respawn_proxy->snapshot.fz_respawned_from != NULL;
    return compile_join(po, ge, branch, true);
}

DEFINEFN
void
psyco_compile_cond(PsycoObject *po, mergepoint_t *mp,
                   condition_code_t condition)
{
    PsycoObject *po2 = PsycoObject_Duplicate(po);
    vcompatible_t *cmp;
    psyco_resolved_cc(po2, condition);
    psyco_resolved_cc(po, INVERT_CC(condition));
    /* the exact match's moves go in a short block, with no room to spill
       XMM registers first: a state holding them is compiled later, as a
       new block, by psyco_compile_code */
    cmp = mp == NULL || psyco_xmm_live(po2)
              ? NULL
              : psyco_compatible(po2, &mp->entries);

    extra_assert((int)condition < CC_TOTAL);

    if (cmp != NULL && cmp->diff == NullArray) /* exact match */
    {
        /* try to emit:
                             JNcond Label
                             <unification-and-jump>
                            Label:

           if <unification-and-jump> is only a JMP, recode the whole as a
           single Jcond <unification-jump-target>
        */
        CodeBufferObject *oldcodebuf;
        code_t *codeend;
        void *extra = setup_conditional_code_bounds(po, po2, condition);
        codeend = psyco_unify(po2, cmp, &oldcodebuf);
        make_code_conditional(po, codeend, condition, extra);
    }
    else {
        /* Use the conditional-compiling abilities of
           coding_pause(); it will write a Jcond to a proxy
           which will perform the actual compilation later.
        */
        if (cmp != NULL)
            psyco_stabilize(cmp);
        po->code =
            psyco_coding_pause(po2, condition, &psyco_resume_compile, NULL, 0);
    }
}

/* Simplified interface to compile() without using a previously
   existing code buffer. Return a new code buffer. */
DEFINEFN
CodeBufferObject *
psyco_compile_code(PsycoObject *po, mergepoint_t *mp)
{
    code_t *code1;
    CodeBufferObject *codebuf;
    bool compile_now;
    vcompatible_t *cmp;

    if (psyco_xmm_live(po)) {
        /* a paused state that held XMM registers: a block of its own
           spills them, then compiles on from the merge point */
        codebuf = psyco_new_code_buffer(po, NULL, &po->codelimit);
        po->code = insn_code_label(codebuf->codestart);
        code1 = psyco_compile(po, mp, false);
        CLOSE_CODE("block", codebuf->codestart, code1);
        return codebuf;
    }
    cmp = mp == NULL ? NULL : psyco_compatible(po, &mp->entries);

    /*psyco_assert_cleared_tmp_marks(&po->vlocals);  -- not needed -- */

    if (cmp != NULL && cmp->diff == NullArray) /* exact match */
        return psyco_unify_code(po, cmp);

    /* We compile the new code right now if we have a full mismatch and if
       there are not too many extents being written (codemanager.h) */
    compile_now = cmp == NULL && psyco_code_open_count() < OPEN_EXTENTS_MAX;
    if (cmp == NULL && !compile_now)
        mp = NULL; /* we are about to write a coding pause,
                      don't register it in mp->entries */

    /* Normal case. Start a new buffer */
    codebuf = psyco_new_code_buffer(po, mp == NULL ? NULL : &mp->entries,
                                    &po->codelimit);
    po->code = insn_code_label(codebuf->codestart);

    if (compile_now && mp != NULL && mp->blocks > 0 &&
        psyco_unpromote_large_ints(po))
        code1 = psyco_compile(po, mp, false);
    else if (compile_now && mp_capped(mp) &&
             psyco_forget_values(po, &mp->entries))
        code1 = psyco_compile(po, mp, false);
    else if (compile_now) {
        /* call the entry point function which performs the actual compilation
           (this is the usual case) */
        mp_new_block(po, mp);
        code1 = GLOBAL_ENTRY_POINT(po);
    }
    else if (cmp != NULL) /* partial match */
    {
        int i;
        for (i = cmp->diff->count; i--;) psyco_unfix(po, cmp->diff->items[i]);
        psyco_stabilize(cmp);
        /* start over (maybe we have already seen this new state) */
        code1 = psyco_compile(po, mp, false);
    }
    else {
        /* too many extents being written. This occurs when compiling a
           function that calls a function that needs compiling, recursively.
           Delay compilation until run-time, when psyco_code_open_count() will
           be much smaller. */
        code1 = psyco_coding_pause(po, CC_ALWAYS_TRUE, &psyco_resume_compile,
                                   NULL, 0);
    }

    CLOSE_CODE("block", codebuf->codestart, code1);
    return codebuf;
}

/*****************************************************************/

static bool
computed_do_not_use(PsycoObject *po, vinfo_t *vi)
{
    fprintf(stderr, "psyco: internal error (computed_do_not_use)\n");
    extra_assert(0); /* stop if debugging */
    vi->source = SOURCE_DUMMY;
    return true;
}

static PyObject *
direct_computed_do_not_use(vinfo_t *vi, char *data)
{
    PyErr_SetString(PyExc_RuntimeError,
                    "internal error (direct_computed_do_not_use)");
    extra_assert(0); /* stop if debugging */
    return NULL;
}

INITIALIZATIONFN
void
psyco_compiler_init(void)
{
    INIT_SVIRTUAL(psyco_vsource_not_important, computed_do_not_use,
                  direct_computed_do_not_use, 0, 0, 0);
}

DEFINEFN
mergepoint_t *
PsycoObject_Ready(PsycoObject *po)
{
    mergepoint_t *mp;
    psyco_assert_coherent(po);
    mp = psyco_first_merge_point(po->pr.merge_points);
    psyco_forget_dead_vars(po, &mp->entries, false);
    return mp;
}

static bool
is_cell_arg(PyCodeObject *co, int arg)
{
    int i;
    for (i = 0; i < le_code_ncells(co); i++)
        if (le_code_cell_arg(co, i) == arg)
            return true;
    return false;
}

/* an argument CPython moved into its cell, leaving its own slot NULL
   (ceval.c:_PyEval_EvalCodeWithName); only LOAD_DEREF reads it then */
static bool
unbound_cell_arg(PyCodeObject *co, int arg)
{
    return is_cell_arg(co, arg) && !le_code_local_is_cell(co, arg);
}

/* the *args or **kwargs argument in the frame slot at the stack depth,
   of type 'tp', or the unbound constant if CPython moved it into its
   cell */
static vinfo_t *
star_argument(PsycoObject *po, PyCodeObject *co, int arg, PyTypeObject *tp)
{
    vinfo_t *v;
    if (unbound_cell_arg(co, arg))
        return psyco_vi_Zero();
    v = vinfo_new(RunTime_NewStack(po->stack_depth, false, false));
    v->array = array_new(iOB_TYPE + 1);
    v->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)tp));
    return v;
}

/* a local after the arguments is also a cell (an inlined comprehension's
   variable that a closure in it reads) */
static bool
has_cell_local(PyCodeObject *co, int argc)
{
    int i;
    for (i = argc; i < le_code_nlocals(co); i++)
        if (le_code_local_is_cell(co, i))
            return true;
    return false;
}

/* The frame's globals and builtins, as constants. The globals are
   borrowed: compiled code holds its constants until its code object dies
   (sk_new), and here that would close a cycle the GC can't see, globals
   -> function -> code -> record -> owner -> globals, in all code with
   globals of its own (exec, namedtuple), which only the owner scan
   (codemanager.c) would find. Borrowing is safe because compiled code
   uses the dict only as an argument to run-time calls, never its contents
   at compile time: it runs only in a frame that holds the dict, and if a
   new dict later gets a dead one's address, the old code does for it what
   new code would. Code that reads globals at compile time (say a cached
   LOAD_GLOBAL) must take its own reference. */
static void
frame_globals(PsycoObject *po, PyObject *globals, PyObject *builtins)
{
    source_known_t *sk;
    sk = sk_new((long)globals, SkFlagPyObj | SkFlagBorrowed);
    LOC_GLOBALS = vinfo_new(CompileTime_NewSk(sk));
    /* the frame's f_builtins, owned: lookups read it at compile time, and a
       dict swapped in for a while may not outlive the code */
    Py_INCREF(builtins);
    LOC_BUILTINS =
        vinfo_new(CompileTime_NewSk(sk_new((long)builtins, SkFlagPyObj)));
}

DEFINEFN
PsycoObject *
PsycoObject_FromCode(PyCodeObject *co, PyObject *globals, PyObject *builtins,
                     PyObject *merge_points, int recursion)
{
    int i, argc, extras;
    PsycoObject *po;
    Source rsrc;
    vinfo_t *v;

    extras = co->co_stacksize + le_code_nlocalsplus(co);

    po = PsycoObject_New(INDEX_LOC_LOCALS_PLUS + extras);
    po->stack_depth = INITIAL_STACK_DEPTH;
    po->vlocals.count = INDEX_LOC_LOCALS_PLUS + extras;
    INIT_PROCESSOR_PSYCOOBJECT(po);
    po->pr.auto_recursion = AUTO_RECURSION(recursion);

    frame_globals(po, globals, builtins);

    /* keyword-only arguments follow the positional ones in the frame, filled
       in by CPython (only frame entries get here with any: psyfunc.c) */
    argc = co->co_argcount + co->co_kwonlyargcount;
    if (co->co_flags & CO_VARARGS)
        argc++;
    if (co->co_flags & CO_VARKEYWORDS)
        argc++;

    /* initialize the free and cell vars */
    i = le_code_nlocalsplus(co);
    if (i > le_code_nlocals(co) || has_cell_local(co, argc)) {
        while (i > le_code_nlocals(co)) {
            po->stack_depth += sizeof(long);
            /* borrowed references from the frame object */
            rsrc = RunTime_NewStack(po->stack_depth, false, false);
            v = vinfo_new(rsrc);
            LOC_LOCALS_PLUS[--i] = v;
        }
        /* skip the unbound local variables */
        po->stack_depth += sizeof(long) * (i - argc);
    }
    /* initialize the local variables to zero (unbound), but for one that
       is also a cell, which the frame holds from the start
       (frames.h:le_frame_start) */
    while (i > argc) {
        i--;
        if (le_code_local_is_cell(co, i))
            v = vinfo_new(RunTime_NewStack(
                po->stack_depth - (int)sizeof(long) * (i - argc), false,
                false));
        else
            v = psyco_vi_Zero();
        LOC_LOCALS_PLUS[i] = v;
    }
    /* initialize the keyword arguments dict, known to be a dict */
    if (co->co_flags & CO_VARKEYWORDS) {
        po->stack_depth += sizeof(long);
        i--;
        LOC_LOCALS_PLUS[i] = star_argument(po, co, i, &PyDict_Type);
    }
    /* initialize the extra arguments tuple, known to be a tuple */
    if (co->co_flags & CO_VARARGS) {
        po->stack_depth += sizeof(long);
        i--;
        LOC_LOCALS_PLUS[i] = star_argument(po, co, i, &PyTuple_Type);
    }
    /* initialize the regular arguments */
    while (i > 0) {
        po->stack_depth += sizeof(long);
        if (unbound_cell_arg(co, i - 1)) {
            LOC_LOCALS_PLUS[--i] = psyco_vi_Zero();
            continue;
        }
        /* arguments get borrowed references */
        rsrc = RunTime_NewStack(po->stack_depth, false, false);
        v = vinfo_new(rsrc);
        LOC_LOCALS_PLUS[--i] = v;
    }
    /* the rest of the stack in LOC_LOCALS_PLUS is
       initialized to NULL by PsycoObject_New() */

    /* store the code object */
    po->pr.co = co; /* borrowed: see psyfunc.c:fncall_finish_store */
    pyc_data_build(po, merge_points);

    /* set up the CALL return address */
    po->stack_depth += sizeof(long);
    rsrc = RunTime_NewStack(po->stack_depth, false, false);
    LOC_CONTINUATION = vinfo_new(rsrc);
    return po;
}

/* A resumable generator's body entered at 'next_instr' (eval_hook.c:
   LeGen_Run), given its frame, borrowed, and the value sent, owned, which
   is on top of the 'nstack' stack items under it. Every local, cell, free
   variable and stack item starts as the frame's own word
   (pycompiler.c:psyco_frame_word), read where the code uses it, so the
   frame may hold anything ceval or Python left in it. 'blocks' are the
   frame's 'nblocks' blocks, as (type, handler in bytes, level). A body
   whose start has no instruction to pop the value sent (GEN_START) is
   entered there with the value in '*unpushed' instead, for the caller to
   let go (psyco_compile_code_dropping). */
DEFINEFN
PsycoObject *
PsycoObject_FromGenFrame(PyCodeObject *co, PyObject *globals,
                         PyObject *builtins, PyObject *merge_points,
                         int recursion, int next_instr, int nstack,
                         const int *blocks, int nblocks, vinfo_t **unpushed)
{
    int nplus = le_code_nlocalsplus(co);
    int count =
        INDEX_LOC_LOCALS_PLUS + nplus + GENEXPR_SLOTS + co->co_stacksize;
    int i;
    PsycoObject *po = PsycoObject_New(count);
    vinfo_t **stack_a = po->vlocals.items + count - co->co_stacksize;
    vinfo_t *vsent;
    po->stack_depth = INITIAL_STACK_DEPTH;
    po->vlocals.count = count;
    INIT_PROCESSOR_PSYCOOBJECT(po);
    po->pr.auto_recursion = AUTO_RECURSION(recursion);
    frame_globals(po, globals, builtins);
    /* fixed, as a fused generator's mode (psyfunc.c:psyco_call_genexpr) */
    po->vlocals.items[GENEXPR_SLOT_MODE(co)] =
        vinfo_new(CompileTime_NewSk(sk_new(GENEXPR_RESUMABLE, SkFlagFixed)));
    /* the arguments, the last pushed first */
    po->stack_depth += sizeof(long);
    vsent = vinfo_new(RunTime_NewStack(po->stack_depth, true, false));
    *unpushed = NULL;
    if (next_instr == 0 && !LE_HAS_OPCODE_GEN_START)
        *unpushed = vsent;
    else
        stack_a[nstack] = vsent;
    po->stack_depth += sizeof(long);
    po->vlocals.items[GENEXPR_SLOT_ACC(co)] =
        vinfo_new(RunTime_NewStack(po->stack_depth, false, false));
    for (i = 0; i < nplus; i++) LOC_LOCALS_PLUS[i] = psyco_frame_word(i);
    for (i = 0; i < nstack; i++) stack_a[i] = psyco_frame_word(nplus + i);
    po->pr.co = co; /* borrowed, as in PsycoObject_FromCode */
    pyc_data_build(po, merge_points);
    po->pr.next_instr = next_instr;
#if LE_HAS_EXCEPTION_TABLE
    (void)blocks;
    (void)nblocks;
    po->pr.iblock = 0;
#else
    for (i = 0; i < nblocks; i++) {
        po->pr.blockstack[i].b_type = blocks[3 * i];
        po->pr.blockstack[i].b_handler = blocks[3 * i + 1];
        po->pr.blockstack[i].b_level = blocks[3 * i + 2];
    }
    po->pr.iblock = nblocks;
#endif
    po->stack_depth += sizeof(long);
    LOC_CONTINUATION =
        vinfo_new(RunTime_NewStack(po->stack_depth, false, false));
    return po;
}

/* As psyco_compile_code, for an entry that lets go of 'v' before the
   code compiled from 'mp' */
DEFINEFN
CodeBufferObject *
psyco_compile_code_dropping(PsycoObject *po, mergepoint_t *mp, vinfo_t *v)
{
    CodeBufferObject *codebuf =
        psyco_new_code_buffer(po, NULL, &po->codelimit);
    code_t *code1;
    po->code = insn_code_label(codebuf->codestart);
    vinfo_decref(v, po);
    code1 = psyco_compile(po, mp, false);
    CLOSE_CODE("block", codebuf->codestart, code1);
    return codebuf;
}
