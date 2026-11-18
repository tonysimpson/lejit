/*
 * Makes compiled frames visible to Python. stack_frame_info_t records (code,
 * globals) for each compiled call, and the machine stack is walked through the
 * finfo slot that each Psyco frame keeps (x64/iprocessor.c). sys._getframe
 * is replaced by ni_getframe, which counts compiled frames and emulates
 * them (no locals, first line only). Not yet done: upstream's hook of the
 * frame getter behind PyEval_GetFrame, and C code that walks
 * tstate->frame itself (warnings) still skips compiled frames. Real
 * frames running compiled code are never updated (f_lasti/f_lineno).
 */
#include "frames.h"
#include "../Objects/pobject.h"
#include "../codemanager.h"
#include "../vcompiler.h"
#include "pyver.h"
/* ni_frame_t and its accessors (D75) */
#include "mechanism_frames_gen.h"
/* ni_recursion_suspend and ni_recursion_resume (D77) */
#include "../codegen.h"
#include "../dispatcher.h"
#include "mechanism_recursion_gen.h"

/***************************************************************/
#define FRAME_STACK_ALLOC_BY 83 /* about 1KB */

DEFINEFN
stack_frame_info_t *psyco_finfo(PsycoObject *caller, PsycoObject *callee) {
  static stack_frame_info_t *current = NULL;
  static stack_frame_info_t *end = NULL;

  Source sglobals;
  stack_frame_info_t *p;
  int inlining = caller != NULL && caller->pr.is_inlining;

  if (end - current <= inlining) {
    current = PyMem_NEW(stack_frame_info_t, FRAME_STACK_ALLOC_BY);
    if (current == NULL)
      OUT_OF_MEMORY();
    end = current + FRAME_STACK_ALLOC_BY;
  }
  p = current;
  current += inlining + 1;
  p->link_stack_depth = -inlining;
  p->co = callee->pr.co;
  sglobals = callee->vlocals.items[INDEX_LOC_GLOBALS]->source;
  if (is_compiletime(sglobals))
    p->globals = (PyObject *)CompileTime_Get(sglobals)->value;
  else
    p->globals = NULL; /* uncommon */
  if (inlining) {
    (p + 1)->co = caller->pr.co;
    sglobals = caller->vlocals.items[INDEX_LOC_GLOBALS]->source;
    if (is_compiletime(sglobals))
      (p + 1)->globals = (PyObject *)CompileTime_Get(sglobals)->value;
    else
      (p + 1)->globals = NULL; /* uncommon */
  }
  return p;
}

static void frame_runtime_capsule_dealloc(PyObject *capsule) {
  FrameRuntime *fr = (FrameRuntime *)PyCapsule_GetPointer(capsule, NULL);
  /* release the references taken in frame_runtime_capsule_new */
  Py_DECREF(fr->py_frame);
  Py_DECREF(fr->psy_code);
  Py_DECREF(fr->psy_globals);
  PyMem_Free(fr);
}

static PyObject *
frame_runtime_capsule_new(PyFrameObject *py_frame,
                          stack_frame_info_t ***psy_frames_start) {
  PyObject *capsule = NULL;
  FrameRuntime *fr = (FrameRuntime *)PyMem_Malloc(sizeof(FrameRuntime));
  if (fr == NULL)
    return PyErr_NoMemory();
  fr->psy_frames_start = psy_frames_start;
  fr->py_frame = py_frame;
  fr->psy_code = py_frame->f_code;
  fr->psy_globals = py_frame->f_globals;
  capsule = PyCapsule_New((void *)fr, NULL, frame_runtime_capsule_dealloc);
  if (capsule == NULL) {
    PyMem_Free(fr);
    return NULL;
  }
  Py_INCREF(py_frame);
  Py_INCREF(py_frame->f_code);
  Py_INCREF(py_frame->f_globals);
  return capsule;
}

DEFINEFN int
psyco_frame_start_register(PyFrameObject *f,
                           stack_frame_info_t ***psy_frames_start) {
  PyObject *tdict;
  tdict = psyco_thread_dict();
  if (tdict == NULL) {
    return 0;
  }
  PyObject *fr_cap = frame_runtime_capsule_new(f, psy_frames_start);
  if (fr_cap == NULL) {
    return 0;
  }
  if (PyDict_SetItem(tdict, (PyObject *)f, fr_cap) == -1) {
    Py_DECREF(fr_cap);
    return 0;
  }
  Py_DECREF(fr_cap);
  return 1;
}

DEFINEFN FrameRuntime *psyco_frame_start_get(PyFrameObject *f) {
  PyObject *tdict;
  tdict = psyco_thread_dict();
  if (tdict == NULL) {
    return NULL;
  }
  {
    /* most frames are not running compiled code: no entry, and no error */
    PyObject *cap = PyDict_GetItem(tdict, (PyObject *)f);
    if (cap == NULL)
      return NULL;
    return (FrameRuntime *)PyCapsule_GetPointer(cap, NULL);
  }
}

