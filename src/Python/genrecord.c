/*
 * Virtual generators' records (mechanisms/frames.h:le_gen_record_t). A
 * generator that only compiled code can reach is a virtual object there
 * (psyfunc.c:psyco_vgen_start), whose paused state between steps is a
 * record: an interpreter frame laid out as a generator's, run by the same
 * compiled entries as a generator's frame (psyfunc.c:gen_send), with the
 * generator's state and exception state beside it. Nothing CPython runs
 * sees the record. Anything else that needs the generator makes it
 * escape: the real generator is made from the record as CPython would
 * have left it at that point (a frame object owning the record's frame,
 * then frames.c:le_gen_of_frame), and the record holds nothing but it from
 * then on. A record that ends is cleared as CPython clears a generator's
 * frame that ends; one dropped while paused escapes, so that CPython
 * closes the generator, running its finally blocks at the drop.
 */
#include "genrecord.h"
#include "../vcompiler.h"
#include "pycinternal.h"
#include "../bytecode.h"
#include "../mechanisms/frames.h"

#if LE_RESUMES_GENERATORS_DIRECTLY

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
le_gen_record_new(PyObject *func, long start)
{
    PyCodeObject *co = (PyCodeObject *)((PyFunctionObject *)func)->func_code;
    le_gen_record_t *r = PyObject_Malloc(
        offsetof(le_gen_record_t, frame.localsplus) +
        (co->co_nlocalsplus + co->co_stacksize) * sizeof(PyObject *));
    if (r == NULL)
        return PyErr_NoMemory();
    PyObject_Init((PyObject *)r, &le_gen_record_type);
    LE_STAT(le_stat_gen_records[0]++);
    r->escaped = NULL;
    r->exc_state.exc_value = NULL;
    r->exc_state.previous_item = NULL;
    r->state = FRAME_CREATED;
    le_frame_init_gen(&r->frame, (PyFunctionObject *)func, (int)start,
                      LE_FRAME_OWNED_BY_RECORD);
    return (PyObject *)r;
}

