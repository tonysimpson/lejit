/* On-stack replacement's events: a function's frame running in ceval
   enters compiled code at a loop head (eval_hook.c:le_osr_switch) once its
   code's loops have jumped back a given number of times. ceval hands
   running frames to C code only in tracing events, so the backward jumps
   are counted in one: from sys.monitoring, the JUMP events of tool
   LE_OSR_TOOL on that code alone; before it, LeJIT's own C trace function,
   installed in the thread's state as PyEval_SetTrace would but without its
   sys.settrace audit event, and with no object, so sys.gettrace() stays
   None. The count is per code object (le_code_extra_t.osr_left). */
#include "Python/frames.h"
#include "bytecode.h"
#include "psyco.h"
#include <Python.h>
#include "mechanisms/frames.h"

#if LE_HAS_MONITORING
/* sys.monitoring's tool id for optimizers, as CPython names it
   (OPTIMIZER_ID) */
#define LE_OSR_TOOL 5

/* 1 once LeJIT holds the tool id, -1 if another tool held it */
static int tool_held;
static long jump_event;
static PyObject *disable;

/* sys.monitoring, a new reference, read without the import system */
static PyObject *
monitoring(void)
{
    PyObject *sys = PyImport_AddModule("sys"); /* borrowed */
    return sys == NULL ? NULL : PyObject_GetAttrString(sys, "monitoring");
}

/* sys.monitoring.name(*args), a new reference */
static PyObject *
monitoring_call(const char *name, const char *format, ...)
{
    PyObject *mon = monitoring(), *fn, *args, *r = NULL;
    va_list va;
    if (mon == NULL)
        return NULL;
    fn = PyObject_GetAttrString(mon, name);
    Py_DECREF(mon);
    if (fn == NULL)
        return NULL;
    va_start(va, format);
    args = Py_VaBuildValue(format, va);
    va_end(va);
    if (args != NULL)
        r = PyObject_CallObject(fn, args);
    Py_DECREF(fn);
    Py_XDECREF(args);
    return r;
}

static int
code_events_set(PyCodeObject *co, long events)
{
    PyObject *r = monitoring_call("set_local_events", "(iOl)", LE_OSR_TOOL,
                                  (PyObject *)co, events);
    Py_XDECREF(r);
    return r == NULL ? -1 : 0;
}

/* JUMP(code, instruction_offset, destination_offset): a backward jump of
   a code being counted counts one; a forward one is never given again */
