/* Frames as Python sees them are PyFrameObjects: the thread's
   innermost is tstate->frame, each links to its caller by f_back, and a
   running frame's arguments and locals are its f_localsplus. ni_frame_t is
   the frame entry hands to eval_hook.c:NiCode_Run and frames.c walks; here
   it is the frame object itself. */
typedef PyFrameObject ni_frame_t;

static inline ni_frame_t *ni_frame_current(PyThreadState *ts) {
  return ts->frame;
}
static inline void ni_frame_set_current(PyThreadState *ts, ni_frame_t *f) {
  ts->frame = f;
}
static inline ni_frame_t *ni_frame_back(ni_frame_t *f) { return f->f_back; }
/* the frame object Python sees (borrowed) */
static inline PyFrameObject *ni_frame_object(ni_frame_t *f) { return f; }
static inline PyCodeObject *ni_frame_code(ni_frame_t *f) { return f->f_code; }
static inline PyObject *ni_frame_globals(ni_frame_t *f) {
  return f->f_globals;
}
/* the argument words compiled code runs on, owned by the frame */
static inline PyObject **ni_frame_args(ni_frame_t *f) {
  return f->f_localsplus;
}
/* while set, frame.clear() refuses the frame */
static inline void ni_frame_set_executing(ni_frame_t *f, int on) {
  f->f_executing = on;
}
