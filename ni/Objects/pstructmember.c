/*
 * Compile-time PyMember_GetOne: maps each T_* member type onto a field
 * descriptor and emits a load. Unreachable for now (pdescrobject.c: nothing
 * dispatches to the member-descriptor meta). Known problems on x86-64:
 *  - T_INT/T_UINT share the 'long' case, so a 4-byte field is read as 8
 *    bytes (KNOWN_BUGS M10);
 *  - T_STRING_INPLACE builds bytes where CPython 3 builds str.
 */
#include "pstructmember.h"
#include "../Python/pyver.h"
#include "../compat2to3.h"
#include "../pycodegen.h"
#include "pfloatobject.h"
#include "plongobject.h"
#include "pstringobject.h"

DEFINEFN
vinfo_t *PsycoMember_GetOne(PsycoObject *po, vinfo_t *addr, PyMemberDef *l) {
  condition_code_t cc;
  vinfo_t *v;
  vinfo_t *w;
  defield_t rdf;
  if (l->flags & READ_RESTRICTED)
    goto fallback;

  /* XXX add (some of) the missing types */
  switch (l->type) {
  case T_BYTE: /* read a byte, extend it signed */
    rdf = FMUT(DEF_ARRAY(signed char, 0));
    break;
  case T_UBYTE: /* read a byte, extend it unsigned */
    rdf = FMUT(UNSIGNED_ARRAY(unsigned char, 0));
    break;
  case T_SHORT: /* read a short, extend it signed */
    rdf = FMUT(DEF_ARRAY(signed short, 0));
    break;
  case T_USHORT: /* read a short, extend it unsigned */
    rdf = FMUT(UNSIGNED_ARRAY(unsigned short, 0));
    break;
  case T_INT:
    rdf = FMUT(DEF_ARRAY(int, 0));
    break;
  case T_UINT:
    rdf = FMUT(UNSIGNED_ARRAY(unsigned int, 0));
    break;
  case T_LONG:
    rdf = FMUT(DEF_ARRAY(long, 0));
    break;
  case T_ULONG: /* may not fit a signed long */
    goto fallback;
  case T_FLOAT: /* read a long, turn it into a float */
    rdf = FMUT(DEF_ARRAY(float, 0));
    w = psyco_get_field_offset(po, addr, rdf, l->offset);
    if (w == NULL)
      return NULL;
    v = PsycoFloat_FromFloat(po, w);
    vinfo_decref(w, po);
    return v;
  case T_DOUBLE: /* read two longs, turn them into a double */
    rdf = FMUT(DEF_ARRAY(double, 0));
    w = psyco_get_field_offset(po, addr, rdf, l->offset);
    if (w == NULL)
      return NULL;
    v = PsycoFloat_FROM_DOUBLE(w);
    return v;
  case T_STRING: /* read a char*, turn it into a string */
    rdf = FMUT(DEF_ARRAY(char *, 0));
    w = psyco_get_field_offset(po, addr, rdf, l->offset);
    if (w == NULL)
      return NULL;
    cc = integer_non_null(po, w);
    if (cc == CC_ERROR) {
      vinfo_decref(w, po);
      return NULL;
    }

    if (runtime_condition_t(po, cc)) {
      /* the char* is not NULL */
      v = psyco_generic_call(po, NiCompatStr_FromString, CfCommonNewRefPyObject,
                             "v", w);
      if (v != NULL)
        Psyco_AssertType(po, v, &NiCompatStr_Type);
    } else {
      v = psyco_vi_None();
    }
    vinfo_decref(w, po);
    return v;
  case T_STRING_INPLACE: /* read an array of characters from the struct */
    w = integer_add_i(po, addr, l->offset, false);
    if (w == NULL)
      return NULL;
    v = psyco_generic_call(po, PyBytes_FromString, CfCommonNewRefPyObject, "v",
                           w);
    if (v != NULL)
      Psyco_AssertType(po, v, &PyBytes_Type);
    vinfo_decref(w, po);
    return v;
  case T_OBJECT: /* read a long, turn it into a PyObject with reference */
    rdf = FMUT(DEF_ARRAY(PyObject *, 0));
    v = psyco_get_field_offset(po, addr, rdf, l->offset);
    if (v == NULL)
      return NULL;
    cc = integer_non_null(po, v);
    if (cc == CC_ERROR) {
      vinfo_decref(v, po);
      return NULL;
    }

    if (runtime_condition_t(po, cc)) {
      /* 'v' contains a non-NULL value */
      need_reference(po, v);
    } else {
      /* 'v' contains NULL */
      vinfo_decref(v, po);
      v = psyco_vi_None();
    }
    return v;
  case T_OBJECT_EX: /* same as T_OBJECT, exception if NULL */
    rdf = FMUT(DEF_ARRAY(PyObject *, 0));
    v = psyco_get_field_offset(po, addr, rdf, l->offset);
    if (v == NULL)
      return NULL;
    cc = integer_non_null(po, v);
    if (cc == CC_ERROR) {
      vinfo_decref(v, po);
      return NULL;
    }

    if (runtime_condition_t(po, cc)) {
      /* 'v' contains a non-NULL value */
      need_reference(po, v);
    } else {
      /* 'v' contains NULL */
      vinfo_decref(v, po);
      PycException_SetString(po, PyExc_AttributeError, l->name);
      return NULL;
    }
    return v;
  default:
    goto fallback;
  }

  /* for integer fields of any size */
  v = psyco_get_field_offset(po, addr, rdf, l->offset);
  if (v != NULL) {
    if (!T_UINT_READS_AS_SIGNED && (l->type == T_UINT || l->type == T_ULONG)) {
      vinfo_t *result = PsycoLong_FromUnsignedLong(po, v);
      vinfo_decref(v, po);
      v = result;
    } else {
      v = PsycoLong_FROM_LONG(po, v);
    }
  }
  return v;

fallback:
  /* call the Python implementation for cases we cannot handle directy */
  return psyco_generic_call(po, PyMember_GetOne, CfCommonNewRefPyObject, "vl",
                            addr, l);
}