static PyObject *
osr_jump(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyCodeObject *co = (PyCodeObject *)args[0];
    long from = PyLong_AsLong(args[1]), to = PyLong_AsLong(args[2]);
    le_code_extra_t *x = le_code_extra_get(co);
    PyThreadState *ts;
    le_frame_t *f;
    (void)self;
    (void)nargs;
    if (to > from) {
        Py_INCREF(disable);
        return disable;
    }
    if (x == NULL || x->osr_left <= 0 || --x->osr_left > 0)
        Py_RETURN_NONE;
    /* before anything runs, so that the compiled code isn't traced */
    if (code_events_set(co, 0) < 0)
        return NULL;
    ts = PyThreadState_GET();
    f = le_frame_current(ts);
    if (f == NULL || le_frame_code(f) != co)
        Py_RETURN_NONE;
    if (le_osr_switch(ts, f, (int)to) < 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyMethodDef osr_jump_def = {
    "lejit_jump", (PyCFunction)(void (*)(void))osr_jump, METH_FASTCALL, NULL};

/* LeJIT's hold on the tool id: 0, or -1 if it can't have it (another tool
   has it), with an exception only on an error */
static int
tool_take(void)
{
    PyObject *r, *cb;
    if (tool_held)
        return tool_held > 0 ? 0 : -1;
    r = monitoring_call("get_tool", "(i)", LE_OSR_TOOL);
    if (r == NULL)
        return -1;
    if (r != Py_None) {
        Py_DECREF(r);
        tool_held = -1;
        return -1;
    }
    Py_DECREF(r);
    if (disable == NULL) {
        PyObject *mon = monitoring(), *events, *jump;
        if (mon == NULL)
            return -1;
        disable = PyObject_GetAttrString(mon, "DISABLE");
        events = PyObject_GetAttrString(mon, "events");
        Py_DECREF(mon);
        jump = events == NULL ? NULL : PyObject_GetAttrString(events, "JUMP");
        Py_XDECREF(events);
        if (disable == NULL || jump == NULL) {
            Py_XDECREF(jump);
            Py_CLEAR(disable);
            return -1;
        }
        jump_event = PyLong_AsLong(jump);
        Py_DECREF(jump);
    }
    r = monitoring_call("use_tool_id", "(is)", LE_OSR_TOOL, "lejit");
    if (r == NULL)
        return -1;
    Py_DECREF(r);
    cb = PyCFunction_New(&osr_jump_def, NULL);
    if (cb == NULL)
        return -1;
    r = monitoring_call("register_callback", "(ilO)", LE_OSR_TOOL, jump_event,
                        cb);
    Py_DECREF(cb);
    if (r == NULL)
        return -1;
    Py_DECREF(r);
    tool_held = 1;
    return 0;
}

DEFINEFN int
le_osr_arm(PyCodeObject *co, long k)
{
    le_code_extra_t *x = le_code_extra(co);
    if (x == NULL)
        return -1;
    if (tool_take() < 0)
        return PyErr_Occurred() ? -1 : 0;
    if (code_events_set(co, k > 0 ? jump_event : 0) < 0)
        return -1;
    x->osr_left = k;
    return 1;
}
#else
/* the jump targets at or before their jumps, as bits in the record */
static int
loop_heads_make(le_code_extra_t *x, PyCodeObject *co)
{
    le_code_t c = le_code(co);
    int pos, units = c.size / LE_UNIT;
    x->loop_heads = PyMem_Calloc(units / 8 + 1, 1);
    if (x->loop_heads == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        if (in.target >= 0 && in.target <= pos)
            x->loop_heads[in.target / LE_UNIT / 8] |=
                1 << (in.target / LE_UNIT % 8);
        pos = in.next;
    }
    return 0;
}

extern void
le_trace_set_hidden(Py_tracefunc fn); /* pyruntime.c */

/* a line event at a loop head of a code being counted counts one: a
   backward jump always gives one there */
static int
osr_trace(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg)
{
    le_frame_t *f;
    le_code_extra_t *x;
    int lasti, head;
    PyThreadState *ts;
    (void)obj;
    (void)arg;
    if (what != PyTrace_LINE)
        return 0;
    f = le_frame_of(frame);
    x = le_code_extra_get(le_frame_code(f));
    if (x == NULL || x->osr_left <= 0 || x->loop_heads == NULL)
        return 0;
    lasti = le_frame_lasti(f);
    head = LE_HAS_LASTI_IN_INSTRUCTIONS ? lasti * LE_UNIT : lasti;
    if (!(x->loop_heads[head / LE_UNIT / 8] & (1 << (head / LE_UNIT % 8))) ||
        --x->osr_left > 0)
        return 0;
    /* before anything runs, so that the compiled code isn't traced */
    le_trace_set_hidden(NULL);
    ts = PyThreadState_GET();
    return le_osr_switch(ts, f, head) < 0 ? -1 : 0;
}

DEFINEFN int
le_osr_arm(PyCodeObject *co, long k)
{
    PyThreadState *ts = PyThreadState_GET();
    le_code_extra_t *x = le_code_extra(co);
    if (x == NULL || (x->loop_heads == NULL && loop_heads_make(x, co) < 0))
        return -1;
    /* not over a tool's trace function */
    if (ts->c_tracefunc != NULL && ts->c_tracefunc != osr_trace)
        return 0;
    x->osr_left = k;
    if (k > 0 && ts->c_tracefunc == NULL)
        le_trace_set_hidden(osr_trace);
    else if (k == 0 && ts->c_tracefunc == osr_trace)
        le_trace_set_hidden(NULL);
    return 1;
}
#endif
