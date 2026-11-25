#ifndef _GENRECORD_H
#define _GENRECORD_H
/* Virtual generators' records (genrecord.c, mechanisms/frames.h:
   le_gen_record_t), for interpreters whose frames are interpreter frames */
#include "frames.h"

#if LE_RESUMES_GENERATORS_DIRECTLY
#if ALL_CHECKS
/* records made, and escapes: of a paused or new one, of one dropped
   paused, of one running, of one ended (lejit._core._stats) */
EXTERNVAR long le_stat_gen_records[6];
#endif
/* A new record for a generator of the function 'func', paused as
   RETURN_GENERATOR leaves a new generator, which is at code unit 'start'
   of its code: no words yet. A new reference, or NULL with an exception. */
EXTERNFN PyObject *
le_gen_record_new(PyObject *func, long start);
/* its arguments are in its words: its cells and free variables are made,
   as the instructions before RETURN_GENERATOR make them. -1 with an
   exception. */
EXTERNFN int
le_gen_record_start(PyObject *rec);
/* The real generator the record 'rec' stands for, of the function 'func'
   (needed only once the record has finished): made now, in the state the
   record is in, unless it has been made already. From then on the record
   holds nothing but it. A new reference, or NULL with an exception. */
EXTERNFN PyObject *
le_gen_record_escape(PyObject *rec, PyObject *func);
/* the same, for the record whose frame 'f' is, paused */
EXTERNFN PyObject *
le_gen_record_escape_frame(le_frame_t *f);
/* A run of the record's body from compiled code (psyfunc.c:gen_send) that
   didn't pause there in the usual way: it ended, or paused with a frame
   object asked for (the record escapes then), or the record escaped while
   it ran. As eval_hook.c's le_gen_direct_end, le_gen_direct_send_end and
   le_gen_direct_send_status do for a generator. */
EXTERNFN PyObject *
le_gen_record_end(PyObject *rec, PyObject *result);
EXTERNFN PyObject *
le_gen_record_send_end(PyObject *rec, PyObject *result);
EXTERNFN int
le_gen_record_send_status(PyObject *rec, PyObject *result, PyObject *out[]);
/* the unit of RETURN_GENERATOR in the code of a generator function, -1 if
   it has none */
EXTERNFN long
le_gen_record_start_unit(PyCodeObject *co);
INITIALIZATIONFN void
psyco_genrecord_init(void);
/* whether 'o' is a record */
EXTERNFN bool
le_gen_record_check(PyObject *o);
/* the record whose frame 'f' is (a record's frame), made the real
   generator now, while its body runs, so that ceval can run the body on
   (frames.c:psyco_resume_begin): the generator, borrowed (the record holds
   it), running, its exception state linked in place of the record's; NULL
   with an exception */
EXTERNFN PyGenObject *
le_gen_record_escape_running(le_frame_t *f);
#endif
#endif /* _GENRECORD_H */