/* the instructions before RETURN_GENERATOR: MAKE_CELL, COPY_FREE_VARS */
DEFINEFN int
le_gen_record_start(PyObject *rec)
{
    le_frame_t *f = &((le_gen_record_t *)rec)->frame;
    PyCodeObject *co = LE_FRAME_CODE(f);
    le_code_t c = le_code(co);
    int pos, i;
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
   the record keeps its code. Its place stays unless 'ended', where a
   place before the first traceable instruction becomes that one, and the
   frame's caller, if it is still linked, becomes the frame object's. */
static void
frame_give(PyFrameObject *fo, le_frame_t *f, bool ended)
{
    le_frame_t *own = (le_frame_t *)fo->_f_frame_data, *p;
    PyCodeObject *co = LE_FRAME_CODE(f);
    int k, n = frame_nwords(f);
    if (ended && fo->f_back == NULL) {
        for (p = f->previous; p != NULL && !le_frame_complete(p);)
            p = p->previous;
        if (p != NULL)
            fo->f_back = (PyFrameObject *)Py_XNewRef(le_frame_object(p));
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
    if (ended && !le_frame_complete(own))
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

/* the record's frame cleared, as CPython clears a generator's frame: a
   frame object that outlives it takes it; else its locals go, then its
   function. Its code stays for the record's life, as a generator keeps its
   own. */
static void
frame_clear(le_frame_t *f)
{
    PyFrameObject *fo = f->frame_obj;
    PyObject *o;
    if (fo != NULL) {
        if (Py_REFCNT(fo) > 1) {
            frame_give(fo, f, true);
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
    le_frame_t *f = &r->frame;
    PyCodeObject *co = LE_FRAME_CODE(f);
    le_instr_t in;
    if (le_monitoring_on(PyThreadState_GET()) || le_code_monitored(co))
        return false;
    in = le_instr(le_code(co),
                  (int)((char *)LE_FRAME_PLACE(f) - (char *)_PyCode_CODE(co)));
    return in.op == RESUME && (in.arg & RESUME_OPARG_DEPTH1_MASK) != 0;
}
#endif

static PyObject *
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

DEFINEFN PyObject *
le_gen_record_real(PyObject *rec)
{
    le_gen_record_t *r = (le_gen_record_t *)rec;
    if (r->escaped != NULL)
        return Py_NewRef(r->escaped);
    if (r->state >= FRAME_CLEARED) {
        /* ended, its function let go: only the word that held it until
           END_SEND pops it can still see it, which nothing reads */
        return Py_NewRef(Py_None);
    }
    LE_STAT(le_stat_gen_records[1]++);
    return record_escape(r, frame_func(&r->frame), r->state);
}

/* The real generator made from the record 'r', in the state 'state': a
   frame object for its frame (the one Python asked for, or a new one)
   takes the frame, and the generator is made around it. The record holds
   only the generator from then on. A new reference, or NULL. */
static PyObject *
record_escape(le_gen_record_t *r, PyFunctionObject *func, int8_t state)
{
    le_frame_t *f = &r->frame;
    _Py_CODEUNIT *at = LE_FRAME_PLACE(f);
    PyFrameObject *fo;
    PyGenObject *gen;
    if (words_escape(f) < 0)
        return NULL;
    /* complete for the while, or no frame object is made for it */
    if (!le_frame_complete(f))
        le_frame_set_lasti(f, 0);
    fo = le_frame_object(f);
    LE_FRAME_PLACE(f) = at;
    if (fo == NULL) {
        if (!PyErr_Occurred())
            PyErr_NoMemory();
        return NULL;
    }
    Py_INCREF(fo);
    frame_give(fo, f, false);
    Py_DECREF(fo); /* the frame's reference */
    gen = (PyGenObject *)le_gen_of_frame(fo, func);
    if (gen == NULL) {
        Py_DECREF(fo);
        r->state = FRAME_CLEARED;
        return NULL;
    }
    /* the frame object's own frame is a stale copy now, which a frame
       that ends with its stack emptied is copied over only in part */
    {
        le_frame_t *own = (le_frame_t *)fo->_f_frame_data;
        int k, n = LE_FRAME_CODE(own)->co_nlocalsplus +
                   LE_FRAME_CODE(own)->co_stacksize;
        for (k = 0; k < n; k++) le_frame_word_put(own, k, NULL);
    }
    gen->gi_frame_state = state;
    gen->gi_exc_state.exc_value = r->exc_state.exc_value;
    r->exc_state.exc_value = NULL;
    r->state = FRAME_CLEARED;
    r->escaped = Py_NewRef(gen);
    return (PyObject *)gen;
}

DEFINEFN PyObject *
le_gen_record_escape(PyObject *rec, PyObject *func)
{
    le_gen_record_t *r = (le_gen_record_t *)rec;
    PyObject *gen;
    if (r->escaped != NULL)
        return Py_NewRef(r->escaped);
    if (r->state < FRAME_CLEARED) {
        LE_STAT(le_stat_gen_records[1]++);
        return record_escape(r, (PyFunctionObject *)func, r->state);
    }
    LE_STAT(le_stat_gen_records[4]++);
    /* ended: a generator of the same function, ended too, its words long
       freed */
#if LE_HAS_FRAME_STACKREFS
    r->frame.f_funcobj = PyStackRef_FromPyObjectNew(func);
#elif LE_HAS_FRAME_FUNCOBJ
    r->frame.f_funcobj = Py_NewRef(func);
#else
    r->frame.f_func = (PyFunctionObject *)Py_NewRef(func);
#endif
    gen = record_escape(r, (PyFunctionObject *)func, LE_GEN_ENDED_STATE);
    return gen;
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
    PyThreadState *ts = PyThreadState_GET();
    _PyErr_StackItem *previous = r->exc_state.previous_item, **link;
    le_frame_t *caller = f->previous;
    int hidden = le_frame_stack_depth(f) < 0;
    PyGenObject *gen;
    if (r->escaped != NULL)
        return (PyGenObject *)r->escaped;
    LE_STAT(le_stat_gen_records[3]++);
    gen = (PyGenObject *)record_escape(r, frame_func(f), FRAME_EXECUTING);
    if (gen == NULL)
        return NULL;
    /* its stack still compiled code's, as it was the record's */
    if (hidden)
        le_gen_frame_set_depth(
            (le_frame_t *)((char *)gen + offsetof(PyGenObject, gi_iframe)),
            -1 - LE_FRAME_CODE(f)->co_nlocalsplus);
    Py_DECREF(gen); /* the record's */
    /* running, as gen_send_ex2 leaves it: its exception state where the
       record's was in the thread's chain */
    gen->gi_exc_state.previous_item = previous;
    r->exc_state.previous_item = NULL;
    for (link = &ts->exc_info; *link != NULL && *link != &r->exc_state;
         link = &(*link)->previous_item);
    if (*link != NULL)
        *link = &gen->gi_exc_state;
    ((le_frame_t *)((char *)gen + offsetof(PyGenObject, gi_iframe)))
        ->previous = caller;
    return gen;
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
    if (r->escaped != NULL) {
        /* made real while it ran: ended as a generator's run */
        PyObject *gen = r->escaped;
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
    le_gen_frame_end(&r->frame, result);
    ts->exc_info = r->exc_state.previous_item;
    r->exc_state.previous_item = NULL;
    if (LE_GEN_PAUSED(r->state)) {
        r->frame.previous = NULL;
        *paused = 1;
        return result;
    }
#if LE_HAS_FINISHED_GENERATOR_UNLINKED
    r->frame.previous = NULL;
#endif
#if !LE_HAS_OPCODE_CALL_INTRINSIC_1
    /* PEP 479, as gen_send_ex2 does it once the frame is left */
    if (result == NULL && PyErr_ExceptionMatches(PyExc_StopIteration))
        _PyErr_FormatFromCause(PyExc_RuntimeError, "%s",
                               LE_MSG_GENERATOR_STOP_ITERATION);
#endif
    r->state = FRAME_CLEARED;
#if LE_HAS_EVAL_FRAME_POPS
    /* ceval's clear_gen_frame */
    frame_clear(&r->frame);
    Py_CLEAR(r->exc_state.exc_value);
#else
    /* gen_send_ex2's */
    Py_CLEAR(r->exc_state.exc_value);
    frame_clear(&r->frame);
#endif
    r->frame.previous = NULL;
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
   escapes and the generator is dropped, which closes it. */
static void
record_dealloc(PyObject *self)
{
    le_gen_record_t *r = (le_gen_record_t *)self;
    PyObject *code;
    if (r->escaped == NULL && r->state != FRAME_CLEARED) {
        if (r->state == FRAME_CREATED) {
            r->state = FRAME_CLEARED;
            frame_clear(&r->frame);
        }
#if LE_HAS_GEN_CLOSE_UNRUN
        else if (record_closes_unrun(r)) {
            /* as CPython's close, then its dealloc */
            LE_STAT(le_stat_gen_records[5]++);
            if (LE_HAS_GEN_CLOSE_LOCALS_ONLY)
                frame_clear_locals(&r->frame);
            r->state = FRAME_CLEARED;
            frame_clear(&r->frame);
            Py_CLEAR(r->exc_state.exc_value);
        }
#endif
        else {
            PyObject *t, *v, *tb;
            PyErr_Fetch(&t, &v, &tb);
            LE_STAT(le_stat_gen_records[2]++);
            if (record_escape(r, frame_func(&r->frame), r->state) == NULL)
                PyErr_WriteUnraisable(self);
            else
                Py_DECREF(r->escaped); /* the escape's reference */
            PyErr_Restore(t, v, tb);
        }
    }
    Py_CLEAR(r->escaped);
#if LE_HAS_FRAME_STACKREFS
    code = PyStackRef_AsPyObjectSteal(r->frame.f_executable);
#elif LE_HAS_FRAME_EXECUTABLE
    code = r->frame.f_executable;
#else
    code = (PyObject *)r->frame.f_code;
#endif
    Py_XDECREF(code);
    PyObject_Free(r);
}

PyTypeObject le_gen_record_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "generator record",
    .tp_basicsize = offsetof(le_gen_record_t, frame),
    .tp_dealloc = record_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

/* the unit of RETURN_GENERATOR in the code of a generator function, -1 if
   it has none or something before it a record's start doesn't do
   (le_gen_record_start), such as the iterator a generator expression
   takes before it (GET_ITER). The record's type is made ready here, when
   code that makes records is first compiled, not before. */
DEFINEFN long
le_gen_record_start_unit(PyCodeObject *co)
{
    le_code_t c = le_code(co);
    int pos;
    if (!(le_gen_record_type.tp_flags & Py_TPFLAGS_READY) &&
        PyType_Ready(&le_gen_record_type) < 0) {
        PyErr_Clear();
        return -1;
    }
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        if (in.op == RETURN_GENERATOR)
            return pos / LE_UNIT;
        if (in.op != MAKE_CELL && in.op != COPY_FREE_VARS && in.op != RESUME &&
            in.op != NOP)
            return -1;
        pos = in.next;
    }
    return -1;
}
#endif
