#ifndef LE_SEGMENTS_H
#define LE_SEGMENTS_H
#include "psyco.h"

#if LE_STACK_SEGMENTS
/* the offset from the thread pointer (%fs:0) of this thread's stack low
   mark: compiled code below it makes its next Python call through CPython
   on a segment (le_call_as_direct) */
EXTERNVAR long le_stack_low_at;
/* sets this thread's low mark, if not yet set */
EXTERNFN void
le_stack_init(void);
/* fn(a, b, c, d), a call through CPython that stands for one CPython makes
   with no C stack (a Python function's, as compiled code makes it
   directly, or one it can't): what fn returns. Made on a stack segment if
   the stack is below the low mark; CPython's entry's C units are given back
   for it, and the C stack it takes counts as a direct call's where a
   segment's room is carried (frames.c:psyco_direct_calls_size). */
EXTERNFN long
le_call_as_direct(void *fn, long a, long b, long c, long d);
/* one of those calls, on le_call_as_direct's C stack: a list per thread,
   innermost first */
typedef struct le_direct_entry_s {
    struct le_direct_entry_s *next;
} le_direct_entry_t;
#endif

#endif /* LE_SEGMENTS_H */
