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

DEFINEFN vinfo_t *
PsycoMember_GetOne(PsycoObject *po, vinfo_t *addr, PyMemberDef *l)
{
    vinfo_t *_t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    vinfo_t *_t13;
    vinfo_t *_t14;
    vinfo_t *_t15;
    vinfo_t *_t16;
    vinfo_t *_t17;
    vinfo_t *_t18;
    vinfo_t *_t19;
    vinfo_t *_t2;
    vinfo_t *_t20;
    vinfo_t *_t21;
    vinfo_t *_t23;
    condition_code_t _t24;
    int _t25;
    vinfo_t *_t26;
    vinfo_t *_t27;
    vinfo_t *_t28;
    vinfo_t *_t29;
    vinfo_t *_t3;
    vinfo_t *_t30;
    vinfo_t *_t31;
    vinfo_t *_t32;
    condition_code_t _t33;
    int _t34;
    vinfo_t *_t35;
    condition_code_t _t36;
    int _t37;
    vinfo_t *_t38;
    vinfo_t *_t39;
    vinfo_t *_t4;
    vinfo_t *_t40;
    vinfo_t *_t41;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *v;
    switch (l->type) {
        case 14:
            _t1 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(char, 0)),
                                         l->offset);
            if (_t1 == NULL) {
                return NULL;
            }
            _t2 = PsycoBool_FromLong(po, _t1);
            vinfo_decref(_t1, po);
            v = _t2;
            return v;
        case 8:
            _t3 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(char, 0)),
                                         l->offset);
            if (_t3 == NULL) {
                return NULL;
            }
            _t4 = PsycoLong_FromLong(po, _t3);
            vinfo_decref(_t3, po);
            v = _t4;
            return v;
        case 9:
            _t5 = psyco_get_field_offset(
                po, addr, FMUT(UNSIGNED_ARRAY(unsigned char, 0)),
                l->offset);
            if (_t5 == NULL) {
                return NULL;
            }
            _t6 = PsycoLong_FromUnsignedLong(po, _t5);
            vinfo_decref(_t5, po);
            v = _t6;
            return v;
        case 0:
            _t7 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(short, 0)),
                                         l->offset);
            if (_t7 == NULL) {
                return NULL;
            }
            _t8 = PsycoLong_FromLong(po, _t7);
            vinfo_decref(_t7, po);
            v = _t8;
            return v;
        case 10:
            _t9 = psyco_get_field_offset(
                po, addr, FMUT(UNSIGNED_ARRAY(unsigned short, 0)),
                l->offset);
            if (_t9 == NULL) {
                return NULL;
            }
            _t10 = PsycoLong_FromUnsignedLong(po, _t9);
            vinfo_decref(_t9, po);
            v = _t10;
            return v;
        case 1:
            _t11 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(int, 0)),
                                          l->offset);
            if (_t11 == NULL) {
                return NULL;
            }
            _t12 = PsycoLong_FromLong(po, _t11);
            vinfo_decref(_t11, po);
            v = _t12;
            return v;
        case 11:
            _t13 = psyco_get_field_offset(
                po, addr, FMUT(UNSIGNED_ARRAY(unsigned int, 0)),
                l->offset);
            if (_t13 == NULL) {
                return NULL;
            }
            _t14 = PsycoLong_FromUnsignedLong(po, _t13);
            vinfo_decref(_t13, po);
            v = _t14;
            return v;
        case 2:
            _t15 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(long, 0)),
                                          l->offset);
            if (_t15 == NULL) {
                return NULL;
            }
            _t16 = PsycoLong_FromLong(po, _t15);
            vinfo_decref(_t15, po);
            v = _t16;
            return v;
        case 12:
            _t17 = psyco_get_field_offset(
                po, addr, FMUT(UNSIGNED_ARRAY(unsigned long, 0)),
                l->offset);
            if (_t17 == NULL) {
                return NULL;
            }
            _t18 = PsycoLong_FromUnsignedLong(po, _t17);
            vinfo_decref(_t17, po);
            v = _t18;
            return v;
        case 19:
            _t19 = psyco_get_field_offset(
                po, addr, FMUT(DEF_ARRAY(Py_ssize_t, 0)), l->offset);
            if (_t19 == NULL) {
                return NULL;
            }
            _t20 = PsycoLong_FromSsize_t(po, _t19);
            vinfo_decref(_t19, po);
            v = _t20;
            return v;
        case 3:
            return psyco_generic_call(po, PyMember_GetOne,
                                      CfCommonNewRefPyObject, "vl", addr,
                                      (long)(l));
        case 4:
            _t21 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(double, 0)),
                                          l->offset);
            if (_t21 == NULL) {
                return NULL;
            }
            v = PsycoFloat_FROM_DOUBLE(_t21);
            return v;
        case 5:
            _t23 = psyco_get_field_offset(po, addr, FMUT(DEF_ARRAY(char *, 0)),
                                          l->offset);
            if (_t23 == NULL) {
                return NULL;
            }
            _t24 = integer_non_null(po, _t23);
            vinfo_decref(_t23, po);
            if (_t24 == CC_ERROR) {
                return NULL;
            }
            _t24 = INVERT_CC(_t24);
            _t25 = runtime_condition_f(po, _t24);
            if (_t25) {
                v = psyco_vi_None();
            }
            else {
                _t26 = psyco_get_field_offset(
                    po, addr, FMUT(DEF_ARRAY(char *, 0)), l->offset);
                if (_t26 == NULL) {
                    return NULL;
                }
                _t27 = psyco_generic_call(po, PyUnicode_FromString,
                                          CfCommonNewRefPyObject, "v", _t26);
                vinfo_decref(_t26, po);
                v = _t27;
            }
            return v;
        case 13:
            _t28 = integer_add_i(po, addr, l->offset, false);
            _t29 = psyco_generic_call(po, PyUnicode_FromString,
                                      CfCommonNewRefPyObject, "v", _t28);
            vinfo_decref(_t28, po);
            v = _t29;
            return v;
        case 7:
            _t30 = integer_add_i(po, addr, l->offset, false);
            _t31 = psyco_generic_call(po, PyUnicode_FromStringAndSize,
                                      CfCommonNewRefPyObject, "vl", _t30,
                                      (long)(1));
            vinfo_decref(_t30, po);
            v = _t31;
            return v;
        case 6:
            _t32 = psyco_get_field_offset(
                po, addr, FMUT(DEF_ARRAY(PyObject *, 0)), l->offset);
            if (_t32 == NULL) {
                return NULL;
            }
            v = _t32;
            _t33 = integer_non_null(po, v);
            if (_t33 == CC_ERROR) {
                vinfo_decref(v, po);
                return NULL;
            }
            _t33 = INVERT_CC(_t33);
            _t34 = runtime_condition_f(po, _t33);
            if (_t34) {
                vinfo_decref(v, po);
                v = psyco_vi_None();
            }
            need_reference(po, v);
            return v;
        case 16:
            _t35 = psyco_get_field_offset(
                po, addr, FMUT(DEF_ARRAY(PyObject *, 0)), l->offset);
            if (_t35 == NULL) {
                return NULL;
            }
            v = _t35;
            _t36 = integer_non_null(po, v);
            if (_t36 == CC_ERROR) {
                vinfo_decref(v, po);
                return NULL;
            }
            _t36 = INVERT_CC(_t36);
            _t37 = runtime_condition_f(po, _t36);
            if (_t37) {
                vinfo_decref(v, po);
#if LE_HAS_MEMBER_EX_ERROR_FROM_TYPE
                return psyco_generic_call(po, PyMember_GetOne,
                                          CfCommonNewRefPyObject, "vl", addr,
                                          (long)(l));
#else
                PycException_SetString(po, PyExc_AttributeError, l->name);
                return NULL;
#endif
            }
            need_reference(po, v);
            return v;
        case 17:
            _t38 = psyco_get_field_offset(
                po, addr, FMUT(DEF_ARRAY(long long, 0)), l->offset);
            if (_t38 == NULL) {
                return NULL;
            }
            _t39 = psyco_generic_call(
                po, PyLong_FromLongLong,
                CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", _t38);
            vinfo_decref(_t38, po);
            v = _t39;
            return v;
        case 18:
            _t40 = psyco_get_field_offset(
                po, addr, FMUT(UNSIGNED_ARRAY(unsigned long long, 0)),
                l->offset);
            if (_t40 == NULL) {
                return NULL;
            }
            _t41 = psyco_generic_call(
                po, PyLong_FromUnsignedLongLong,
                CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", _t40);
            vinfo_decref(_t40, po);
            v = _t41;
            return v;
        case 20:
            return psyco_vi_None();
        default:
            PycException_SetString(po, PyExc_SystemError,
                                   "bad memberdescr type");
            return NULL;
    }
}
