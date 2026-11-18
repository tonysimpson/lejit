/*
 * Compile-time PyMember_GetOne, generated from Python/structmember.c
 * (pstructmember_gen.h, D89): each T_* member type is a read of the
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

#include "pstructmember_gen.h"