DEFINEFN int psyco_frame_start_unregister(PyFrameObject *f) {
  PyObject *tdict;
  tdict = psyco_thread_dict();
  if (tdict == NULL) {
    return 0;
  }
  if (PyDict_DelItem(tdict, (PyObject *)f) == -1) {
    return 0;
  }
  return 1;
}

PSY_INLINE PyFrameObject *psyco_build_pyframe(PyObject *co, PyObject *globals,
                                              PyThreadState *tstate) {
  ni_frame_t *back;
  PyFrameObject *result;

  /* frame objects are not created in stack order
     with Psyco, so it's probably better not to
     create plain wrong chained lists */
  back = ni_frame_current(tstate);
  ni_frame_set_current(tstate, NULL);
  result = PyFrame_New(tstate, (PyCodeObject *)co, globals, NULL);
  if (result == NULL)
    OUT_OF_MEMORY();
  result->f_lasti = -1; /* can be used to identify emulated frames */
  ni_frame_set_current(tstate, back);
  return result;
}

DEFINEFN
PyFrameObject *psyco_emulate_frame(PyObject *o) {
  if (PyFrame_Check(o)) {
    /* a real Python frame */
    Py_INCREF(o);
    return (PyFrameObject *)o;
  } else if (PyTuple_GET_SIZE(o) == 3 && PyFrame_Check(PyTuple_GET_ITEM(o, 2))) {
    /* (co, globals, f): the real frame whose code NiCode_Run is running */
    PyObject *f = PyTuple_GET_ITEM(o, 2);
    Py_INCREF(f);
    return (PyFrameObject *)f;
  } else {
    /* a Psyco frame: emulate it */
    PyObject *co = PyTuple_GetItem(o, 0);
    PyObject *globals = PyTuple_GetItem(o, 1);
    extra_assert(co != NULL);
    extra_assert(globals != NULL);
    return psyco_build_pyframe(co, globals, PyThreadState_GET());
  }
}

struct sfitmp_s {
  stack_frame_info_t **fi;
  struct sfitmp_s *next;
};

static PyObject *pvisitframes(PyObject *(*callback)(PyObject *, void *),
                              void *data) {
  /* Whenever we run Psyco-produced machine code, we mark the current
     Python frame as the starting point of a chained list of Psyco
     frames. The machine code will update this chained list so that
     psyco_next_stack_frame() can be used to visit the list from
     the outermost to the innermost frames. Note that the list does
     not contain the first Psyco frame, the one directly run by a
     call to psyco_processor_run(). This still gives the expected
     result, because psyco_processor_run() is only called by
     eval_hook.c:NiCode_Run() on a real Python frame, which replaces
     the missing Psyco frame.
     XXX this would no longer work if we filled the emulated frames
         with more information, like local variables */

  PyObject *result = NULL;
  FrameRuntime *fstart;
  ni_frame_t *f = ni_frame_current(PyThreadState_Get());
  ni_recursion_suspend();
  while (f != NULL) {
    /* is this Python frame the starting point of a chained
       list of Psyco frames ? */
    fstart = psyco_frame_start_get(ni_frame_object(f));
    if (fstart != NULL) {
      /* Yes. Get the list start. */
      struct sfitmp_s *revlist;
      struct sfitmp_s *p;
      PyObject *o;
      PyObject *g;
      long tag;
      stack_frame_info_t **f1;
      stack_frame_info_t **finfo;
      stack_frame_info_t *fdata;
      stack_frame_info_t *flimit;
      finfo = *(fstart->psy_frames_start);

      /* Enumerate the frames and store them in a
         last-in first-out linked list. The end is marked by
         a pointer with an odd integer value (actually with
         i386 the least significant byte of the integer value
         is -1, and with ivm the end pointer's value is
         exactly 1; but real pointers cannot be odd at all
         because they are aligned anyway). */
      revlist = NULL;
      for (f1 = finfo; (((long)(*f1)) & 1) == 0;
           f1 = psyco_next_stack_frame(f1)) {
        p = (struct sfitmp_s *)PyMem_MALLOC(sizeof(struct sfitmp_s));
        if (p == NULL)
          OUT_OF_MEMORY();
        p->fi = f1;
        p->next = revlist;
        revlist = p;
        if ((*f1)->link_stack_depth == 0)
          break; /* stack top is an inline frame */
      }

      /* now actually visit them in the correct order */
      while (revlist) {
        p = revlist;
        /* a Psyco frame is represented as
           (co, globals, address_of(*fi)) */
        if (result == NULL) {
          tag = (long)(p->fi);
          fdata = *p->fi;
          flimit = finfo_last(fdata);
          while (1) {
            g = fdata->globals;
            if (g == NULL)
              g = ni_frame_globals(f);
            o = Py_BuildValue("OOl", fdata->co, g, tag);
            if (o == NULL)
              OUT_OF_MEMORY();
            result = callback(o, data);
            Py_DECREF(o);
            if (result != NULL)
              break;
            if (fdata == flimit)
              break;
            fdata++, tag--;
          }
        }
        revlist = p->next;
        PyMem_FREE(p);
      }
      if (result != NULL)
        break;

      /* there is still the real Python frame
         which is shadowed by a Psyco frame, i.e. a
         proxy function. Represented as
         (co, globals, f) */
      o = Py_BuildValue("OOO", fstart->psy_code, fstart->psy_globals,
                        ni_frame_object(f));
      if (o == NULL)
        OUT_OF_MEMORY();
      result = callback(o, data);
      Py_DECREF(o);
    } else {
      /* a real unshadowed Python frame */
      result = callback((PyObject *)ni_frame_object(f), data);
    }
    if (result != NULL)
      break;
    f = ni_frame_back(f);
  }
  ni_recursion_resume();
  return result;
}

