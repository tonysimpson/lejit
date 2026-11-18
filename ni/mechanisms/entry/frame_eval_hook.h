/* Entry by the PEP 523 frame-evaluation hook, which is given a
   PyFrameObject (D75). Every frame CPython evaluates comes here first: one
   whose code Ni has compiled (eval_hook.c:ni_entry_point) runs on the
   frame's argument words (eval_hook.c:NiCode_Run), anything else goes to
   the evaluator installed before. Included by eval_hook.c after those two. */
static _PyFrameEvalFunction original_eval_frame = NULL;

static PyObject *NiEval_EvalFrame(PyFrameObject *frame, int throwflag) {
  PyObject *entry_point = ni_entry_point(frame->f_code, frame->f_globals,
                                         frame->f_locals, frame->f_builtins);
  if (entry_point != NULL)
    return NiCode_Run(entry_point, frame);
  return original_eval_frame(frame, throwflag);
}

static int ni_entry_install(void) {
  PyThreadState *tstate = PyThreadState_Get();
  if (original_eval_frame == NULL) {
    original_eval_frame = tstate->interp->eval_frame;
    tstate->interp->eval_frame = NiEval_EvalFrame;
  }
  return 0;
}
