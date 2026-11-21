/*
 * Virtual lists, for short lists only (length <= VLIST_LENGTH_MAX, i.e. 3).
 * Layout: [iOB_TYPE], [iVAR_SIZE] = length, [LIST_ob_item] (unset while
 * virtual), then the items from VLIST_ITEMS. Lists are mutable, so they are
 * forced out of virtual time at any call boundary (INIT_SVIRTUAL_NOCALL) and
 * whenever they are indexed with a run-time index. compute_vlist emits
 * PyList_New and the item stores, then forgets the size, which becomes
 * mutable. The list tp_iter slot is redirected to the generic virtual
 * seq-iterator (piterobject.c), so a materialised iterator would be an
 * 'iterator' rather than a 'list_iterator'.
 */
#include "plistobject.h"
#include "plongobject.h"
#include "piterobject.h"
#include "punicodeobject.h"
#include "psliceobject.h"
#include "../pycodegen.h"


/***************************************************************/
/* virtual short lists.                                        */

/*DEFINEVAR vinfo_t* psyco_empty_list;*/
static source_virtual_t psyco_computed_vlist;

PSY_INLINE int vlist_length(vinfo_t* v)
{
	int len = v->array->count - VLIST_ITEMS;
#if ALL_CHECKS
	vinfo_t* vlen = vinfo_getitem(v, iVAR_SIZE);
	extra_assert(vlen != NULL);
	extra_assert(len == CompileTime_Get(vlen->source)->value);
#endif
	return len;
}

