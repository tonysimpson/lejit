#include "pboolobject.h"

DEFINEFN
vinfo_t *
PsycoBool_FromLong(PsycoObject *po, vinfo_t *v)
{
    condition_code_t cc;
    if (is_compiletime(v->source))
        return CompileTime_Get(v->source)->value ? psyco_vi_Py_True()
                                                 : psyco_vi_Py_False();
    cc = integer_non_null(po, v);
    if (cc == CC_ERROR)
        return NULL;
    return PsycoBool_FromCondition(po, cc);
}

DEFINEFN
vinfo_t *
PsycoBool_FromZeroOrOne(PsycoObject *po, vinfo_t *v)
{
    if (is_compiletime(v->source))
        return PsycoBool_FromLong(po, v);
    vinfo_incref(v);
    return PsycoBool_FROM_WORD(v);
}