static PyObject *visit_get_globals(PyObject *o, void *ignored) {
  if (PyFrame_Check(o))
    return ((PyFrameObject *)o)->f_globals;
  else
    return PyTuple_GetItem(o, 1);
}

DEFINEFN
PyObject *psyco_get_globals(void) {
  PyObject *result = pvisitframes(visit_get_globals, NULL);
  if (result == NULL)
    psyco_fatal_msg("sorry, don't know what to do with no globals");
  return result;
}

/* sys._getframe, replaced (as upstream's support.py did) by a version that
   also counts compiled frames, which have no real frame object: they are
   emulated, with f_lasti == -1 and empty locals (KNOWN_BUGS C11; history B2). */

static PyObject *visit_nth_frame(PyObject *o, void *n) {
  if ((*(int *)n)-- == 0)
    return (PyObject *)psyco_emulate_frame(o); /* new reference */
  return NULL;
}

/* the frame 'depth' calls below the top, or NULL (no exception set) */
static PyFrameObject *nth_frame(int depth) {
  return (PyFrameObject *)pvisitframes(visit_nth_frame, &depth);
}

/* as nth_frame, with emulated frames linked to the next frame out; a real
   frame keeps its own f_back, which skips any compiled callers between it
   and its creator (frames are not created in stack order) */
static PyFrameObject *linked_frame(int depth) {
  PyFrameObject *top = nth_frame(depth), *f;
  for (f = top; f != NULL && f->f_lasti == -1 && f->f_back == NULL;) {
    PyFrameObject *next = nth_frame(++depth);
    if (next == NULL)
      break;
    f->f_back = next; /* takes the reference */
    f = next;
  }
  return top;
}

DEFINEFN
PyFrameObject *psyco_running_real_frame(PyCodeObject *co) {
  ni_frame_t *f = ni_frame_current(PyThreadState_GET());
  FrameRuntime *fstart;
  if (f == NULL || ni_frame_code(f) != co)
    return NULL;
  fstart = psyco_frame_start_get(ni_frame_object(f));
  /* the top compiled frame's finfo slot is odd (-1) except while it is in a
     compiled sub-call (ABOUT_TO_CALL_SUBFUNCTION) */
  if (fstart == NULL || (((long)**fstart->psy_frames_start) & 1) == 0)
    return NULL;
  return ni_frame_object(f);
}

static PyObject *ni_getframe(PyObject *self, PyObject *args) {
  int depth = 0;
  PyFrameObject *top;
  if (!PyArg_ParseTuple(args, "|i:_getframe", &depth))
    return NULL;
  top = linked_frame(depth);
  if (top == NULL) {
    PyErr_SetString(PyExc_ValueError, "call stack is not deep enough");
    return NULL;
  }
  return (PyObject *)top;
}

static PyMethodDef ni_getframe_def = {
    "_getframe", ni_getframe, METH_VARARGS,
    "_getframe([depth]) -> frameobject\n\n"
    "Return a frame object from the call stack, counting frames of code\n"
    "compiled by Ni (emulated: no locals, f_lineno is the first line)."};

INITIALIZATIONFN
void psyco_frames_init(void) {
  /* Upstream also hooked _PyThreadState_GetFrame (in 3.7,
     _PyRuntime.gilstate.getframe, behind PyEval_GetFrame), not done yet */
  PyObject *sysmod = PyImport_ImportModule("sys");
  PyObject *fn = PyCFunction_New(&ni_getframe_def, NULL);
  if (sysmod == NULL || fn == NULL ||
      PyObject_SetAttrString(sysmod, "_getframe", fn) < 0)
    PyErr_Print();
  Py_XDECREF(fn);
  Py_XDECREF(sysmod);
}
