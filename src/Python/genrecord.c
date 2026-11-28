/*
 * Virtual generators' records (mechanisms/frames.h:le_gen_record_t). A
 * generator that only compiled code can reach is a virtual object there
 * (psyfunc.c:psyco_vgen_start), whose paused state between steps is a
 * record: a generator object of its own type, untracked, its frame run by
 * the same compiled entries as a generator's frame (psyfunc.c:gen_send).
 * Nothing CPython runs sees the record. Anything else that needs the
 * generator makes it escape: the record becomes the generator in place,
 * as CPython would have left it at that point. A record that ends is
 * cleared as CPython clears a generator's frame that ends; one dropped
 * while paused escapes, so that CPython closes the generator, running its
 * finally blocks at the drop.
 */
#include "genrecord.h"
#include "../vcompiler.h"
#include "pycinternal.h"
#include "../bytecode.h"
#include "../mechanisms/frames.h"
#include "../mechanisms/store_gc.h"
#include "pyruntime.h"

/* exported by CPython, declared in its internal headers where it has them */
PyAPI_FUNC(int) _PyGen_SetStopIterationValue(PyObject *);
PyAPI_FUNC(PyObject *)
    _PyErr_FormatFromCause(PyObject *exception, const char *format, ...);

#if ALL_CHECKS
DEFINEVAR long le_stat_gen_records[6];
#endif

DEFINEFN bool
le_gen_record_check(PyObject *o)
{
    return Py_TYPE(o) == &le_gen_record_type;
}

/* the collector's header before a generator (PyGC_Head): two words where
   it is internal */
#if LE_HAS_GC_HEAD_INTERNAL
#define GC_HEAD_SIZE (2 * sizeof(uintptr_t))
#else
#define GC_HEAD_SIZE sizeof(PyGC_Head)
#endif

/* a record's memory: the collector's header, untracked, then the record,
   not counted among the collector's allocations, so that a record that
   never escapes doesn't hasten a collection; NULL with an exception */
