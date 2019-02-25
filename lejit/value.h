#ifndef LE_VALUE_H
#define LE_VALUE_H

struct le_value_s;

typedef struct {
    void *ptr;
} ct_fields;

typedef struct {
    le_value_s **values;
} vt_fields;

typedef struct {
} rt_fields;

typedef union {
    ct_fields ct;
    vt_fields vt;
    rt_fields rt;
} time_fields;

typedef struct le_value_s {
    PyTypeObject *known_type;
    int refcount;
    short stack;
    char reg;
    char flags;
    time_fields tf;
} LeValue;

#define LE_VALUE_RUN_TIME 0
#define LE_VALUE_COMPILE_TIME 1
#define LE_VALUE_VIRTUAL_TIME 2
#define LE_VALUE_TIME_MASK 3
#define LE_VALUE_IS_TIME(v, time) (((LeValue*)(v))->flags & LE_VALUE_TIME_MASK == (time))
#define LE_VALUE_IS_CT(v) (LE_VALUE_IS_TIME(v, LE_VALUE_TIME_COMPILE_TIME))
#define LE_VALUE_IS_RT(v) (LE_VALUE_IS_TIME(v, LE_VALUE_COMPILE_TIME))
#define LE_VALUE_IS_VT(v) (LE_VALUE_IS_TIME(v, LE_VALUE_COMPILE_TIME))

#endif /* LE_VALUE_H */