static bool compute_vlist(PsycoObject* po, vinfo_t* v)
{
	int length = vlist_length(v);
	vinfo_t* newobj;
        vinfo_t* ob_item;

	newobj = psyco_generic_call(po, PyList_New,
				    CfCommonNewRefPyObject,
				    "l", (long) length);
	if (newobj == NULL)
		return false;

	/* write the storing of the objects in the list */
	if (length > 0) {
		int i;
		ob_item = psyco_get_field(po, newobj, LIST_ob_item);
		if (ob_item == NULL)
			goto fail;

		for (i=0; i<length; i++) {
			vinfo_t* vi = v->array->items[VLIST_ITEMS + i];
			extra_assert(vi != NULL);
			if (!psyco_put_nth_field(po, ob_item,
						 FPYREF(LIST_itemsarray),
						 i, vi))
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

PSY_INLINE vinfo_t* list_maybe_compute(PsycoObject* po, vinfo_t* v)
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


PSY_INLINE vinfo_t* PsycoList_NEW(int size)
{
	/* returns a virtual list. The caller should use list_maybe_compute()
	   after initialization. */
	vinfo_t* r = vinfo_new(VirtualTime_New(&psyco_computed_vlist));
	r->array = array_new(VLIST_ITEMS + size);
	r->array->items[iOB_TYPE] =
		vinfo_new(CompileTime_New((long)(&PyList_Type)));
	r->array->items[iVAR_SIZE] = vinfo_new(CompileTime_NewSk
                                               (sk_new(size, SkFlagFixed)));
	return r;
}

DEFINEFN
vinfo_t* PsycoList_New(PsycoObject* po, int size, vinfo_t** source)
{
	vinfo_t* v;
	int i;
	vinfo_t* r = PsycoList_NEW(size);
	extra_assert(source != NULL);
	for (i=0; i<size; i++) {
		v = source[i];
		vinfo_incref(v);
		r->array->items[VLIST_ITEMS + i] = v;
	}
	return list_maybe_compute(po, r);
}

DEFINEFN
vinfo_t* PsycoList_SingletonNew(vinfo_t* vitem)
{
	vinfo_t* r = PsycoList_NEW(1);
	vinfo_incref(vitem);
	r->array->items[VLIST_ITEMS + 0] = vitem;
	return r;
}

DEFINEFN
int PsycoList_Load(vinfo_t* list)
{
	/* only for virtual lists */
	if (list->source == VirtualTime_New(&psyco_computed_vlist))
		return vlist_length(list);
	else
		return -1;
}


 /***************************************************************/
  /*** list objects meta-implementation                        ***/

DEFINEFN
vinfo_t* plist_item(PsycoObject* po, vinfo_t* a, vinfo_t* i)
{
	condition_code_t cc;
	vinfo_t* vlen;
	vinfo_t* ob_item;
	vinfo_t* result;

	vlen = psyco_get_field(po, a, VAR_size);
	if (vlen == NULL)
		return NULL;
	
	cc = integer_cmp(po, i, vlen, Py_GE|COMPARE_UNSIGNED);
	vinfo_decref(vlen, po);
	if (cc == CC_ERROR)
		return NULL;

	if (runtime_condition_f(po, cc)) {
		PycException_SetString(po, PyExc_IndexError,
				       LE_MSG_LIST_INDEX_RANGE);
		return NULL;
	}
        assert_nonneg(i);

	if (a->source == VirtualTime_New(&psyco_computed_vlist) &&
	    is_compiletime(i->source)) {
		/* optimize virtual lists */
		vlist_length(a);  /* for the assert()s */
		result = vinfo_getitem(a, VLIST_ITEMS +
				       CompileTime_Get(i->source)->value);
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

static bool plist_ass_item(PsycoObject* po, vinfo_t* a, vinfo_t* i, vinfo_t* v)
{
	condition_code_t cc;
	vinfo_t* vlen;
	vinfo_t* ob_item;
	vinfo_t* old_value;
	bool ok;

	if (v == NULL) {
		/* XXX implement item deletion */
		return psyco_generic_call(po, PyList_Type.tp_as_sequence->
					  sq_ass_item,
					  CfCommonIntZeroOk,
					  "vvl", a, i, (long) NULL) != NULL;
	}

	vlen = psyco_get_field(po, a, VAR_size);
	if (vlen == NULL)
		return false;
	
	cc = integer_cmp(po, i, vlen, Py_GE|COMPARE_UNSIGNED);
        vinfo_decref(vlen, po);
	if (cc == CC_ERROR)
		return false;

	if (runtime_condition_f(po, cc)) {
		PycException_SetString(po, PyExc_IndexError,
				       LE_MSG_LIST_ASSIGN_INDEX_RANGE);
		return false;
	}
        assert_nonneg(i);

	if (a->source == VirtualTime_New(&psyco_computed_vlist) &&
	    is_compiletime(i->source) &&
            !vinfo_would_be_recursive(a, v)) {
		/* optimize virtual lists */
		vlist_length(a);  /* for the assert()s */
		vinfo_incref(v);
		vinfo_setitem(po, a, VLIST_ITEMS +
			      CompileTime_Get(i->source)->value, v);
		return true;
	}

	ob_item = psyco_get_field(po, a, LIST_ob_item);
	if (ob_item == NULL)
		return false;

	old_value = psyco_get_field_array(po, ob_item, LIST_itemsarray, i);
	ok = (old_value != NULL) &&
	      psyco_put_field_array(po, ob_item, FPYREF(LIST_itemsarray), i, v);

	vinfo_decref(ob_item, po);
	if (ok) psyco_decref_v(po, old_value);
	vinfo_xdecref(old_value, po);

	return ok;
}

DEFINEFN
vinfo_t* psyco_plist_concat(PsycoObject* po, vinfo_t* a, vinfo_t* b)
{
	PyTypeObject* btp = Psyco_NeedType(po, b);
	if (btp == NULL)
		return NULL;

	if (PyType_TypeCheck(btp, &PyList_Type)) {
		vinfo_t* r;
		int alen, blen;

		blen = PsycoList_Load(b);
		if (blen >= 0) {
			alen = PsycoList_Load(a);
			if (alen >= 0) {
				/* known source list lengths:
				   build a virtual list */
				vinfo_t* buffer[VLIST_LENGTH_MAX*2];
                                extra_assert(alen <= VLIST_LENGTH_MAX);
                                extra_assert(blen <= VLIST_LENGTH_MAX);
				memcpy(buffer,
				       a->array->items + VLIST_ITEMS,
				       alen * sizeof(vinfo_t*));
				memcpy(buffer + alen,
				       b->array->items + VLIST_ITEMS,
				       blen * sizeof(vinfo_t*));
				return PsycoList_New(po, alen+blen, buffer);
			}
			else if (blen == 1) {
				/* XXX - fallback to an append ? */
			}
		}
		/* fallback - but we still know that the result is a list */
		r = psyco_generic_call(po,
				       PyList_Type.tp_as_sequence->sq_concat,
				       CfCommonNewRefPyObject,
				       "vv", a, b);
		if (r != NULL)
			Psyco_AssertType(po, r, &PyList_Type);
		return r;
	}

	/* fallback */
	return psyco_generic_call(po, PyList_Type.tp_as_sequence->sq_concat,
				  CfCommonNewRefPyObject,
				  "vv", a, b);
}

DEFINEFN
bool PsycoList_Append(PsycoObject* po, vinfo_t* v, vinfo_t* vitem)
{
	/* use the real PyList_Append */
	return psyco_generic_call(po, PyList_Append,
				  CfCommonIntZeroOk,
				  "vv", v, vitem) != NULL;
}


/* list_subscript's slice case with step 1, after PySlice_Unpack */
static PyObject* cimpl_list_slice(PyObject* self, Py_ssize_t start,
				  Py_ssize_t stop)
{
	if (PySlice_AdjustIndices(Py_SIZE(self), &start, &stop, 1) <= 0)
		return PyList_New(0);
	return PyList_GetSlice(self, start, stop);
}

/* list_ass_subscript's slice case with step 1: list_ass_slice, which
   PyList_SetSlice is (value NULL deletes) */
static int cimpl_list_ass_slice(PyObject* self, Py_ssize_t start,
				Py_ssize_t stop, PyObject* value)
{
	PySlice_AdjustIndices(Py_SIZE(self), &start, &stop, 1);
	return PyList_SetSlice(self, start, stop, value);
}

/* list_subscript is also list.__getitem__ (METH_COEXIST), so
   super().__getitem__(i) brings it a subclass instance whose own slots run
   Python code; the generic meta would dispatch on those */
static vinfo_t* plist_subscript(PsycoObject* po, vinfo_t* o, vinfo_t* key)
{
	/* a subclass's slots behind guards, as the generic meta
	   reads them */
	void* slots[3];
	void* list_slots[3];
	int i;
	PyTypeObject* tp = Psyco_NeedType(po, o);
	if (tp == NULL)
		return NULL;
	slots[0] = &tp->tp_as_mapping->mp_subscript;
	slots[1] = &tp->tp_as_sequence->sq_item;
	slots[2] = &tp->tp_as_sequence->sq_length;
	list_slots[0] = PyList_Type.tp_as_mapping->mp_subscript;
	list_slots[1] = PyList_Type.tp_as_sequence->sq_item;
	list_slots[2] = PyList_Type.tp_as_sequence->sq_length;
	for (i = 0; i < 3; i++) {
		void* f;
		switch (Psyco_TypeSlot(po, tp, slots[i], &f)) {
		case 1:
			if (f == list_slots[i])
				break;
			/* fall through */
		case 0:
			return psyco_generic_call(po,
					PyList_Type.tp_as_mapping->mp_subscript,
					CfCommonNewRefPyObject, "vv", o, key);
		default:
			return NULL;
		}
	}
	if (Psyco_KnownType(key) == &PySlice_Type) {
		vinfo_t *i, *j, *r;
		switch (PsycoSlice_Unpack(po, key, &i, &j)) {
		case -1:
			return NULL;
		case 1:
			r = psyco_generic_call(po, cimpl_list_slice,
					       CfCommonNewRefPyObject,
					       "vvv", o, i, j);
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
static bool plist_ass_subscript(PsycoObject* po, vinfo_t* o, vinfo_t* key,
				vinfo_t* value)
{
	PyTypeObject* ktp = Psyco_NeedType(po, key);
	if (ktp == NULL)
		return false;
	if (ktp == &PySlice_Type) {
		vinfo_t *i, *j, *r;
		switch (PsycoSlice_Unpack(po, key, &i, &j)) {
		case -1:
			return false;
		case 1:
			if (value != NULL)
				r = psyco_generic_call(po, cimpl_list_ass_slice,
						       CfCommonIntZeroOk,
						       "vvvv", o, i, j, value);
			else
				r = psyco_generic_call(po, cimpl_list_ass_slice,
						       CfCommonIntZeroOk,
						       "vvvl", o, i, j, 0L);
			vinfo_decref(j, po);
			vinfo_decref(i, po);
			return r != NULL;
		}
	}
	return psyco_generic_ass_subscript(po, o, key, value);
}


/* list_iter; a str.split() list held only by the caller is iterated
   without being made */
static vinfo_t* plist_iter(PsycoObject* po, vinfo_t* v)
{
	if (PsycoUnicode_IsSplitList(v))
		return PsycoUnicode_SplitIter(po, v);
	return PsycoSeqIter_New(po, v);
}

/* the list slots translated from listobject.c (sq_length) */
static vinfo_t *
list_length(PsycoObject *po, vinfo_t *);

static vinfo_t *
list_length(PsycoObject *po, vinfo_t *a)
{
    vinfo_t *_t1;
    _t1 = psyco_get_field(po, a, VAR_size);
    if (_t1 == NULL) {
        return NULL;
    }
    return _t1;
}

static void
gen_listobject_init(void)
{
    Psyco_DefineMeta(PyList_Type.tp_as_sequence->sq_length, list_length);
}

INITIALIZATIONFN
void psy_listobject_init(void)
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
		Psyco_DefineMeta(mm->mp_ass_subscript,
				 plist_ass_subscript);
	}

	/* CPython gives lists their own iterator type for speed; a
	   virtual sequence iterator costs compiled code nothing, so
	   iter(l) is one here, never a listiterator */
	if (PyList_Type.tp_iter != NULL)
		Psyco_DefineMeta(PyList_Type.tp_iter, &plist_iter);

        /* list object are mutable;
           they must be forced out of virtual-time across function calls */
        INIT_SVIRTUAL_NOCALL(psyco_computed_vlist, compute_vlist, NW_VLISTS);

	/*psyco_empty_list = PsycoList_NEW(0);*/
}
