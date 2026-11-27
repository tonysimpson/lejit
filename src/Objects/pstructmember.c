/*
 * Compile-time PyMember_GetOne, translated from Python/structmember.c:
 * each T_* member type is a read of the
 * object's bytes at the member's offset (FMUT: read again each time) and
 * CPython's conversion. A T_OBJECT member read holds no reference until
 * need_reference; a NULL one is None, or AttributeError for T_OBJECT_EX,
 * on a respawned branch. T_FLOAT is a C call (no float to double here).
 * Reached through the member-descriptor meta (pdescrobject.c) when
 * pobject.c:generic_getattr specialises on the type.
 */
#include "pstructmember.h"
#include "../pycodegen.h"
#include "pboolobject.h"
#include "pfloatobject.h"
#include "plongobject.h"

_Static_assert((char)-1 < 0, "a char member reads signed");

/* the member, read at its offset as a 'field' (an FMUT array of one) */
#define READ_MEMBER(field) psyco_get_field_offset(po, addr, field, l->offset)

/* an integer member, converted by 'psycofn' */
#define INT_MEMBER(T, field, psycofn) \
    case T:                           \
        v = READ_MEMBER(field);       \
        if (v == NULL)                \
            return NULL;              \
        res = psycofn(po, v);         \
        vinfo_decref(v, po);          \
        return res

/* the same, converted by a C call to 'fn' */
#define CALL_MEMBER(T, field, fn)                                             \
    case T:                                                                   \
        v = READ_MEMBER(field);                                               \
        if (v == NULL)                                                        \
            return NULL;                                                      \
        res = psyco_generic_call(                                             \
            po, fn, CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", v); \
        vinfo_decref(v, po);                                                  \
        return res

DEFINEFN vinfo_t *
PsycoMember_GetOne(PsycoObject *po, vinfo_t *addr, PyMemberDef *l)
{
    vinfo_t *v, *res;
    condition_code_t cc;
#ifdef _Py_AFTER_ITEMS
    /* placed after the items: its offset moves with the size */
    if (l->flags & _Py_AFTER_ITEMS)
        return psyco_generic_call(po, PyMember_GetOne, CfCommonNewRefPyObject,
                                  "vl", addr, (long)l);
#endif
    switch (l->type) {
        INT_MEMBER(T_BOOL, FMUT(DEF_ARRAY(char, 0)), PsycoBool_FromLong);
        INT_MEMBER(T_BYTE, FMUT(DEF_ARRAY(char, 0)), PsycoLong_FromLong);
        INT_MEMBER(T_UBYTE, FMUT(UNSIGNED_ARRAY(unsigned char, 0)),
                   PsycoLong_FromUnsignedLong);
        INT_MEMBER(T_SHORT, FMUT(DEF_ARRAY(short, 0)), PsycoLong_FromLong);
        INT_MEMBER(T_USHORT, FMUT(UNSIGNED_ARRAY(unsigned short, 0)),
                   PsycoLong_FromUnsignedLong);
        INT_MEMBER(T_INT, FMUT(DEF_ARRAY(int, 0)), PsycoLong_FromLong);
        INT_MEMBER(T_UINT, FMUT(UNSIGNED_ARRAY(unsigned int, 0)),
                   PsycoLong_FromUnsignedLong);
        INT_MEMBER(T_LONG, FMUT(DEF_ARRAY(long, 0)), PsycoLong_FromLong);
        INT_MEMBER(T_ULONG, FMUT(UNSIGNED_ARRAY(unsigned long, 0)),
                   PsycoLong_FromUnsignedLong);
        INT_MEMBER(T_PYSSIZET, FMUT(DEF_ARRAY(Py_ssize_t, 0)),
                   PsycoLong_FromSsize_t);
        CALL_MEMBER(T_LONGLONG, FMUT(DEF_ARRAY(long long, 0)),
                    PyLong_FromLongLong);
        CALL_MEMBER(T_ULONGLONG, FMUT(UNSIGNED_ARRAY(unsigned long long, 0)),
                    PyLong_FromUnsignedLongLong);
        case T_FLOAT:
            return psyco_generic_call(po, PyMember_GetOne,
                                      CfCommonNewRefPyObject, "vl", addr,
                                      (long)l);
        case T_DOUBLE:
            v = READ_MEMBER(FMUT(DEF_ARRAY(double, 0)));
            if (v == NULL)
                return NULL;
            return PsycoFloat_FROM_DOUBLE(v);
        case T_STRING:
            v = READ_MEMBER(FMUT(DEF_ARRAY(char *, 0)));
            if (v == NULL)
                return NULL;
            cc = integer_non_null(po, v);
            vinfo_decref(v, po);
            if (cc == CC_ERROR)
                return NULL;
            if (runtime_condition_f(po, INVERT_CC(cc)))
                return psyco_vi_None();
            v = READ_MEMBER(FMUT(DEF_ARRAY(char *, 0)));
            if (v == NULL)
                return NULL;
            res = psyco_generic_call(po, PyUnicode_FromString,
                                     CfCommonNewRefPyObject, "v", v);
            vinfo_decref(v, po);
            return res;
        case T_STRING_INPLACE:
            v = integer_add_i(po, addr, l->offset, false);
            res = psyco_generic_call(po, PyUnicode_FromString,
                                     CfCommonNewRefPyObject, "v", v);
            vinfo_decref(v, po);
            return res;
        case T_CHAR:
            v = integer_add_i(po, addr, l->offset, false);
            res = psyco_generic_call(po, PyUnicode_FromStringAndSize,
                                     CfCommonNewRefPyObject, "vl", v, 1L);
            vinfo_decref(v, po);
            return res;
        case T_OBJECT:
        case T_OBJECT_EX:
            v = READ_MEMBER(FMUT(DEF_ARRAY(PyObject *, 0)));
            if (v == NULL)
                return NULL;
            cc = integer_non_null(po, v);
            if (cc == CC_ERROR) {
                vinfo_decref(v, po);
                return NULL;
            }
            if (runtime_condition_f(po, INVERT_CC(cc))) {
                vinfo_decref(v, po);
                if (l->type == T_OBJECT)
                    return psyco_vi_None();
#if LE_HAS_MEMBER_EX_ERROR_FROM_TYPE
                return psyco_generic_call(po, PyMember_GetOne,
                                          CfCommonNewRefPyObject, "vl", addr,
                                          (long)l);
#else
                PycException_SetString(po, PyExc_AttributeError, l->name);
                return NULL;
#endif
            }
            need_reference(po, v);
            return v;
        case T_NONE:
            return psyco_vi_None();
        default:
            PycException_SetString(po, PyExc_SystemError,
                                   "bad memberdescr type");
            return NULL;
    }
}
