/*
 * psyco_compile() makes the merge-point decision. It compares the current state
 * with the snapshots stored at this merge point (dispatcher.c:psyco_compatible):
 *   - exact match:   emit unification moves and jump to the existing code;
 *   - partial match: un-promote the differing constants to run-time and retry;
 *   - no match:      start new code (or a coding pause if the buffer is full).
 * A coding pause is a stub that compiles the continuation only when execution
 * first reaches it.
 *
 * Field access (psyco_internal_getfld/putfld) folds loads from compile-time
 * objects in C and caches results in vinfo->array.
 *
 * References to compiled code buffers are mostly dropped ("XXX don't know
 * what to do with reference"): those buffers, and machine code, are never
 * freed. Only the buffers in a code object's fatlists die with it (C14).
 */
#include "vcompiler.h"
#include "Python/pycompiler.h"
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
vinfo_array_t *array_grow1(vinfo_array_t *array, int ncount) {
  int i = array->count;
  extra_assert(ncount > i);
  if (i == 0)
    array = PyMem_MALLOC(offsetof(vinfo_array_t, items) +
                         ncount * sizeof(vinfo_t *));
  else
    array = PyMem_REALLOC(array, offsetof(vinfo_array_t, items) +
                                     ncount * sizeof(vinfo_t *));
  if (array == NULL)
    OUT_OF_MEMORY();
  array->count = ncount;
  while (i < ncount)
    array->items[i++] = NULL;
  return array;
}

