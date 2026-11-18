#include "pdictobject.h"


/* ma_used is read as its low 32 bits (policy_table.LAYOUTS) */
#define DICT_ma_used   FMUT(CPY_FIELD(PyDictObject, ma_used, OB_type))
#define iDICT_MA_USED  FIELD_INDEX(DICT_ma_used)

DEFINEFN
vinfo_t* PsycoDict_New(PsycoObject* po)
{
	/* XXX no virtual dicts yet */
	vinfo_t* v = psyco_generic_call(po, PyDict_New,
					CfCommonNewRefPyObject, "");
	if (v == NULL)
		return NULL;

	/* the result is a dict */
	Psyco_AssertType(po, v, &PyDict_Type);
	return v;
}

#if HAVE_DICT_NEWPRESIZED
DEFINEFN
vinfo_t* _PsycoDict_NewPresized(PsycoObject* po, long minused)
{
	if (minused > 5) {
		vinfo_t* v = psyco_generic_call(po, _PyDict_NewPresized,
						CfCommonNewRefPyObject,
						"l", minused);
		if (v == NULL)
			return NULL;
		/* the result is a dict */
		Psyco_AssertType(po, v, &PyDict_Type);
		return v;
	}
	else
		return PsycoDict_New(po);
}
#endif

DEFINEFN
vinfo_t* PsycoDict_Copy(PsycoObject* po, vinfo_t* orig)
{
	vinfo_t* v = psyco_generic_call(po, PyDict_Copy,
					CfCommonNewRefPyObject,
					"v", orig);
	if (v == NULL)
		return NULL;

	/* the result is a dict */
	Psyco_AssertType(po, v, &PyDict_Type);
	return v;
}

DEFINEFN
bool PsycoDict_SetItem(PsycoObject* po, vinfo_t* vdict,
		       vinfo_t* key, vinfo_t* vvalue)
{
	return psyco_generic_call(po, PyDict_SetItem,
				  CfCommonIntZeroOk,
				  "vvv", vdict, key, vvalue) != NULL;
}

DEFINEFN
bool PsycoDict_SetItemC(PsycoObject* po, vinfo_t* vdict,
			PyObject* key, vinfo_t* vvalue)
{
	return psyco_generic_call(po, PyDict_SetItem,
				  CfCommonIntZeroOk,
				  "vlv", vdict, (long) key, vvalue) != NULL;
}

/* the dict slots generated from dictobject.c (mp_length, D84) */
#include "pdictobject_gen.h"

INITIALIZATIONFN
void psy_dictobject_init(void)
{
	gen_dictobject_init();
}
