/***************************************************************/
/***            Python frames and virtual frames               ***/
/***************************************************************/

#ifndef _FRAMES_H
#define _FRAMES_H

#include "../psyco.h"
#include "pyver.h"
#include <compile.h>
#include <frameobject.h>
#include <idispatcher.h>

/* A frame object for one item pvisitframes() visits: a real frame, the
   real frame of a (code, globals, frame) tuple, or a new emulated frame for
   a compiled frame's (code, globals, tag, lasti). New reference. */
EXTERNFN PyFrameObject *psyco_emulate_frame(PyObject *o);
/* the real frame, if the compiled code 'co' making the current C call is
   the one LeCode_Run runs (not a compiled callee); borrowed, or NULL */
EXTERNFN PyFrameObject *psyco_running_real_frame(PyCodeObject *co);
/* ceval evaluating a frame itself, past the hook (eval_hook.c) */
EXTERNFN PyObject *le_eval_frame_default(PyFrameObject *f);

EXTERNFN PyObject *psyco_get_globals(void);

/* called by psyco_return_barrier as a compiled callee whose frame object
   was kept returns, 'sp' one word below its slot: lets the frame go and
   gives the call's real return address */
EXTERNFN code_t *psyco_activation_returned(long *sp);
/* the frame object kept for the innermost compiled frame, if it is a
   compiled callee of code 'co' (borrowed), else NULL */
EXTERNFN PyFrameObject *psyco_innermost_activation(PyCodeObject *co);

/* A compiled frame going on interpreted: psyco_resume_begin, then each of
   its f_localsplus and value stack items in order (psyco_resume_value) and
   its blocks (psyco_resume_block); psyco_resume_run runs it in ceval from
   the instruction after 'lasti' and returns its result. Called from
   compiled code (pycompiler.c:resume_if_tracing). */
struct resume_s;
EXTERNFN struct resume_s *psyco_resume_begin(PyCodeObject *co,
                                             PyObject *globals,
                                             PyObject *builtins, long nvalues,
                                             PyObject **word);
EXTERNFN void psyco_resume_value(struct resume_s *st, long i, PyObject *v);
EXTERNFN void psyco_resume_block(struct resume_s *st, long type,
                                 long handler, long level);
/* A compiled frame with handlers has a catcher word, which holds a
   reference to its traceback frame from when it catches an exception
   until its call ends (pycompiler.c:catcher_word). psyco_frame_catch puts
   'f' in it as the frame catches, the innermost compiled frame, whose own
   slot is 'own'; _drop lets it go if nothing else holds it, _shared does
   too and says whether something does, and _end lets it go once given its
   locals, as the call ends. */
EXTERNFN void psyco_frame_catch(PyFrameObject *f, PyObject **word,
                                stack_frame_info_t **own);
EXTERNFN void psyco_frame_catcher_drop(PyObject **word);
EXTERNFN long psyco_frame_catcher_shared(PyObject **word);
EXTERNFN void psyco_frame_catcher_end(PyObject **word);
EXTERNFN void psyco_frame_link_callee(PyFrameObject *callee,
                                      PyFrameObject *f);
EXTERNFN PyObject *psyco_resume_run(struct resume_s *st, long lasti,
                                    long nstack);
/* PyObject* psyco_get_locals(void); this one implemented in psyco.c */

/* Where a compiled frame's locals (co_nlocals, then cells and frees)
   are during a call it makes that may run Python: in its slot, as a note
   ((index in psyco_locals_maps << 1) | 1) for a C call, or as the
   callee's record's back_locals for a compiled one. A note stays after
   its call (no store to clear it): its return address tells. Each value is three longs, (LM_STACK,
   offset from data, 0), (LM_REG, register, 0) for a callee-saved register
   at a C call, (LM_CONST, value, 0), (LM_NONE, 0, 0) or (LM_VIRTUAL,
   source_virtual_t *, n) followed by its n items; data is the slot's
   address minus 'below' (frames.c:slot_locals). */
enum { LM_NONE, LM_STACK, LM_REG, LM_CONST, LM_VIRTUAL };
struct locals_map_s {
  code_t *retaddr; /* a C call's return address, to tell a stale note */
  int lasti;       /* the call's instruction */
  int nvalues;
  long below;
  long items[1];
};

/* what the glue from C into compiled code pushes to align the stack */
#define LE_GLUE_PAD 0x4e49474cL

/* to keep a trace of the Psyco stack frames */
struct stack_frame_info_s {
  long link_stack_depth; /* -1 if there is an inline frame following */
  PyCodeObject *co;
  PyObject *globals; /* NULL if not compile-time */
  int back_lasti;    /* the instruction of the next frame out that called
                        this one, or -1 */
  int nargs;         /* the words a compiled call pushes between this
                        frame's own slot and its return address, or -1
                        for an inlined frame (frames.c:kept_frame) */
  struct locals_map_s *back_locals; /* that frame's locals then, or NULL */
  code_t *ret_addr; /* a compiled call's return address */
};

/* the map of po's locals now, with lasti, or NULL while inlining. A
   local only in a register is found there (LM_REG), or with reg_pos at
   the position its register is pushed at for this call
   (psyco_call_psyco). Nothing is emitted. */
EXTERNFN struct locals_map_s *psyco_locals_map(PsycoObject *po, int lasti,
                                               const long *reg_pos);
EXTERNVAR struct locals_map_s **psyco_locals_maps;
EXTERNFN bool psyco_locals_map_reads(vinfo_t *vi, int level);
PSY_INLINE struct locals_map_s *psyco_note_map(long note) {
  return psyco_locals_maps[note >> 1];
}

PSY_INLINE stack_frame_info_t *finfo_last(stack_frame_info_t *finfo) {
  if (finfo->link_stack_depth < 0) {
    finfo -= finfo->link_stack_depth;
  }
  return finfo;
}

EXTERNFN stack_frame_info_t *psyco_finfo(PsycoObject *caller,
                                         PsycoObject *callee);

/* A real frame LeCode_Run runs: on its C stack, linked into this
   thread's list from start to return (frames.c:frame_runs) */
typedef struct frame_runtime_s {
  PyFrameObject *py_frame; /* borrowed: LeCode_Run's caller holds it */
  stack_frame_info_t ***psy_frames_start;
  PyCodeObject *psy_code;
  PyObject *psy_globals;
  PyObject **entry_locals; /* see psyco_frame_write_locals, or NULL */
  int n_entry_locals;
  /* ceval now runs the frame (psyco_resume_run): its locals and line are
     ceval's, and the compiled code under it waits for ceval's result */
  int resumed;
  struct frame_runtime_s *next; /* the next outer one */
} FrameRuntime;

EXTERNFN void psyco_frame_start_register(FrameRuntime *fr, PyFrameObject *f,
                                         stack_frame_info_t ***psy_frames_start);
EXTERNFN void psyco_frame_start_unregister(FrameRuntime *fr);
/* f's record if LeCode_Run runs it on this thread, else NULL */
EXTERNFN FrameRuntime *psyco_frame_start_get(PyFrameObject *f);

/* f_localsplus[start...] = values (up to count, within the frame's
   locals), each with a new reference: the locals of a compiled frame, put
   into the frame of its traceback entry. NULL clears a slot (an unbound
   local); LE_LOCAL_UNKNOWN leaves it. */
#define LE_LOCAL_UNKNOWN ((PyObject *)1)
EXTERNFN void psyco_frame_write_locals(PyFrameObject *f, int start,
                                       PyObject **values, int count);

#endif /* _FRAMES_H */