DEFINEFN
void vinfo_array_shrink(PsycoObject *po, vinfo_t *vi, int ncount) {
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
  if (ncount == 0)
    array = NullArray;
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
void sk_release(source_known_t *sk) {
#if 0
  XXX --- XXX --- XXX --- XXX --- XXX --- XXX --- XXX --- XXX --- XXX
    The Python objects who get once in a source_known_t are never
    freed. This is so because we might have references to them left
    in the code buffers. This is tricky because the references can
    be indirect (to a sub-objects, to a field in the object structure,
                 etc...)
    So not freeing them at all is the easy way out. It is expected
    that not too many objects will get lost this way. This must be
    carefully worked out when implementing releasing of code
    buffers. It will probably require careful checks for all
    instructions that might emit an immediate value in the code,
    and for where this immediate value (indirectly or not) comes from.
  XXX --- XXX --- XXX --- XXX --- XXX --- XXX --- XXX --- XXX --- XXX
  if ((sk->refcount1_flags & SkFlagPyObj) != 0) {
    Py_XDECREF((PyObject*)(sk->value));
  }
#endif
  sk_delete(sk);
}

DEFINEFN
vinfo_t *vinfo_copy(vinfo_t *vi) {
  vinfo_t *result = vinfo_new_skref(vi->source);
  result->array = vi->array;
  if (result->array->count > 0) {
    result->array = array_new(result->array->count);
    duplicate_array(result->array, vi->array);
  }
  return result;
}

DEFINEFN
void vinfo_release(vinfo_t *vi, PsycoObject *po) {
  switch (gettime(vi->source)) {

  case RunTime:
    if (po != NULL) {
      if (has_rtref(vi->source)) {
        /* write Py_DECREF() when releasing the last reference to
           a run-time vinfo_t holding a reference to a Python object */
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
     array to extract any available type information to speed up Py_DECREF(). */
  if (vi->array != NullArray)
    array_delete(vi->array, po);

#if HAVE_CCREG && ALL_CHECKS
  /* only virtual-time vinfos are allowed in po->ccreg */
  if (po != NULL) {
    int i;
    for (i = 0; i < HAVE_CCREG; i++)
      extra_assert(vi != po->ccregs[i]);
  }
#endif
  psyco_llfree_vinfo(vi);
}

DEFINEFN
void vinfo_move(PsycoObject *po, vinfo_t *vtarget, vinfo_t *vsource) {
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
  psyco_llfree_vinfo(vsource);
  clear_tmp_marks(vtarget->array);
  psyco_simplify_array(vtarget->array, po);
}

DEFINEFN
bool vinfo_would_be_recursive(vinfo_t *vi, vinfo_t *newitem) {
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
void clear_tmp_marks(vinfo_array_t *array) {
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
void assert_cleared_tmp_marks(vinfo_array_t *array) {
  /* assert that all 'tmp' fields are NULL */
  int i = array->count;
  while (i--)
    if (array->items[i] != NULL) {
      extra_assert(array->items[i]->tmp == NULL);
      if (array->items[i]->array != NullArray)
        assert_cleared_tmp_marks(array->items[i]->array);
    }
}

static bool array_contains(vinfo_array_t *array, vinfo_t *vi) {
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
void assert_array_contains_nonct(vinfo_array_t *array, vinfo_t *vi) {
  /* check that 'vi' appears at least once in 'array', and
     never appears as subitem of a compile-time vinfo_t */
  extra_assert(array_contains(array, vi));
}

static void coherent_array(vinfo_array_t *source, PsycoObject *po, int found[],
                           bool allow_any) {
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

static void hack_refcounts(vinfo_array_t *source, int delta, int mvalue) {
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

static vinfo_t *nonnull_refcount(vinfo_array_t *source) {
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
void psyco_assert_coherent1(PsycoObject *po, bool full) {
  vinfo_array_t debug_extra_refs;
  int found[REG_TOTAL + HAVE_CCREG];
  int i;
  vinfo_t *err;
  for (i = 0; i < REG_TOTAL + HAVE_CCREG; i++)
    found[i] = 0;
  debug_extra_refs.count = 3;
  debug_extra_refs.items[0] = po->pr.exc; /* normally private to pycompiler.c,*/
  debug_extra_refs.items[1] = po->pr.val; /* but this is for debugging only */
  debug_extra_refs.items[2] = po->pr.tb;  /* run-time after END_FINALLY */
  coherent_array(&po->vlocals, po, found, true);
  coherent_array(&debug_extra_refs, po, found, true);
  if (full) {
    RTVINFO_CHECKED(po, found);
#if HAVE_CCREG
    for (i = 0; i < HAVE_CCREG; i++)
      if (!found[REG_TOTAL + i])
        extra_assert(po->ccregs[i] == NULL);
#endif
    hack_refcounts(&po->vlocals, -1, 0);
    hack_refcounts(&debug_extra_refs, -1, 0);
    err = nonnull_refcount(&po->vlocals);
    hack_refcounts(&debug_extra_refs, +1, 0x10000);
    hack_refcounts(&po->vlocals, +1, 0x10000);
    extra_assert(!err); /* see nonnull_refcounts() */
  }
}
#endif /* ALL_CHECKS */

DEFINEFN
void duplicate_array(vinfo_array_t *target, vinfo_array_t *source) {
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
    } else {
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
PsycoObject *psyco_duplicate(PsycoObject *po) {
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
  result->code = po->code;
  result->codelimit = po->codelimit;
  pyc_data_duplicate(&result->pr, &po->pr);

  assert_cleared_tmp_marks(&result->vlocals);
  psyco_assert_coherent(result);
  return result;
}

DEFINEFN void PsycoObject_Delete(PsycoObject *po) {
  pyc_data_release(&po->pr);
  deallocate_array(&po->vlocals, NULL);
  PyMem_FREE(po);
}

DEFINEFN
bool psyco_limit_nested_weight(PsycoObject *po, vinfo_array_t *array,
                               int nw_index, signed char nw_end) {
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
long direct_read_vinfo(vinfo_t *vi, char *data) {
  int sindex;
  if (vi == NULL) {
    PyErr_SetString(PyExc_PsycoError, "undefined value");
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
PyObject *direct_xobj_vinfo(vinfo_t *vi, char *data) {
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
      return VirtualTime_Get(vi->source)->direct_compute_fn(vi, data);
    }
    Py_XINCREF(o);
  }
  return o;
}

/*****************************************************************/

PSY_INLINE vinfo_t *field_read(PsycoObject *po, vinfo_t *vi, long offset,
                               vinfo_t *vindex, defield_t df, bool newref) {
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
vinfo_t *psyco_internal_getfld(PsycoObject *po, int findex, defield_t df,
                               vinfo_t *vi, long offset) {
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
  } else {
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

DEFINEFN
vinfo_t *psyco_get_field_array(PsycoObject *po, vinfo_t *vi, defield_t df,
                               vinfo_t *vindex) {
  long offset = FIELD_C_OFFSET(df);
  if (!compute_vinfo(vindex, po))
    return NULL;

  extra_assert((long)df & FIELD_ARRAY);
  if (is_compiletime(vindex->source)) {
    return psyco_get_nth_field(po, vi, df,
                               CompileTime_Get(vindex->source)->value);
  } else {
    if (!compute_vinfo(vi, po))
      return NULL;
    return field_read(po, vi, offset, vindex, df, true);
  }
}

DEFINEFN
bool psyco_internal_putfld(PsycoObject *po, int findex, defield_t df,
                           vinfo_t *vi, long offset, vinfo_t *value) {
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
    } else {
      /* common case */
      decref_create_new_ref(po, value);
    }
  }
  return true;
}

DEFINEFN
bool psyco_put_field_array(PsycoObject *po, vinfo_t *vi, defield_t df,
                           vinfo_t *vindex, vinfo_t *value) {
  long offset = FIELD_C_OFFSET(df);
  if (!compute_vinfo(vindex, po))
    return false;

  extra_assert((long)df & FIELD_ARRAY);
  if (is_compiletime(vindex->source)) {
    return psyco_put_nth_field(po, vi, df,
                               CompileTime_Get(vindex->source)->value, value);
  } else {
    if (!compute_vinfo(vi, po))
      return false;
    if (v_mem_mov(po, value, vi, vindex, offset, FIELD_SIZE2(df),
                  ((long)df & FIELD_UNSIGNED)) == NULL) {
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
void psyco_assert_field(PsycoObject *po, vinfo_t *vi, defield_t df,
                        long value) {
  long sk_flag = 0;
  extra_assert(!((long)df & FIELD_MUTABLE));

  if (is_compiletime(vi->source)) {
#if ALL_CHECKS
    /* check assertion at compile-time */
    vinfo_t *vf = psyco_get_field(po, vi, df);
    extra_assert(CompileTime_Get(vf->source)->value == value);
    vinfo_decref(vf, po);
#endif
  } else {
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
  CodeBufferObject *self;
  PsycoObject *po;
  resume_fn_t resume_fn;
  void *jump_to_fix;
} coding_pause_t;

static code_t *do_resume_coding_body(coding_pause_t *cp);
static code_t *do_resume_coding(coding_pause_t *cp) {
  code_t *r;
  NI_COMPILE_BEGIN(NI_ENTER_PAUSE);
  r = do_resume_coding_body(cp);
  NI_COMPILE_END(NI_ENTER_PAUSE);
  return r;
}

static code_t *do_resume_coding_body(coding_pause_t *cp) {
  /* called when entering a coding_pause (described by 'cp') */
  code_t *target =
      (cp->resume_fn)(cp->po, cp + 1); /* resume compilation work */

  /* then fix the jump to point to 'target' */
  change_cond_jump_target(cp->jump_to_fix, target);

  /* cannot Py_DECREF(cp->self) because the current function is returning into
     that code now, but any time later is fine: use the trash of codemanager.c
   */
  psyco_trash_object((PyObject *)cp->self);
  return target;
}

/* Prepare a 'coding pause', i.e. a short amount of code (proxy) that will be
   called only if the execution actually reaches it to go on with compilation.
   'po' is the PsycoObject corresponding to the proxy.
   'jmpcondition' should not be CC_ALWAYS_FALSE.
   The (possibly conditional) jump to the proxy is encoded in 'calling_code'.
   When the execution reaches the proxy, 'resume_fn' is called and the proxy
   destroys itself and replaces the original jump to it by a jump to the newly
   compiled code. */
DEFINEFN
void psyco_coding_pause(PsycoObject *po, condition_code_t jmpcondition,
                        resume_fn_t resume_fn, void *extra, size_t extrasize) {
  coding_pause_t *cp;
  code_t *calling_code;
  code_t *calling_limit;
  code_t *limit;
  CodeBufferObject *codebuf;

  /* the proxy's call clobbers the flags (KNOWN_BUGS B8): a condition still
     live in them, such as a virtual bool just compared, is saved in a
     register first, in the calling code. A conditional pause jumps on the
     flags, so there the only live condition must be the one it jumps on. */
  if (jmpcondition == CC_ALWAYS_TRUE) {
    code_t *code = po->code;
    NEED_CC();
    po->code = code;
  }
  codebuf = psyco_new_code_buffer(NULL, NULL, &limit);

  /* the proxy contains only a jump to do_resume_coding,
     followed by a coding_pause_t structure, itself followed by the
     'extra' data. */
  calling_code = po->code;
  calling_limit = po->codelimit;
  po->code = insn_code_label(codebuf->codestart);
  po->codelimit = limit;
  cp = (coding_pause_t *)psyco_call_code_builder(
      po, &do_resume_coding, true, SOURCE_DUMMY,
      sizeof(coding_pause_t) + extrasize);
  SHRINK_CODE_BUFFER(codebuf, po->code, "coding_pause");
  /* fill in the coding_pause_t structure and the following 'extra' data */
  psyco_resolved_cc(po, jmpcondition); /* jmpcondition is true if we follow
                                          the branch */
  /* the proxy's call clobbers the flags (upstream saved them; the x64
     builder does not, KNOWN_BUGS B8): no other condition may still be live */
  extra_assert(!HAS_CCREG(po));
  cp->self = codebuf;
  cp->po = po;
  cp->resume_fn = resume_fn;
  memcpy(cp + 1, extra, extrasize);

  /* write the jump to the proxy */
  po->code = calling_code;
  po->codelimit = calling_limit;
  cp->jump_to_fix =
      conditional_jump_to(po, (code_t *)codebuf->codestart, jmpcondition);
}

/* for psyco_coding_pause(): a resume function that simply resumes compilation.
 */
static code_t *psyco_resume_compile(PsycoObject *po, void *extra) {
  mergepoint_t *mp =
      psyco_exact_merge_point(po->pr.merge_points, po->pr.next_instr);
  /* check that we are not compiling recursively, or at least not too deeply */
  extra_assert(psyco_locked_buffers() < WARN_TOO_MANY_BUFFERS - 1);
  return (code_t *)psyco_compile_code(po, mp)->codestart;
  /* XXX don't know what to do with the reference returned by
     XXX psyco_compile_code() */
}

/* Main compiling function. Emit machine code corresponding to the state
   'po'. The compiler produces its code into 'code' and the return value is
   the end of the written code. 'po' is freed. */
DEFINEFN
code_t *psyco_compile(PsycoObject *po, mergepoint_t *mp,
                      bool continue_compilation) {
  vcompatible_t *cmp = mp == NULL ? NULL : psyco_compatible(po, &mp->entries);

  /*psyco_assert_cleared_tmp_marks(&po->vlocals);  -- not needed -- */

  if (cmp != NULL && cmp->diff == NullArray) /* exact match, jump there */
  {
    CodeBufferObject *oldcodebuf;
    code_t *code2 = psyco_unify(po, cmp, &oldcodebuf);
    /* XXX store reference to oldcodebuf somewhere */
    return code2;
  } else {
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
      psyco_coding_pause(po, CC_ALWAYS_TRUE, &psyco_resume_compile, NULL, 0);
      return po->code;
    }

    /* Enough space left, continue in the same buffer. */
    {
      CodeBufferObject *codebuf =
          psyco_proxy_code_buffer(po, mp != NULL ? &mp->entries : NULL);
      /*Py_DECREF(codebuf); XXX cannot loose reference if mp == NULL*/
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

    if (continue_compilation)
      return NULL; /* I won't actually compile myself, let the caller know */

    /* call the entry point function which performs the actual compilation */
    return GLOBAL_ENTRY_POINT(po);
  }
}

DEFINEFN
void psyco_compile_cond(PsycoObject *po, mergepoint_t *mp,
                        condition_code_t condition) {
  PsycoObject *po2 = PsycoObject_Duplicate(po);
  vcompatible_t *cmp;
  psyco_resolved_cc(po2, condition);
  psyco_resolved_cc(po, INVERT_CC(condition));
  cmp = mp == NULL ? NULL : psyco_compatible(po2, &mp->entries);

  extra_assert((int)condition < CC_TOTAL);

  if (cmp != NULL && cmp->diff == NullArray) /* exact match */
  {
    /* try to emit:
                         JNcond Label
                         <unification-and-jump>
                        Label:

       if <unification-and-jump> is only a JMP, recode the whole as a single
                         Jcond <unification-jump-target>
    */
    CodeBufferObject *oldcodebuf;
    code_t *codeend;
    void *extra = setup_conditional_code_bounds(po, po2, condition);
    codeend = psyco_unify(po2, cmp, &oldcodebuf);
    make_code_conditional(po, codeend, condition, extra);
    /* XXX store reference to oldcodebuf somewhere */
  } else {
    /* Use the conditional-compiling abilities of
       coding_pause(); it will write a Jcond to a proxy
       which will perform the actual compilation later.
    */
    if (cmp != NULL)
      psyco_stabilize(cmp);
    psyco_coding_pause(po2, condition, &psyco_resume_compile, NULL, 0);
    po->code = po2->code;
  }
}

/* Simplified interface to compile() without using a previously
   existing code buffer. Return a new code buffer. */
DEFINEFN
CodeBufferObject *psyco_compile_code(PsycoObject *po, mergepoint_t *mp) {
  code_t *code1;
  CodeBufferObject *codebuf;
  bool compile_now;
  vcompatible_t *cmp = mp == NULL ? NULL : psyco_compatible(po, &mp->entries);

  /*psyco_assert_cleared_tmp_marks(&po->vlocals);  -- not needed -- */

  if (cmp != NULL && cmp->diff == NullArray) /* exact match */
    return psyco_unify_code(po, cmp);

  /* We compile the new code right now if we have a full mismatch and if
     there are not too many locked big buffers in codemanager.c */
  compile_now =
      cmp == NULL && psyco_locked_buffers() < WARN_TOO_MANY_BUFFERS - 1;
  if (cmp == NULL && !compile_now)
    mp = NULL; /* we are about to write a coding pause,
                  don't register it in mp->entries */

  /* Normal case. Start a new buffer */
  codebuf = psyco_new_code_buffer(po, mp == NULL ? NULL : &mp->entries,
                                  &po->codelimit);
  po->code = insn_code_label(codebuf->codestart);

  if (compile_now) {
    /* call the entry point function which performs the actual compilation
       (this is the usual case) */
    code1 = GLOBAL_ENTRY_POINT(po);
  } else if (cmp != NULL) /* partial match */
  {
    int i;
    for (i = cmp->diff->count; i--;)
      psyco_unfix(po, cmp->diff->items[i]);
    psyco_stabilize(cmp);
    /* start over (maybe we have already seen this new state) */
    code1 = psyco_compile(po, mp, false);
  } else {
    /* detected too many locked buffers. This occurs when compiling a
       function that calls a function that needs compiling, recursively.
       Delay compilation until run-time, when psyco_locked_buffers() will
       be much smaller. */
    psyco_coding_pause(po, CC_ALWAYS_TRUE, &psyco_resume_compile, NULL, 0);
    code1 = po->code;
  }

  /* we have written some code into a new codebuf, now shrink it to
     its actual size */
  psyco_shrink_code_buffer(codebuf, code1);
  return codebuf;
}

/*****************************************************************/

static bool computed_do_not_use(PsycoObject *po, vinfo_t *vi) {
  fprintf(stderr, "psyco: internal error (computed_do_not_use)\n");
  extra_assert(0); /* stop if debugging */
  vi->source = SOURCE_DUMMY;
  return true;
}

static PyObject *direct_computed_do_not_use(vinfo_t *vi, char *data) {
  PyErr_SetString(PyExc_PsycoError,
                  "internal error (direct_computed_do_not_use)");
  extra_assert(0); /* stop if debugging */
  return NULL;
}

INITIALIZATIONFN
void psyco_compiler_init(void) {
  INIT_SVIRTUAL(psyco_vsource_not_important, computed_do_not_use,
                direct_computed_do_not_use, 0, 0, 0);
}

DEFINEFN
mergepoint_t *PsycoObject_Ready(PsycoObject *po) {
  mergepoint_t *mp;
  psyco_assert_coherent(po);
  mp = psyco_first_merge_point(po->pr.merge_points);
  psyco_delete_unused_vars(po, &mp->entries);
  return mp;
}

static bool is_cell_arg(PyCodeObject *co, int arg) {
  Py_ssize_t i;
  if (co->co_cell2arg == NULL)
    return false;
  for (i = 0; i < PyTuple_GET_SIZE(co->co_cellvars); i++)
    if (co->co_cell2arg[i] == arg)
      return true;
  return false;
}

DEFINEFN
PsycoObject *PsycoObject_FromCode(PyCodeObject *co, PyObject *globals,
                                  PyObject *merge_points, int recursion) {
  int i, argc, ncells, nfrees, extras;
  PsycoObject *po;
  Source rsrc;
  source_known_t *sk;
  vinfo_t *v;

  ncells = PyTuple_GET_SIZE(co->co_cellvars);
  nfrees = PyTuple_GET_SIZE(co->co_freevars);
  extras = co->co_stacksize + co->co_nlocals + ncells + nfrees;

  po = PsycoObject_New(INDEX_LOC_LOCALS_PLUS + extras);
  po->stack_depth = INITIAL_STACK_DEPTH;
  po->vlocals.count = INDEX_LOC_LOCALS_PLUS + extras;
  INIT_PROCESSOR_PSYCOOBJECT(po);
  po->pr.auto_recursion = AUTO_RECURSION(recursion);

  /* initialize po->vlocals. The globals are borrowed (C14): compiled code
     keeps its constants forever (sk_release), and here that closed a cycle
     the GC can't see, globals -> function -> code -> co_extra -> buffers ->
     globals, so code with globals of its own (exec, namedtuple) never died.
     Borrowing is safe because compiled code uses the dict only as an
     argument to run-time calls, never its contents at compile time: it
     runs only in a frame that holds the dict, and if a new dict later gets
     a dead one's address, the old code does for it what new code would.
     Code that reads globals at compile time (say a cached LOAD_GLOBAL)
     must take its own reference (DECISIONS D23). */
  sk = sk_new((long)globals, SkFlagPyObj);
  LOC_GLOBALS = vinfo_new(CompileTime_NewSk(sk));

  /* keyword-only arguments follow the positional ones in the frame, filled
     in by CPython (only frame entries get here with any: psyfunc.c) */
  argc = co->co_argcount + co->co_kwonlyargcount;
  if (co->co_flags & CO_VARARGS)
    argc++;
  if (co->co_flags & CO_VARKEYWORDS)
    argc++;

  /* initialize the free and cell vars */
  i = co->co_nlocals + ncells + nfrees;
  if (ncells || nfrees) {
    while (i > co->co_nlocals) {
      po->stack_depth += sizeof(long);
      /* borrowed references from the frame object */
      rsrc = RunTime_NewStack(po->stack_depth, false, false);
      v = vinfo_new(rsrc);
      LOC_LOCALS_PLUS[--i] = v;
    }
    /* skip the unbound local variables */
    po->stack_depth += sizeof(long) * (i - argc);
  }
  /* initialize the local variables to zero (unbound) */
  while (i > argc) {
    v = psyco_vi_Zero();
    LOC_LOCALS_PLUS[--i] = v;
  }
  /* initialize the keyword arguments dict */
  if (co->co_flags & CO_VARKEYWORDS) {
    po->stack_depth += sizeof(long);
    rsrc = RunTime_NewStack(po->stack_depth, false, false);
    v = vinfo_new(rsrc);
    /* known to be a dict */
    /*Psyco_AssertType(NULL, v, &PyDict_Type);*/
    rsrc = CompileTime_New((long)&PyDict_Type);
    v->array = array_new(FIELDS_TOTAL(OB_type));
    v->array->items[iOB_TYPE] = vinfo_new(rsrc);
    LOC_LOCALS_PLUS[--i] = v;
  }
  /* initialize the extra arguments tuple */
  if (co->co_flags & CO_VARARGS) {
    po->stack_depth += sizeof(long);
    rsrc = RunTime_NewStack(po->stack_depth, false, false);
    v = vinfo_new(rsrc);
    /* known to be a tuple */
    rsrc = CompileTime_New((long)&PyTuple_Type);
    v->array = array_new(iOB_TYPE + 1);
    v->array->items[iOB_TYPE] = vinfo_new(rsrc);
    LOC_LOCALS_PLUS[--i] = v;
  }
  /* initialize the regular arguments */
  while (i > 0) {
    po->stack_depth += sizeof(long);
    if (is_cell_arg(co, i - 1)) {
      /* CPython moved it into its cell and left NULL (ceval.c:
         _PyEval_EvalCodeWithName); only LOAD_DEREF reads it now */
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
