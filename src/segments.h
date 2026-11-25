#ifndef LE_SEGMENTS_H
#define LE_SEGMENTS_H
#include "psyco.h"

#if LE_STACK_SEGMENTS
/* the offset from the thread pointer (%fs:0) of this thread's stack low
   mark: compiled code below it makes its next Python call through CPython
   on a segment (le_call_on_segment) */
EXTERNVAR long le_stack_low_at;
/* sets this thread's low mark, if not yet set */
EXTERNFN void
le_stack_init(void);
/* fn(a, b, c, d), a call through CPython that stands for one compiled code
   would make with no C stack in CPython, run on a stack segment: what fn
   returns */
EXTERNFN long
le_call_on_segment(void *fn, long a, long b, long c, long d);
#endif

#endif /* LE_SEGMENTS_H */