static le_gen_record_t *
record_alloc(size_t size)
{
    char *mem = PyObject_Malloc(GC_HEAD_SIZE + size);
    if (mem == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    memset(mem, 0, GC_HEAD_SIZE);
#if !LE_HAS_GC_HEAD_INTERNAL && !LE_HAS_GC_TRACKED_BY_NEXT
    _PyGCHead_SET_REFS((PyGC_Head *)mem, _PyGC_REFS_UNTRACKED);
#endif
    return (le_gen_record_t *)(mem + GC_HEAD_SIZE);
}

static void
record_free(le_gen_record_t *r)
{
    PyObject_Free((char *)r - GC_HEAD_SIZE);
}

static int
record_type_ready(void);

#if LE_HAS_INTERPRETER_FRAME
/* the frame's function, the reference it holds taken (NULL for none) */
static PyObject *
frame_func_take(le_frame_t *f)
{
    PyObject *o;
#if LE_HAS_FRAME_STACKREFS
    if (PyStackRef_IsNull(f->f_funcobj))
        return NULL;
    o = PyStackRef_AsPyObjectSteal(f->f_funcobj);
    f->f_funcobj = PyStackRef_NULL;
#elif LE_HAS_FRAME_FUNCOBJ
    o = f->f_funcobj;
    f->f_funcobj = NULL;
#else
    o = (PyObject *)f->f_func;
    f->f_func = NULL;
#endif
    return o;
}

/* the frame's function, borrowed */
static PyFunctionObject *
frame_func(le_frame_t *f)
{
#if LE_HAS_FRAME_STACKREFS
    return (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(f->f_funcobj);
#elif LE_HAS_FRAME_FUNCOBJ
    return (PyFunctionObject *)f->f_funcobj;
#else
    return f->f_func;
#endif
}

/* its locals and stack; a running frame's stack is compiled code's
   (LE_HAS_RUNNING_STACK_HIDDEN: depth -1) */
static int
frame_nwords(le_frame_t *f)
{
    int depth = le_frame_stack_depth(f);
    return LE_FRAME_CODE(f)->co_nlocalsplus + (depth > 0 ? depth : 0);
}

DEFINEFN PyObject *
le_gen_record_new(PyObject *func, long start, long nstack)
{
    PyCodeObject *co = (PyCodeObject *)((PyFunctionObject *)func)->func_code;
    Py_ssize_t n = co->co_nlocalsplus + co->co_stacksize;
    /* as a generator is laid out */
    le_gen_record_t *r =
        record_alloc(_PyObject_VAR_SIZE(&le_gen_record_type, n));
    long i;
    if (r == NULL)
        return NULL;
    PyObject_Init((PyObject *)r, &le_gen_record_type);
    Py_SET_SIZE(r, n);
    LE_STAT(le_stat_gen_records[0]++);
    r->gi_exc_state.exc_value = NULL;
    r->gi_exc_state.previous_item = NULL;
    r->gi_frame_state = FRAME_CREATED;
    le_frame_init_gen(le_record_frame(r), (PyFunctionObject *)func, (int)start,
                      LE_FRAME_OWNED_BY_RECORD);
    for (i = 0; i < nstack; i++)
        le_frame_word_put(le_record_frame(r), co->co_nlocalsplus + i, NULL);
    le_gen_frame_set_depth(le_record_frame(r), (int)nstack);
    return (PyObject *)r;
}

/* the instructions before RETURN_GENERATOR: MAKE_CELL, COPY_FREE_VARS */
DEFINEFN int
le_gen_record_start(PyObject *rec, PyObject *func)
{
    le_frame_t *f = &*le_record_frame((le_gen_record_t *)rec);
    PyCodeObject *co = LE_FRAME_CODE(f);
    le_code_t c = le_code(co);
    int pos, i;
    (void)func; /* the frame holds it */
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        if (in.op == RETURN_GENERATOR)
            break;
        if (in.op == MAKE_CELL) {
            PyObject *cell = PyCell_New(LE_FRAME_WORD(f, in.arg));
            if (cell == NULL)
                return -1;
            le_frame_word_set(f, in.arg, cell);
        }
        else if (in.op == COPY_FREE_VARS) {
            PyObject *closure = frame_func(f)->func_closure;
            int ofs = co->co_nlocalsplus - in.arg;
            for (i = 0; i < in.arg; i++)
                le_frame_word_put(f, ofs + i,
                                  Py_NewRef(PyTuple_GET_ITEM(closure, i)));
        }
        pos = in.next;
    }
    return 0;
}

/* The frame object 'fo', a view of the record's frame 'f', takes the frame
   as its own, as frameobject.c's take_ownership does when a frame it
   outlives is cleared: its words, locals mapping and function move to it;
   the record keeps its code. A place before the first traceable
   instruction becomes that one, and the frame's caller, if it is still
   linked, becomes the frame object's. */
static void
frame_give(PyFrameObject *fo, le_frame_t *f)
{
    le_frame_t *own = (le_frame_t *)fo->_f_frame_data, *p;
    PyCodeObject *co = LE_FRAME_CODE(f);
    int k, n = frame_nwords(f);
    if (fo->f_back == NULL) {
        for (p = f->previous; p != NULL && !le_frame_complete(p);)
            p = p->previous;
        if (p != NULL) {
            fo->f_back = (PyFrameObject *)Py_XNewRef(le_frame_object(p));
            psyco_frame_stands(fo->f_back);
        }
    }
    memcpy(own, f, (char *)&f->localsplus[n] - (char *)f);
#if LE_HAS_FRAME_STACKREFS
    own->stackpointer = own->localsplus + n;
    own->f_executable = PyStackRef_FromPyObjectNew((PyObject *)co);
#elif LE_HAS_FRAME_EXECUTABLE
    own->f_executable = Py_NewRef(co);
#else
    own->f_code = (PyCodeObject *)Py_NewRef(co);
#endif
#if !LE_HAS_FRAME_STACKREFS
    le_gen_frame_set_depth(own, n - co->co_nlocalsplus);
#endif
    own->owner = FRAME_OWNED_BY_FRAME_OBJECT;
    own->previous = NULL;
    own->frame_obj = NULL; /* as a frame object's own frame has it */
    if (!le_frame_complete(own))
        le_frame_set_lasti(own, 0);
    fo->f_frame = own;
    for (k = 0; k < n; k++) le_frame_word_put(f, k, NULL);
    le_gen_frame_set_depth(f, 0);
    f->f_locals = NULL;
    (void)frame_func_take(f); /* the frame object's now */
    f->frame_obj = NULL;
    if (!PyObject_GC_IsTracked((PyObject *)fo))
        PyObject_GC_Track(fo);
}

/* the frame's words released in the order CPython frees them, then its
   locals mapping (_PyFrame_ClearLocals) */
static void
frame_clear_locals(le_frame_t *f)
{
    int k, n = frame_nwords(f);
    for (k = 0; k < n; k++)
        Py_XDECREF(le_frame_word_take(
            f, LE_HAS_FRAME_CLEARED_LAST_FIRST ? n - 1 - k : k));
    le_gen_frame_set_depth(f, 0);
    Py_CLEAR(f->f_locals);
}

DEFINEFN void
le_frame_clear(le_frame_t *f)
{
    PyFrameObject *fo = f->frame_obj;
    PyObject *o;
    if (fo != NULL) {
        if (Py_REFCNT(fo) > 1) {
            frame_give(fo, f);
            Py_DECREF(fo);
            return;
        }
        f->frame_obj = NULL;
        Py_DECREF(fo);
    }
    frame_clear_locals(f);
    o = frame_func_take(f);
    Py_XDECREF(o);
}

#if LE_HAS_GEN_CLOSE_UNRUN
/* CPython closes a generator paused at a yield in no try block of the
   body's own (the one around the body is PEP 479's) without running it,
   unless a tool would see the unwinding (gen_close). A record closes so
   only while no tool is on at all; otherwise its generator is made and
   CPython chooses. */
static bool
record_closes_unrun(le_gen_record_t *r)
{
    le_frame_t *f = le_record_frame(r);
    PyCodeObject *co = LE_FRAME_CODE(f);
    le_instr_t in;
    if (le_monitoring_on(PyThreadState_GET()) || le_code_monitored(co))
        return false;
    in = le_instr(le_code(co),
                  (int)((char *)LE_FRAME_PLACE(f) - (char *)_PyCode_CODE(co)));
    return in.op == RESUME && (in.arg & RESUME_OPARG_DEPTH1_MASK) != 0;
}
#endif

static int
record_escape(le_gen_record_t *r, PyFunctionObject *func, int8_t state);

/* whether word k of the frame 'f' is a record: one its body steps by
   yield from, held there while it is paused (pycompiler.c:yield_resumable) */
static bool
word_is_record(le_frame_t *f, int k)
{
    PyObject *o;
#if LE_HAS_TAGGED_LASTI
    if ((f->localsplus[k].bits & Py_INT_TAG) == Py_INT_TAG)
        return false; /* a handler's lasti */
#endif
    o = LE_FRAME_WORD(f, k);
    return o != NULL && Py_TYPE(o) == &le_gen_record_type;
}

/* the records in the stack of the frame 'f' made real, so that a
   generator made of the frame holds only real ones; -1 with an exception */
static int
words_escape(le_frame_t *f)
{
    int k, n = frame_nwords(f);
    for (k = LE_FRAME_CODE(f)->co_nlocalsplus; k < n; k++)
        if (word_is_record(f, k)) {
            PyObject *gen = le_gen_record_real(LE_FRAME_WORD(f, k));
            if (gen == NULL)
                return -1;
            le_frame_word_set(f, k, gen);
        }
    return 0;
}

/* whether the record 'r' has become its generator */
static bool
record_escaped(le_gen_record_t *r)
{
    return Py_TYPE(r) != &le_gen_record_type;
}

DEFINEFN PyObject *
le_gen_record_real(PyObject *rec)
{
    le_gen_record_t *r = (le_gen_record_t *)rec;
    if (record_escaped(r))
        return Py_NewRef(rec);
    if (r->gi_frame_state >= FRAME_CLEARED) {
        /* ended, its function let go: only the word that held it until
           END_SEND pops it can still see it, which nothing reads */
        return Py_NewRef(Py_None);
    }
    LE_STAT(le_stat_gen_records[1]++);
    if (record_escape(r, frame_func(le_record_frame(r)), r->gi_frame_state) <
        0)
        return NULL;
    return Py_NewRef(rec);
}

/* The record 'r' becomes the generator of the function 'func' it stands
   for, in the state 'state', in place: its header is written, and its
   frame, and any frame object that is a view of it, stay where they are.
   -1 with an exception. */
static int
record_escape(le_gen_record_t *r, PyFunctionObject *func, int8_t state)
{
    le_frame_t *f = le_record_frame(r);
    if (words_escape(f) < 0)
        return -1;
    le_gen_init_names(r, func);
    Py_SET_TYPE(r, le_gen_type(LE_FRAME_CODE(f)));
    f->owner = FRAME_OWNED_BY_GENERATOR;
    r->gi_frame_state = state;
    PyObject_GC_Track(r);
    return 0;
}

DEFINEFN PyObject *
le_gen_record_escape(PyObject *rec, PyObject *func)
{
    le_gen_record_t *r = (le_gen_record_t *)rec;
    int8_t state = r->gi_frame_state;
    if (record_escaped(r))
        return Py_NewRef(rec);
    if (state < FRAME_CLEARED)
        LE_STAT(le_stat_gen_records[1]++);
    else {
        LE_STAT(le_stat_gen_records[4]++);
        /* ended: a generator of the same function, ended too, its words
           long freed */
#if LE_HAS_FRAME_STACKREFS
        le_record_frame(r)->f_funcobj = PyStackRef_FromPyObjectNew(func);
#elif LE_HAS_FRAME_FUNCOBJ
        le_record_frame(r)->f_funcobj = Py_NewRef(func);
#else
        le_record_frame(r)->f_func = (PyFunctionObject *)Py_NewRef(func);
#endif
        state = LE_GEN_ENDED_STATE;
    }
    if (record_escape(r, (PyFunctionObject *)func, state) < 0)
        return NULL;
    return Py_NewRef(rec);
}

DEFINEFN PyObject *
le_gen_record_escape_frame(le_frame_t *f)
{
    return le_gen_record_escape((PyObject *)le_frame_record(f),
                                (PyObject *)frame_func(f));
}

DEFINEFN PyGenObject *
le_gen_record_escape_running(le_frame_t *f)
{
    le_gen_record_t *r = le_frame_record(f);
    if (record_escaped(r))
        return r;
    LE_STAT(le_stat_gen_records[3]++);
    /* running, as gen_send_ex2 leaves it: its exception state is in the
       thread's chain already, and its stack still compiled code's */
    if (record_escape(r, frame_func(f), FRAME_EXECUTING) < 0)
        return NULL;
    return r;
}

/* The end of a run of the record's body from compiled code that did not
   pause in the usual way, for the step's three forms: 0 for next(), 1
   for send(), 2 for yield from's send (out, as le_iter_send). Paused (in
   the usual way after all), the yielded value; else ended, as gen_send_ex2
   and ceval end a generator's run: the thread's exception state is the
   caller's again, StopIteration raised by the body becomes RuntimeError
   where CPython's C does it (where the bytecode doesn't), and the frame is
   cleared, in CPython's order. */
static PyObject *
record_end(PyThreadState *ts, le_gen_record_t *r, PyObject *result, int form,
           int *paused)
{
    *paused = 0;
    if (record_escaped(r)) {
        /* made real while it ran: ended as a generator's run */
        PyObject *gen = (PyObject *)r;
        if (form == 2) {
            PyObject *out[1];
            int st = le_gen_direct_send_status(ts, gen, result, out);
            if (st < 0)
                return NULL;
            *paused = st;
            return out[0];
        }
        result = form == 1 ? le_gen_direct_send_end(ts, gen, result)
                           : le_gen_direct_end(ts, gen, result);
        *paused = result != NULL;
        return result;
    }
    le_gen_frame_end(le_record_frame(r), result);
    ts->exc_info = r->gi_exc_state.previous_item;
    r->gi_exc_state.previous_item = NULL;
    if (LE_GEN_PAUSED(r->gi_frame_state)) {
        le_record_frame(r)->previous = NULL;
        *paused = 1;
        return result;
    }
#if LE_HAS_FINISHED_GENERATOR_UNLINKED
    le_record_frame(r)->previous = NULL;
#endif
#if !LE_HAS_OPCODE_CALL_INTRINSIC_1
    /* PEP 479, as gen_send_ex2 does it once the frame is left */
    if (result == NULL && PyErr_ExceptionMatches(PyExc_StopIteration))
        _PyErr_FormatFromCause(PyExc_RuntimeError, "%s",
                               LE_MSG_GENERATOR_STOP_ITERATION);
#endif
    r->gi_frame_state = FRAME_CLEARED;
#if LE_HAS_EVAL_FRAME_POPS
    /* ceval's clear_gen_frame */
    le_frame_clear(le_record_frame(r));
    Py_CLEAR(r->gi_exc_state.exc_value);
#else
    /* gen_send_ex2's */
    Py_CLEAR(r->gi_exc_state.exc_value);
    le_frame_clear(le_record_frame(r));
#endif
    le_record_frame(r)->previous = NULL;
    return result;
}

DEFINEFN PyObject *
le_gen_record_end(PyThreadState *ts, PyObject *rec, PyObject *result)
{
    int paused;
    result = record_end(ts, (le_gen_record_t *)rec, result, 0, &paused);
    if (paused || result == NULL)
        return result;
    if (result != Py_None)
        _PyGen_SetStopIterationValue(result);
    Py_DECREF(result);
    return NULL;
}

DEFINEFN PyObject *
le_gen_record_send_end(PyThreadState *ts, PyObject *rec, PyObject *result)
{
    int paused;
    result = record_end(ts, (le_gen_record_t *)rec, result, 1, &paused);
    if (paused || result == NULL)
        return result;
    if (result == Py_None)
        PyErr_SetNone(PyExc_StopIteration);
    else
        _PyGen_SetStopIterationValue(result);
    Py_DECREF(result);
    return NULL;
}

DEFINEFN int
le_gen_record_send_status(PyThreadState *ts, PyObject *rec, PyObject *result,
                          PyObject *out[])
{
    int paused;
    result = record_end(ts, (le_gen_record_t *)rec, result, 2, &paused);
    if (result == NULL)
        return -1;
    out[0] = result;
    return paused;
}

/* Dropped: as CPython drops a generator in the same state. Not started,
   its close only marks it ended, so its frame is cleared here; paused, it
   escapes and the generator is dropped, which closes it. Compiled code
   that knew the record's type calls this even after it has escaped, so an
   escaped one goes to its generator's dealloc. */
static void
record_dealloc(PyObject *self)
{
    le_gen_record_t *r = (le_gen_record_t *)self;
    PyObject *code;
    if (record_escaped(r)) {
        Py_TYPE(self)->tp_dealloc(self);
        return;
    }
    if (r->gi_frame_state == FRAME_CREATED) {
        r->gi_frame_state = FRAME_CLEARED;
        le_frame_clear(le_record_frame(r));
    }
#if LE_HAS_GEN_CLOSE_UNRUN
    else if (r->gi_frame_state != FRAME_CLEARED && record_closes_unrun(r)) {
        /* as CPython's close, then its dealloc */
        LE_STAT(le_stat_gen_records[5]++);
        if (LE_HAS_GEN_CLOSE_LOCALS_ONLY)
            frame_clear_locals(le_record_frame(r));
        r->gi_frame_state = FRAME_CLEARED;
        le_frame_clear(le_record_frame(r));
        Py_CLEAR(r->gi_exc_state.exc_value);
    }
#endif
    else if (r->gi_frame_state != FRAME_CLEARED) {
        PyObject *t, *v, *tb;
        int escaped;
        PyErr_Fetch(&t, &v, &tb);
        LE_STAT(le_stat_gen_records[2]++);
        escaped = record_escape(r, frame_func(le_record_frame(r)),
                                r->gi_frame_state) == 0;
        if (!escaped)
            PyErr_WriteUnraisable(self);
        PyErr_Restore(t, v, tb);
        if (escaped) {
            /* the generator's dealloc, which closes it */
            Py_TYPE(self)->tp_dealloc(self);
            return;
        }
        r->gi_frame_state = FRAME_CLEARED;
        le_frame_clear(le_record_frame(r));
    }
    Py_CLEAR(r->gi_exc_state.exc_value);
#if LE_HAS_FRAME_STACKREFS
    code = PyStackRef_AsPyObjectSteal(le_record_frame(r)->f_executable);
#elif LE_HAS_FRAME_EXECUTABLE
    code = le_record_frame(r)->f_executable;
#else
    code = (PyObject *)le_record_frame(r)->f_code;
#endif
    Py_XDECREF(code);
    record_free(r);
}

#else
/* Here a record is a generator object of the record's type holding a
   frame object, which is linked to it as a generator's frame is (f_gen):
   neither is tracked until the record escapes. A run of its body that
   ends is finished as a generator's (eval_hook.c:le_gen_direct_end),
   which lets its frame go (gi_frame NULL). */

/* the instructions before RETURN_GENERATOR: none here, the record's
   frame is made at its call */
DEFINEFN long
le_gen_record_start_unit(PyCodeObject *co, bool *takes_iter)
{
    (void)co;
    *takes_iter = false;
    if (!(le_gen_record_type.tp_flags & Py_TPFLAGS_READY) &&
        record_type_ready() < 0) {
        PyErr_Clear();
        return -1;
    }
    return 0;
}

DEFINEFN PyObject *
le_gen_record_new(PyObject *func, long start, long nstack)
{
    PyFunctionObject *fn = (PyFunctionObject *)func;
    PyCodeObject *co = (PyCodeObject *)fn->func_code;
    PyThreadState *ts = PyThreadState_GET();
    le_gen_record_t *r;
    PyFrameObject *f;
    (void)start;
    (void)nstack; /* none: a body takes nothing before its start */
    r = record_alloc(le_gen_record_type.tp_basicsize);
    if (r == NULL)
        return NULL;
    /* the frame CPython's call makes, with its builtins, and no caller
       until it runs */
    f = le_pyframe_new_by_cpython(ts, co, fn->func_globals);
    if (f == NULL) {
        record_free(r);
        return NULL;
    }
    Py_CLEAR(f->f_back);
#if LE_HAS_FRAME_BUILTINS_OF_FUNCTION
    le_frame_set_builtins(f, fn->func_builtins);
#endif
    PyObject_GC_UnTrack(f);
    PyObject_Init((PyObject *)r, &le_gen_record_type);
    LE_STAT(le_stat_gen_records[0]++);
    r->gi_frame = f;
    f->f_gen = (PyObject *)r;
#if !LE_HAS_FRAME_STATE
    r->gi_running = 0;
#endif
    Py_INCREF(co);
    r->gi_code = (PyObject *)co;
    r->gi_weakreflist = NULL;
    Py_INCREF(fn->func_name);
    r->gi_name = fn->func_name;
    Py_INCREF(fn->func_qualname);
    r->gi_qualname = fn->func_qualname;
    memset(&r->gi_exc_state, 0, sizeof(r->gi_exc_state));
    return (PyObject *)r;
}

/* its arguments are in its words: its cells are made, a cell of an
   argument taking the argument's value, and its free variables are its
   function's closure's, as CPython's call fills a new frame */
DEFINEFN int
le_gen_record_start(PyObject *rec, PyObject *func)
{
    le_frame_t *f = le_record_frame((le_gen_record_t *)rec);
    PyCodeObject *co = le_frame_code(f);
    PyObject *closure = ((PyFunctionObject *)func)->func_closure;
    int ncells = le_code_ncells(co), nfrees = le_code_nfrees(co), i;
    for (i = 0; i < ncells; i++) {
        int arg = le_code_cell_arg(co, i);
        PyObject *cell = PyCell_New(arg >= 0 ? LE_FRAME_WORD(f, arg) : NULL);
        if (cell == NULL)
            return -1;
        if (arg >= 0)
            le_frame_word_set(f, arg, NULL);
        le_frame_word_put(f, co->co_nlocals + i, cell);
    }
    for (i = 0; i < nfrees; i++) {
        PyObject *o = PyTuple_GET_ITEM(closure, i);
        Py_INCREF(o);
        le_frame_word_put(f, co->co_nlocals + ncells + i, o);
    }
    return 0;
}

/* whether the record 'r' has become its generator */
static bool
record_escaped(le_gen_record_t *r)
{
    return Py_TYPE(r) != &le_gen_record_type;
}

/* the records in the stack of the paused frame 'f' made real (as
   yield_resumable leaves one a yield from steps), so that a generator
   made of the frame holds only real ones; -1 with an exception */
static int
words_escape(le_frame_t *f)
{
    int k, base = le_code_nlocalsplus(le_frame_code(f));
    int n = base + le_frame_stack_depth(f);
    for (k = base; k < n; k++) {
        PyObject *o = LE_FRAME_WORD(f, k);
        if (o != NULL && Py_TYPE(o) == &le_gen_record_type) {
            PyObject *gen = le_gen_record_real(o);
            if (gen == NULL)
                return -1;
            le_frame_word_set(f, k, gen);
        }
    }
    return 0;
}

/* The record 'r' becomes the generator it stands for, in place, in the
   state it is in: its frame, if it has one, is the generator's, and
   both are tracked from now on. A running one's stack is compiled
   code's, so holds no records. -1 with an exception. */
static int
record_escape(le_gen_record_t *r)
{
    le_frame_t *f = le_record_frame(r);
    if (record_escaped(r))
        return 0;
    if (f != NULL && !le_frame_executing(f) && words_escape(f) < 0)
        return -1;
    LE_STAT(le_stat_gen_records[f == NULL ? 4 : 1]++);
    ((PyObject *)r)->ob_type = &PyGen_Type;
    if (f != NULL && !LE_GC_IS_TRACKED(f))
        PyObject_GC_Track(f);
    PyObject_GC_Track(r);
    return 0;
}

DEFINEFN PyObject *
le_gen_record_real(PyObject *rec)
{
    if (record_escape((le_gen_record_t *)rec) < 0)
        return NULL;
    Py_INCREF(rec);
    return rec;
}

DEFINEFN PyObject *
le_gen_record_escape(PyObject *rec, PyObject *func)
{
    (void)func;
    return le_gen_record_real(rec);
}

DEFINEFN PyObject *
le_gen_record_escape_frame(le_frame_t *f)
{
    return le_gen_record_real((PyObject *)le_frame_record(f));
}

DEFINEFN PyGenObject *
le_gen_record_escape_running(le_frame_t *f)
{
    le_gen_record_t *r = le_frame_record(f);
    if (!record_escaped(r))
        LE_STAT(le_stat_gen_records[3]++);
    /* running, as gen_send_ex leaves it: its exception state is in the
       thread's chain already */
    if (record_escape(r) < 0)
        return NULL;
    return r;
}

/* Dropped: as CPython drops a generator in the same state. Not started,
   its close only unwinds it at once, so its frame goes here; paused, it
   escapes and the generator is dropped, which closes it. Compiled code that
   knew the record's type calls this even after it has escaped, so an escaped
   one goes to its generator's dealloc. */
static void
record_dealloc(PyObject *self)
{
    le_gen_record_t *r = (le_gen_record_t *)self;
    le_frame_t *f = le_record_frame(r);
    if (record_escaped(r)) {
        Py_TYPE(self)->tp_dealloc(self);
        return;
    }
    if (f != NULL && f->f_lasti >= 0) {
        PyObject *t, *v, *tb;
        int escaped;
        PyErr_Fetch(&t, &v, &tb);
        LE_STAT(le_stat_gen_records[2]++);
        escaped = record_escape(r) == 0;
        if (!escaped)
            PyErr_WriteUnraisable(self);
        PyErr_Restore(t, v, tb);
        if (escaped) {
            /* the generator's dealloc, which closes it */
            Py_TYPE(self)->tp_dealloc(self);
            return;
        }
    }
    if (f != NULL) {
        /* never run: its words, then the rest, as frame_dealloc frees */
        int k, n = le_code_nlocalsplus(le_frame_code(f));
        f->f_gen = NULL;
        r->gi_frame = NULL;
        for (k = 0; k < n; k++) Py_CLEAR(LE_FRAME_WORD(f, k));
        if (Py_REFCNT(f) > 1 && !LE_GC_IS_TRACKED(f))
            PyObject_GC_Track(f);
        Py_DECREF(f);
    }
    Py_XDECREF(r->gi_code);
    Py_XDECREF(r->gi_name);
    Py_XDECREF(r->gi_qualname);
    record_free(r);
}
#endif

/* sized as a generator, for the generator it may become; the sizes are
   set when the type is made ready (le_gen_record_start_unit). Not a type
   the collector tracks: a record never is, only the generator it becomes,
   and compiled code drops an object of such a type through _Py_Dealloc
   where it could call record_dealloc. */
PyTypeObject le_gen_record_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "generator record",
    .tp_dealloc = record_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

/* the record's type made ready, as big as the biggest of the types a
   record can become */
static int
record_type_ready(void)
{
    PyTypeObject *types[] = {&PyGen_Type, &PyCoro_Type, &PyAsyncGen_Type};
    int i;
    for (i = 0; i < 3; i++) {
        if (le_gen_record_type.tp_basicsize < types[i]->tp_basicsize)
            le_gen_record_type.tp_basicsize = types[i]->tp_basicsize;
        if (le_gen_record_type.tp_itemsize < types[i]->tp_itemsize)
            le_gen_record_type.tp_itemsize = types[i]->tp_itemsize;
    }
    return PyType_Ready(&le_gen_record_type);
}

#if LE_HAS_INTERPRETER_FRAME
/* the unit of RETURN_GENERATOR in the code of a generator function, -1 if
   it has none or something before it a record's start doesn't do
   (le_gen_record_start) other than the iterator a generator expression
   takes of its argument just before it (LOAD_FAST 0, GET_ITER): then
   '*takes_iter' is set. The record's type is made ready here, when code
   that makes records is first compiled, not before. */
DEFINEFN long
le_gen_record_start_unit(PyCodeObject *co, bool *takes_iter)
{
    le_code_t c = le_code(co);
    int pos;
    *takes_iter = false;
    if (!(le_gen_record_type.tp_flags & Py_TPFLAGS_READY) &&
        record_type_ready() < 0) {
        PyErr_Clear();
        return -1;
    }
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        if (in.op == RETURN_GENERATOR)
            return pos / LE_UNIT;
        if (*takes_iter)
            return -1;
        if (in.op == LOAD_FAST && in.arg == 0 && co->co_argcount == 1 &&
            in.next < c.size) {
            le_instr_t get = le_instr(c, in.next);
            if (get.op != GET_ITER)
                return -1;
            *takes_iter = true;
            pos = get.next;
            continue;
        }
        if (in.op != MAKE_CELL && in.op != COPY_FREE_VARS && in.op != RESUME &&
            in.op != NOP)
            return -1;
        pos = in.next;
    }
    return -1;
}
#endif
