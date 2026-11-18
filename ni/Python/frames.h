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
   a compiled frame's (code, globals, tag) (f_lasti -1). New reference. */
EXTERNFN PyFrameObject *psyco_emulate_frame(PyObject *o);
/* the real frame, if the compiled code 'co' making the current C call is
   the one NiCode_Run runs (not a compiled callee); borrowed, or NULL */
EXTERNFN PyFrameObject *psyco_running_real_frame(PyCodeObject *co);

EXTERNFN PyObject *psyco_get_globals(void);
/* PyObject* psyco_get_locals(void); this one implemented in psyco.c */

/* to keep a trace of the Psyco stack frames */
struct stack_frame_info_s {
  long link_stack_depth; /* -1 if there is an inline frame following */
  PyCodeObject *co;
  PyObject *globals; /* NULL if not compile-time */
};

PSY_INLINE stack_frame_info_t *finfo_last(stack_frame_info_t *finfo) {
  if (finfo->link_stack_depth < 0) {
    finfo -= finfo->link_stack_depth;
  }
  return finfo;
}

EXTERNFN stack_frame_info_t *psyco_finfo(PsycoObject *caller,
                                         PsycoObject *callee);

/* Psyco runtime frame date associated with interpreter frame */
typedef struct {
  PyFrameObject *py_frame;
  stack_frame_info_t ***psy_frames_start;
  PyCodeObject *psy_code;
  PyObject *psy_globals;
} FrameRuntime;

EXTERNFN int psyco_frame_start_register(PyFrameObject *f,
                                        stack_frame_info_t ***psy_frames_start);
EXTERNFN int psyco_frame_start_unregister(PyFrameObject *f);
EXTERNFN FrameRuntime *psyco_frame_start_get(PyFrameObject *f);

#endif /* _FRAMES_H */
