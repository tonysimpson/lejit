#ifndef LE_ENCSTATE_H
#define LE_ENCSTATE_H

#include "types.h"
#include "value.h"
#include "execbuff.h"

typedef struct {
    uint64_t stackdepth;
    LeValue *regs[16];
    LeCodeObject *code;
    mm_mem_t *loc;
} EncoderState;

#endif /* LE_ENCSTATE_H */
