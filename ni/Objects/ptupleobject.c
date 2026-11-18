/*
 * Virtual tuples. vinfo->array layout: [iOB_TYPE] = &PyTuple_Type and
 * [iFIX_SIZE] = length (both constants), then the items from iTUPLE_OB_ITEM.
 * compute_tuple materialises a tuple when it escapes: if every item is a
 * constant the tuple is built now and becomes a constant, otherwise
 * PyTuple_New and item stores are emitted. Tuples are immutable, so they stay
 * virtual across calls. PsycoTuple_Load is how other code gets a compile-time
 * length and the item vinfos (it returns -1 if the length is unknown, and
 * refuses constant tuples longer than CT_TUPLE_LOAD_SIZE_LIMIT).
 */
#include "ptupleobject.h"


 /***************************************************************/
  /***   Virtual tuples                                        ***/

static source_virtual_t psyco_computed_tuple;

/* PsycoTuple_New, compute_tuple and direct_compute_tuple, generated from
   PyTuple_New and policy_table.TYPES (D80) */
#include "ptupleobject_virtual_gen.h"


/* do not load a constant tuple into a vinfo_array_t if longer than: */
#define CT_TUPLE_LOAD_SIZE_LIMIT    15

DEFINEFN
int PsycoTuple_Load(vinfo_t* tuple)
{
	int size;
	/* if the tuple is virtual, then all items in its
	   vinfo_array_t are already filled */
	if (tuple->source == VirtualTime_New(&psyco_computed_tuple))
		size = tuple->array->count - iTUPLE_OB_ITEM;
	else if (is_compiletime(tuple->source)) {
		/* a constant tuple means constant tuple items */
		int i;
		PyObject* o = (PyObject*)(CompileTime_Get(tuple->source)->value);
		/* BUILD_*_UNPACK asks about any constant, such as a global
		   list (F45) */
		if (!PyTuple_Check(o))
			return -1;
		size = PyTuple_GET_SIZE(o);
		if (tuple->array->count < iTUPLE_OB_ITEM + size) {
			if (/*not_too_much &&*/ size > CT_TUPLE_LOAD_SIZE_LIMIT)
				return -1;
			vinfo_array_grow(tuple, iTUPLE_OB_ITEM + size);
		}
		/* load the tuple into the vinfo_array_t */
		for (i=0; i<size; i++) {
			if (PsycoTuple_GET_ITEM(tuple, i) == NULL) {
				PyObject* item = PyTuple_GET_ITEM(o, i);
				source_known_t* sk = sk_new((long) item,
							    SkFlagPyObj);
				Py_INCREF(item);
				PsycoTuple_GET_ITEM(tuple, i) =
					vinfo_new(CompileTime_NewSk(sk));
			}
		}
	}
	else
		return -1;
	return size;
}

/* the slots generated from tupleobject.c (D78, D80, D84): sq_length,
   sq_item, sq_contains, sq_concat, sq_repeat, tp_richcompare */
#include "ptupleobject_gen.h"


INITIALIZATIONFN
void psy_tupleobject_init(void)
{
	PyMappingMethods *mm;
	gen_tupleobject_init();

	mm = PyTuple_Type.tp_as_mapping;
	if (mm) {  /* Python >= 2.3 */
		Psyco_DefineMeta(mm->mp_subscript, psyco_generic_subscript);
	}

	gen_virtual_tuple_init();
}
