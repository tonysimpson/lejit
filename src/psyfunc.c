/*
 * Compile-time Python-to-Python calls (psyco_call_pyfunc). When the callee and
 * its argument count are known at compile time, the arguments are bound now,
 * by the binder translated from ceval.c (below), which owns a
 * reference to each value it binds; a binding it can't do is the real call.
 * The callee is then either inlined into the caller (the EInline
 * pseudo-exception swaps po->vlocals) or compiled separately and called with
 * the Psyco calling convention (psyco_call_psyco). Closure code is called this
 * way too: the closure's cells are passed as extra inputs into the
 * free-variable slots, where LOAD_DEREF reads them as the borrowed cells of
 * a frame. They are the virtual closure tuple's items while the function is
 * still the one MAKE_FUNCTION made (comprehensions, local lambdas),
 * constants read now for a constant function (a method, a decorated
 * global), or loaded at run time from func_closure (psyco_call_closure).
 * Anything else (keyword-only arguments, code with cells of its own,
 * unknown arity, recursion depth used up) becomes a residual call of the
 * function object, as does a call made while a trace or profile function is
 * set (tested at run time: tracing_on).
 */
#include "psyfunc.h"
#include "Objects/pfuncobject.h"
#include "Objects/ptupleobject.h"
#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "codemanager.h"
#include "mergepoints.h"
#include "pycodegen.h"
#include "vcompiler.h"
/* le_tracing_test */
#include "mechanisms/tracing.h"
/* le_tstate_load and the recursion count */
#include "mechanisms/tstate.h"
#include "mechanisms/recursion.h"
/* ceval's eval_breaker is a 32-bit int at an address fixed for the
   process: compiled code compares it with 0 in place. The
   address and the handler are Python/pyruntime.c's. */
extern void *
le_eval_breaker_address(void);
extern int
le_eval_breaker_handler(void);

static inline condition_code_t
le_eval_breaker_test(PsycoObject *po)
{
    return psyco_int32_nonzero_at(po, le_eval_breaker_address());
}

/***************************************************************/
/***   Frame and arguments building                            ***/
/***************************************************************/

static void
fix_run_time_args(PsycoObject *po, vinfo_array_t *target,
                  vinfo_array_t *source, RunTimeSource *sources)
{
    int i = target->count;
    extra_assert(target->count <= source->count);
    while (i--) {
        vinfo_t *a = source->items[i];
        if (a != NULL && a->tmp != NULL) {
            vinfo_t *b = a->tmp;
            if (is_runtime(a->source)) {
                if (target->items[i] == NULL)
                    continue; /* item was removed by psyco_simplify_array() */
                if (sources != NULL) {
                    int argc =
                        (po->stack_depth - INITIAL_STACK_DEPTH) / sizeof(long);
                    sources[argc] = a->source;
                }
                po->stack_depth += sizeof(long);
                /* arguments get borrowed references */
                b->source = RunTime_NewStack(po->stack_depth, false, false);
            }
            extra_assert(b == target->items[i]);
            a->tmp = NULL;
            if (a->array != NullArray)
                fix_run_time_args(po, b->array, a->array, sources);
        }
    }
}

struct fncall_arg_s {
    PyCodeObject *co;
    PyObject *merge_points;
    /* the callee frame's vlocals up to its stack: globals, builtins, the
       bound locals (a known NULL where unbound) and the free cells, then any
       extra slots; each item owned */
    vinfo_array_t *inputvinfos;
    int nfrees;
    int po_size;
};

/* What LeJIT's frames support, tested before anything is emitted: no
   **kwargs dict and no cells of the code's own (a frame doesn't make them
   at entry; the binder would refuse both later, after the closure's cells
   were loaded), and free variables only when the caller has the cells
   (psyco_call_pyfunc). */
static bool
fncall_init(struct fncall_arg_s *fncall, PyCodeObject *co, int nfrees)
{
    if (co->co_flags & CO_VARKEYWORDS) {
        return false;
    }
    if (le_code_ncells(co) != 0 || le_code_nfrees(co) != nfrees) {
        return false;
    }
    fncall->co = co;
    fncall->nfrees = nfrees;
    fncall->merge_points = psyco_get_merge_points(co, 0);
    return fncall->merge_points != Py_None;
}

/* the arguments' part of ceval.c:_PyEval_EvalCodeWithName: -1 where it
   needs the real call */
DEFINEFN
int
_PsycoEval_EvalCodeWithName(PsycoObject *po, PyObject *_co,
                            vinfo_t **f_localsplus, vinfo_t **args,
                            Py_ssize_t argcount, vinfo_t **defs,
                            Py_ssize_t defcount, vinfo_t **closure)
{
    PyCodeObject *co = (PyCodeObject *)_co;
    Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount;
    vinfo_t **fastlocals = f_localsplus;
    vinfo_t **freevars = f_localsplus + le_code_nlocals(co);
    Py_ssize_t i, n, m, missing;
    vinfo_t *x;

    if (co->co_flags & CO_VARKEYWORDS) {
        return -1; /* PyDict_New: the real call */
    }
    n = argcount > co->co_argcount ? co->co_argcount : argcount;
    for (i = 0; i < n; i++) {
        x = args[i];
        vinfo_incref(x);
        vinfo_xdecref(fastlocals[i], po);
        fastlocals[i] = x;
    }
    if (co->co_flags & CO_VARARGS) {
#if LE_HAS_FRAME_BUILTINS_OF_FUNCTION
        return -1; /* the tuple: the real call */
#else
        vinfo_t *u;
        if (argcount - n > 15)
            return -1; /* a PyTuple_Type of more than CT_TUPLE_LOAD_SIZE_LIMIT
                          items: the real call */
        u = PsycoTuple_New(argcount - n, NULL);
        vinfo_xdecref(fastlocals[total_args], po);
        fastlocals[total_args] = u;
        for (i = n; i < argcount; i++) {
            x = args[i];
            vinfo_incref(x);
            PsycoTuple_GET_ITEM(u, i - n) = x;
        }
#endif
    }
    if (argcount > co->co_argcount && !(co->co_flags & CO_VARARGS)) {
        return -1; /* too_many_positional: the real call */
    }
    if (argcount < co->co_argcount) {
        m = co->co_argcount - defcount;
        missing = 0;
        for (i = argcount; i < m; i++) {
            if (fastlocals[i] == NULL) {
                missing++;
            }
        }
        if (missing) {
            return -1; /* missing_arguments: the real call */
        }
        for (i = n > m ? n - m : 0; i < defcount; i++) {
            if (fastlocals[m + i] == NULL) {
                x = defs[i];
                vinfo_incref(x);
                fastlocals[m + i] = x;
            }
        }
    }
    if (co->co_kwonlyargcount > 0) {
        for (i = co->co_argcount; i < total_args; i++) {
            if (fastlocals[i] == NULL) {
                return -1; /* missing_arguments: the real call */
            }
        }
    }
    if (le_code_ncells(co) > 0) {
        return -1; /* PyCell_New: the real call */
    }
    for (i = 0; i < le_code_nfrees(co); i++) {
        x = closure[i];
        vinfo_incref(x);
        freevars[i] = x;
    }
    return 0;
}

/* The callee frame's locals: the call's arguments, defaults and cells
   bound as _PyEval_EvalCodeWithName binds them (the translated binder), and
   the 'nextra' values of 'extra' after the cells. False when that binding
   needs the real call: a wrong count (whose TypeError's wording CPython's
   own call gives), or more extra positional arguments than a virtual
   tuple holds. */
static bool
fncall_bind(struct fncall_arg_s *fncall, PsycoObject *po, vinfo_t *vglobals,
            vinfo_t *vbuiltins, vinfo_t **argarray, int argcount,
            vinfo_t **defarray, int defcount, vinfo_t **frees, int nextra,
            vinfo_t **extra)
{
    PyCodeObject *co = fncall->co;
    int i, nlocals = le_code_nlocals(co), nfrees = fncall->nfrees;
    vinfo_array_t *a =
        array_new(INDEX_LOC_LOCALS_PLUS + nlocals + nfrees + nextra);
    vinfo_t **locals = a->items + INDEX_LOC_LOCALS_PLUS;

    vinfo_incref(vglobals);
    a->items[INDEX_LOC_GLOBALS] = vglobals;
    vinfo_incref(vbuiltins);
    a->items[INDEX_LOC_BUILTINS] = vbuiltins;
    if (_PsycoEval_EvalCodeWithName(po, (PyObject *)co, locals, argarray,
                                    argcount, defarray, defcount, frees) < 0) {
        array_delete(a, po);
        return false;
    }
    /* LeJIT's unbound local is a known NULL, as LOAD_FAST tests it */
    for (i = 0; i < nlocals; i++)
        if (locals[i] == NULL)
            locals[i] = psyco_vi_Zero();
    for (i = 0; i < nextra; i++) {
        vinfo_incref(extra[i]);
        locals[nlocals + nfrees + i] = extra[i];
    }
    fncall->inputvinfos = a;
    fncall->po_size =
        INDEX_LOC_LOCALS_PLUS + co->co_stacksize + nlocals + nfrees + nextra;
    return true;
}

PSY_INLINE void
fncall_finish_store(struct fncall_arg_s *fncall, PsycoObject *po)
{
    array_release(fncall->inputvinfos);

    /* store the code object */
    /* borrowed: the code object's record owns its compiled code, so an
       owned reference here would be a cycle through the code object.
       Whenever this code can run, something else holds the code: the frame
       the eval hook was given, or the caller's promotion cache, which keeps
       every function it switched on */
    po->pr.co = fncall->co;
    pyc_data_build(po, fncall->merge_points);
}

/* moves the bound locals into the frame's vlocals (the array keeps only
   its storage, for array_release); the rest of vlocals is the currently
   empty stack, NULL from array_new() */
static void
fncall_store_arguments(struct fncall_arg_s *fncall, vinfo_t **vlocals)
{
    vinfo_array_t *a = fncall->inputvinfos;
    memcpy(vlocals, a->items, a->count * sizeof(vinfo_t *));
}

/* Build a PsycoObject "frame" corresponding to the call of a Python
   function.  If 'sources!=NULL', it is set to an array of the sources of
   the values that must be pushed to make the call. The bound locals are
   the caller's values, released in 'caller' once copied. */
static PsycoObject *
psyco_build_frame(PsycoObject *caller, struct fncall_arg_s *fncall,
                  int recursion, RunTimeSource **sources)
{
    /* build a "frame" in a PsycoObject according to the given code object. */
    vinfo_array_t *arraycopy;
    PsycoObject *po;
    int rtcount;
    RunTimeSource *source1;

    po = PsycoObject_New(fncall->po_size);
    po->stack_depth = INITIAL_STACK_DEPTH;
    po->vlocals.count = fncall->po_size;
    INIT_PROCESSOR_PSYCOOBJECT(po);
    po->pr.auto_recursion = AUTO_RECURSION(recursion);

    /* duplicate the inputvinfos. If two arguments share some common part, they
       will also share it in the copy. */
    clear_tmp_marks(fncall->inputvinfos);
    arraycopy = array_new(fncall->inputvinfos->count);
    duplicate_array(arraycopy, fncall->inputvinfos);

    /* simplify arraycopy in the sense of psyco_simplify_array() */
    rtcount = psyco_simplify_array(arraycopy, NULL);

    /* all run-time arguments or argument parts must be corrected: in the
       input vinfo_t's they have arbitrary sources, but in the new frame's
       sources they will have to be fetched from the machine stack, where
       the caller will have pushed them. */
    if (sources != NULL) {
        source1 = PyMem_NEW(RunTimeSource, rtcount);
        if (source1 == NULL && rtcount > 0)
            OUT_OF_MEMORY();
        *sources = source1;
    }
    else
        source1 = NULL;
    fix_run_time_args(po, arraycopy, fncall->inputvinfos, source1);
    array_delete(fncall->inputvinfos, caller);
    fncall->inputvinfos = arraycopy;

    /* initialize po->vlocals */
    fncall_store_arguments(fncall, po->vlocals.items);
    fncall_finish_store(fncall, po);

    /* set up the CALL return address */
    po->stack_depth += sizeof(long);
    LOC_CONTINUATION =
        vinfo_new(RunTime_NewStack(po->stack_depth, false, false));
    return po;
}

static vinfo_t *
call_with_inline_frame(PsycoObject *po, struct fncall_arg_s *fncall,
                       int recursion)
{
    /* note: 'recursion' is ignored */
    vinfo_t *vresult;
    if (LOC_INLINING != NULL) /* we have the result ready from the
                                 initial case below */
    {
        vresult = LOC_INLINING;
        LOC_INLINING = NULL;
    }
    else {
        /* initial case: move the bound locals into an EInline
           pseudo-exception */
        vinfo_t *v;
        if (!psyco_catcher_flush(po)) {
            array_delete(fncall->inputvinfos, po);
            return NULL;
        }
        v = vinfo_new(SOURCE_NOT_IMPORTANT);
        v->array = array_new(TOTAL_LOC_INLINE + fncall->po_size);
        v->array->items[LOC_INLINE_CO] = vinfo_new(
            CompileTime_New((long)fncall->co)); /* borrowed, as above */
        fncall_store_arguments(fncall, v->array->items + TOTAL_LOC_INLINE);
        array_release(fncall->inputvinfos);
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EInline)), v);
        return NULL;
    }
    array_delete(fncall->inputvinfos, po);
    return vresult;
}

DEFINEFN
vinfo_t *
psyco_save_inline_po(PsycoObject *po)
{
    le_code_extra_t *x;
    int i = po->vlocals.count;
    vinfo_t **pp = po->vlocals.items;
    vinfo_t *v = vinfo_new(VirtualTime_New(&EInline));
    v->array = array_new(TOTAL_LOC_INLINE + i);
    /* fixed: the return reads them at compile time, so a match must never
       make them run-time (a join point in the inlined frame) */
    v->array->items[LOC_INLINE_CO] =
        vinfo_new(CompileTime_NewSk(sk_new((long)po->pr.co, SkFlagFixed)));
    v->array->items[LOC_INLINE_NEXT_INSTR] =
        vinfo_new(CompileTime_NewSk(sk_new(po->pr.next_instr, SkFlagFixed)));
    /* The caller's code object is borrowed, so once it dies another can take
       its address. Blocks compiled inside the inlined frame stay in the
       callee's join points (vcompiler.c:psyco_compile_join), and their code
       goes on in the dead caller's: the serial keeps a new caller from
       matching them. */
    x = le_code_extra(po->pr.co);
    psyco_assert(x != NULL); /* made with the merge points */
    v->array->items[LOC_INLINE_SERIAL] =
        vinfo_new(CompileTime_NewSk(sk_new(x->serial, SkFlagFixed)));
    while (i--) {
        v->array->items[TOTAL_LOC_INLINE + i] = pp[i];
        pp[i] = NULL;
    }
    return v;
}

DEFINEFN
PsycoObject *
psyco_restore_inline_po(PsycoObject *po, vinfo_array_t **a)
{
    int i, oldcount = po->vlocals.count;
    bool was_inlining = po->pr.is_inlining;
    vinfo_t *v;
    vinfo_t **old;
    vinfo_array_t *array = *a;
    *a = NullArray;

    /* the current locals are dropped only once the restored frame is in
       place: a decref may call a dealloc, and C calls are aligned from the
       current frame's return address (STACK_DEPTH_SINCE_CALL), which the
       inlined frame being left doesn't have */
    old = PyMem_Malloc((oldcount + 1) * sizeof(vinfo_t *));
    if (old == NULL)
        OUT_OF_MEMORY();
    memcpy(old, po->vlocals.items, oldcount * sizeof(vinfo_t *));

    i = array->count - TOTAL_LOC_INLINE;
    po = PsycoObject_Resize(po, i);
    po->vlocals.count =
        i; /* attention, vlocals.count cannot be larger than i
              even if there is enough memory already allocated */
    while (i--) po->vlocals.items[i] = array->items[TOTAL_LOC_INLINE + i];

    v = array->items[LOC_INLINE_CO];
    po->pr.co = (PyCodeObject *)CompileTime_Get(v->source)->value;
    vinfo_decref(v, NULL);
    v = array->items[LOC_INLINE_NEXT_INSTR];
    po->pr.next_instr = v ? CompileTime_Get(v->source)->value : 0;
    vinfo_xdecref(v, NULL);
    vinfo_xdecref(array->items[LOC_INLINE_SERIAL], NULL);
    array_release(array);

    pyc_data_build(po, psyco_get_merge_points(po->pr.co, -1));

    /* leaving an inlined frame restores its (never inlining) parent; on
       entry the old locals were already moved out (psyco_save_inline_po) */
    po->pr.is_inlining = false;
    for (i = oldcount; i--;) vinfo_xdecref(old[i], po);
    po->pr.is_inlining = was_inlining;
    PyMem_Free(old);
    return po;
}

/***************************************************************/
/***   Recursion depth of compiled calls                       ***/
/***************************************************************/

/* the running thread's state, read at run time (mechanisms/tstate.h) */
static vinfo_t *
load_tstate(PsycoObject *po)
{
    return le_tstate_load(po);
}

/* Py_LeaveRecursiveCall after a compiled call; entering
   is mechanisms/recursion.h's enter_recursive_call, on a thread state the
   caller has loaded */
DEFINEFN
bool
psyco_leave_recursive_call(PsycoObject *po)
{
    vinfo_t *vts = load_tstate(po);
    if (vts == NULL)
        return false;
    return leave_recursive_call(po, vts);
}

/* ceval's test of eval_breaker before an instruction:
   le_eval_breaker_test, inline, and only when it is set a call
   to the C helper that does what ceval does then (pending calls and signal
   handlers, a GIL drop for other threads, an async exception). The helper
   can run any Python code, as any residual call can. Its branch is a
   runtime_condition_f respawn point, compiled when first taken. */
DEFINEFN
bool
psyco_check_eval_breaker(PsycoObject *po)
{
    condition_code_t cc = le_eval_breaker_test(po);
    if (runtime_condition_f(po, cc))
        return psyco_generic_call(po, le_eval_breaker_handler,
                                  CfCommonIntZeroOk, "") != NULL;
    return true;
}

/* The C functions after which a CALL_* needs no eval_breaker test
   (pycompiler.c:breaker_after_call). Each runs briefly, can't block or
   release the GIL, and runs only CPython's own code for builtin types, so
   what arrives during it waits only until the next test, as it would
   during any other instruction. Allocation can collect garbage and so run
   finalisers, as at any allocation in compiled code; the str methods can
   call an argument's __index__. A Python function's tp_call runs Python
   code, which tests eval_breaker itself (ceval, or compiled code entered
   through LeCode_Run), as an inlined or directly compiled callee does.
   Left out, though common at call sites: whatever can call back into
   arbitrary objects (dict lookups, PyObject_IsTrue, getattr, isinstance,
   a class's tp_call, list.extend, str.join), since that may be C code
   that blocks. Filled at start-up. */
#define CANNOT_BLOCK_MAX 40
static void *cannot_block[CANNOT_BLOCK_MAX];
static int cannot_block_count = 0;

DEFINEFN
bool
psyco_cannot_block(void *c_function)
{
    int i;
    for (i = 0; i < cannot_block_count; i++)
        if (cannot_block[i] == c_function)
            return true;
    return false;
}

static void
add_cannot_block(void *c_function)
{
    psyco_assert(c_function != NULL && cannot_block_count < CANNOT_BLOCK_MAX);
    cannot_block[cannot_block_count++] = c_function;
}

/* by ml_meth: a builtin type's method, or a builtin function (below) */
static void
add_method(PyTypeObject *tp, const char *name)
{
    PyObject *d = PyDict_GetItemString(le_type_dict(tp), name);
    psyco_assert(d != NULL && Py_TYPE(d) == &PyMethodDescr_Type);
    add_cannot_block(((PyMethodDescrObject *)d)->d_method->ml_meth);
}

static void
add_builtin(PyObject *builtins, const char *name)
{
    PyObject *f = PyObject_GetAttrString(builtins, name);
    psyco_assert(f != NULL && PyCFunction_Check(f));
    add_cannot_block(((PyCFunctionObject *)f)->m_ml->ml_meth);
    Py_DECREF(f);
}

static void
cannot_block_init(void)
{
    static const char *const str_methods[] = {
        "find",     "rfind",     "index",      "rindex",  "startswith",
        "endswith", "partition", "rpartition", "lower",   "upper",
        "strip",    "lstrip",    "rstrip",     "replace", "split",
        "isdigit",  "isalpha",   "isspace",    NULL};
    const char *const *m;
    PyObject *builtins = PyImport_ImportModule("builtins");
    psyco_assert(builtins != NULL);
    add_cannot_block(PyTuple_New);
    add_cannot_block(PyList_New);
    add_cannot_block(PyDict_New);
    add_cannot_block(_PyDict_NewPresized);
    add_cannot_block(PyLong_FromLong);
    add_cannot_block(PyFloat_FromDouble);
    add_cannot_block(PyFunction_NewWithQualName);
    add_cannot_block(PyMethod_New);
    add_cannot_block(PyThreadState_Get);
    add_cannot_block(PyLong_Type.tp_richcompare);
    add_cannot_block(PyFloat_Type.tp_richcompare);
    add_cannot_block(le_function_tp_call());
    add_method(&PyList_Type, "append");
    add_method(&PyList_Type, "reverse");
    add_method(&PyDict_Type, "keys");
    add_method(&PyDict_Type, "items");
    add_method(&PyDict_Type, "values");
    for (m = str_methods; *m != NULL; m++) add_method(&PyUnicode_Type, *m);
    add_builtin(builtins, "ord");
    add_builtin(builtins, "chr");
    add_builtin(builtins, "callable");
    Py_DECREF(builtins);
}

/* Whether a trace or profile function is set in this thread: a
   compiled call or inlined frame skips the eval hook, which runs frames
   interpreted while the thread is tracing, so each one tests that first
   (le_tracing_test, mechanisms/tracing.h). When set, the
   caller makes the real call instead (rare path: a runtime_condition_f
   respawn point, compiled when first taken). A frame already running
   compiled code when tracing starts goes on interpreted after its next
   call (pycompiler.c:resume_if_tracing). use_tracing is per thread and
   set by every way in, C ones too
   (PyEval_SetTrace and PyEval_SetProfile, as cProfile uses). */
static bool
tracing_on(PsycoObject *po, vinfo_t *vts)
{
    return runtime_condition_f(po, le_tracing_test(po, vts));
}

DEFINEFN
int
psyco_tracing_on(PsycoObject *po)
{
    bool on;
    vinfo_t *vts;
    vts = load_tstate(po);
    if (vts == NULL)
        return -1;
    on = tracing_on(po, vts);
    vinfo_decref(vts, po);
    return on;
}

#define COMPUTE_DEFCOUNT()                                       \
    do {                                                         \
        /* is vdefaults!=NULL at run-time ? */                   \
        condition_code_t cc = object_non_null(po, vdefaults);    \
        if (cc == CC_ERROR) /* error or more likely promotion */ \
            return NULL;                                         \
        if (runtime_condition_t(po, cc))                         \
            defcount = PsycoTuple_Load(vdefaults);               \
        else                                                     \
            defcount = 0; /* vdefaults==NULL at run-time */      \
    } while (0)

/* The callee frame's builtins, as frameobject.c:_PyFrame_New_NoTrack picks
   them, in *out as a new reference: 1, or 0 if they aren't known now, or -1
   on an error. Where they are the function's (func_builtins, set when it
   was made): those of a function known now, the caller's for one this frame
   made, and for a closure known only at run time (psyco_call_closure, which
   tested that its globals are this frame's) the caller's after a run-time
   test that they are the same, the call being the real one where not. Else
   the caller's when the globals are the same, or the globals'
   __builtins__. The globals' are read at compile time, so a module's
   __builtins__ replaced after a call site compiled is not seen there. */
static bool
same_globals(PsycoObject *po, vinfo_t *vglobals)
{
    return vglobals == LOC_GLOBALS ||
           (is_compiletime(vglobals->source) &&
            is_compiletime(LOC_GLOBALS->source) &&
            CompileTime_Get(vglobals->source)->value ==
                CompileTime_Get(LOC_GLOBALS->source)->value);
}

static int
callee_builtins(PsycoObject *po, vinfo_t *vfunc, vinfo_t *vglobals,
                vinfo_t **out)
{
    PyObject *b;
#if LE_HAS_FRAME_BUILTINS_OF_FUNCTION
    if (vfunc == NULL)
        return 0;
    if (is_compiletime(vfunc->source)) {
        b = ((PyFunctionObject *)CompileTime_Get(vfunc->source)->value)
                ->func_builtins;
        if (is_compiletime(LOC_BUILTINS->source) &&
            (PyObject *)CompileTime_Get(LOC_BUILTINS->source)->value == b) {
            vinfo_incref(LOC_BUILTINS);
            *out = LOC_BUILTINS;
            return 1;
        }
        Py_INCREF(b); /* owned by the constant, as in PsycoObject_FromCode */
        *out = vinfo_new(CompileTime_NewSk(sk_new((long)b, SkFlagPyObj)));
        return 1;
    }
    if (!same_globals(po, vglobals))
        return 0;
    if (is_runtime(vfunc->source)) {
        condition_code_t cc;
        vinfo_t *vb = psyco_get_field_offset(
            po, vfunc,
            FMUT(DEF_FIELD(PyFunctionObject, PyObject *, func_builtins,
                           NO_PREV_FIELD)),
            0);
        if (vb == NULL)
            return -1;
        cc = integer_cmp(po, vb, LOC_BUILTINS, Py_NE);
        vinfo_decref(vb, po);
        if (cc == CC_ERROR)
            return -1;
        if (runtime_condition_f(po, cc))
            return 0;
    }
    vinfo_incref(LOC_BUILTINS);
    *out = LOC_BUILTINS;
    return 1;
#endif
    if (same_globals(po, vglobals)) {
        vinfo_incref(LOC_BUILTINS);
        *out = LOC_BUILTINS;
        return 1;
    }
    if (!is_compiletime(vglobals->source))
        return 0;
    b = psy_get_builtins((PyObject *)CompileTime_Get(vglobals->source)->value);
    Py_INCREF(b); /* owned by the constant, as in PsycoObject_FromCode */
    *out = vinfo_new(CompileTime_NewSk(sk_new((long)b, SkFlagPyObj)));
    return 1;
}

#define MAX_CALL_FREES 64

static void
pin_constant(source_known_t *sk)
{
    if ((sk->refcount1_flags & (SkFlagPyObj | SkFlagBorrowed)) == SkFlagPyObj)
        psyco_owner_pin((PyObject *)sk->value);
}

/* the callee's owner holds every constant object it is given, and every
   type hinted: its code may use one after the caller's last reference has
   gone */
static void
pin_constants(vinfo_array_t *a)
{
    int i;
    for (i = a->count; i--;) {
        vinfo_t *v = a->items[i];
        if (v == NULL)
            continue;
        if (is_compiletime(v->source))
            pin_constant(CompileTime_Get(v->source));
        if (v->type_hint != NULL)
            pin_constant(v->type_hint);
        if (v->array != NullArray)
            pin_constants(v->array);
    }
}

/* Compile a direct call's callee, as its own owner (codemanager.h). The
   caller's code jumps into it, so the caller holds the callee's code
   object, which holds the compiled code. */
static CodeBufferObject *
compile_callee(PsycoObject *mypo, PyCodeObject *co)
{
    CodeBufferObject *codebuf;
    le_owner_t *callee = psyco_code_owner(co);
    psyco_assert(callee != NULL); /* made with the merge points */
    psyco_owner_pin((PyObject *)co);
    {
        LE_OWNER_ENTER(callee);
        pin_constants(&mypo->vlocals);
        codebuf = psyco_compile_code(mypo, PsycoObject_Ready(mypo));
        LE_OWNER_LEAVE();
    }
    return codebuf;
}

DEFINEFN
vinfo_t *
psyco_call_pyfunc(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                  vinfo_t *vglobals, vinfo_t *vdefaults, vinfo_t *arg_tuple,
                  int recursion, bool allow_inline)
{
    CodeBufferObject *codebuf;
    PsycoObject *mypo;
    Source *sources;
    vinfo_t *result;
    stack_frame_info_t *finfo;
    int tuple_size, argcount, defcount = -2;
    int nfrees;
    bool inlining;
    vinfo_t *vbuiltins;
    vinfo_t **frees = NULL;
    vinfo_t *freebuf[MAX_CALL_FREES];
    struct fncall_arg_s fncall;

    if (--recursion < 0)
        goto fail_to_default;
    /* a generator expression is run only by a consumer that knows it
       (psyco_call_genexpr); until then it is a pending generator */
    if (co->co_flags & CO_GENERATOR) {
        if (vfunc != NULL && is_virtualtime(vfunc->source) &&
            psyco_is_genexpr(co) && PsycoTuple_Load(arg_tuple) == 1)
            return PsycoGenexpr_New(vfunc, arg_tuple);
        goto fail_to_default;
    }
    /* closure code: the cells of a function still virtual (MAKE_FUNCTION
       here: a comprehension, say) are its closure tuple's items; those of
       any other are read from func_closure below */
    nfrees = PyCode_GetNumFree(co);
    if (nfrees != 0) {
        vinfo_t *closure;
        if (vfunc == NULL || nfrees > MAX_CALL_FREES)
            goto fail_to_default;
        if (is_virtualtime(vfunc->source)) {
            closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
            if (closure == NULL || PsycoTuple_Load(closure) != nfrees)
                goto fail_to_default;
            frees = &PsycoTuple_GET_ITEM(closure, 0);
        }
    }

    /* XXX tony: kw only args are a Py3 feature that we need to add support for
     */
    if (co->co_kwonlyargcount != 0) {
        goto fail_to_default;
    }

    tuple_size = PsycoTuple_Load(arg_tuple);
    if (tuple_size == -1)
        goto fail_to_default;
    /* XXX calling with an unknown-at-compile-time number of arguments
       is not implemented, revert to the default behaviour */

    COMPUTE_DEFCOUNT();
    if (defcount == -1)
        goto fail_to_default;
    /* calling with an unknown-at-compile-time number of default arguments
       is not implemented (but this is probably not useful to implement);
       revert to the default behaviour */

    /* Force mutable arguments out of virtual-time,
       including the processor condition codes */
    /* This is expected to have already been done on default arguments;
       see PsycoFunction_New() */
    if (!psyco_forking(po, arg_tuple->array))
        return NULL;

    /* prepare a frame */
    if (!fncall_init(&fncall, co, nfrees))
        goto fail_to_default; /* unsupported bytecode features */
    switch (callee_builtins(po, vfunc, vglobals, &vbuiltins)) {
        case 1:
            break;
        case 0:
            goto fail_to_default;
        default:
            return NULL;
    }
    if (nfrees != 0 && frees == NULL) {
        /* func_closure can't be set from Python, and its length is the code's
           number of free variables (funcobject.c:func_set_code checks it), as
           ceval assumes. A constant function's cells are constants read now:
           borrowed, since code compiled for a cell only ever uses its address,
           which a new cell at the same address may as well share. */
        int i;
        vinfo_t *closure = psyco_get_const(po, vfunc, FUNC_closure);
        for (i = 0; closure != NULL && i < nfrees; i++)
            if ((freebuf[i] = psyco_get_nth_const(po, closure, TUPLE_ob_item,
                                                  i)) == NULL)
                closure = NULL;
        if (closure == NULL) {
            vinfo_decref(vbuiltins, po);
            return NULL;
        }
        frees = freebuf;
    }
    if (!fncall_bind(&fncall, po, vglobals, vbuiltins,
                     &PsycoTuple_GET_ITEM(arg_tuple, 0), tuple_size,
                     &PsycoTuple_GET_ITEM(vdefaults, 0), defcount, frees, 0,
                     NULL)) {
        /* the wrong number of arguments: the real call raises CPython's
           TypeError, whose wording depends on the defaults and keyword-only
           arguments (ceval.c:too_many_positional, missing_arguments) */
        vinfo_decref(vbuiltins, po);
        goto fail_to_default;
    }

    /* not into itself: inlined calls aren't counted in the recursion depth,
       and a closure calling itself through its cell would
       reach RecursionError at twice CPython's depth */
    inlining = allow_inline && !po->pr.is_inlining && co != po->pr.co &&
               (psyco_mp_flags(fncall.merge_points) & MP_FLAGS_INLINABLE);
    if (inlining && frees == freebuf && is_runtime(vfunc->source)) {
        /* Cells loaded from a run-time function are borrowed from it, which is
           enough for a call: the caller holds the function until it returns a
           real object. An inlined body's result can still be virtual and hold
           a cell (a closure it made), and the caller drops the function before
           computing it: the cell could be freed first. So each
           cell gets a reference of its own, dropped with its last vinfo_t. */
        int i;
        for (i = 0; i < nfrees; i++) need_reference(po, freebuf[i]);
    }
    /* a traced call goes through the eval hook (tracing_on). Not again when
       the instruction restarts after the inlined body with its result. */
    if (!inlining || LOC_INLINING == NULL) {
        bool traced, entered = true;
        vinfo_t *vts = load_tstate(po);
        if (vts == NULL) {
            array_delete(fncall.inputvinfos, po);
            vinfo_decref(vbuiltins, po);
            return NULL;
        }
        traced = tracing_on(po, vts);
        /* count the call's depth, as ceval does on entering a frame, so that
           deep recursion raises RecursionError instead of overflowing the C
           stack; psyco_call_psyco leaves it after the call.
           Emitted before the frame is built, so no argument source moves. */
        if (!traced && !inlining)
            entered = enter_recursive_call(po, vts);
        vinfo_decref(vts, po);
        if (traced || !entered) {
            array_delete(fncall.inputvinfos, po);
            vinfo_decref(vbuiltins, po);
            if (!entered)
                return NULL;
            goto fail_to_default;
        }
    }

    /* try to inline the call */
    if (inlining) {
        /* inlining call: blocks compiled in the inlined frame are listed at
           the callee's join points until this owner dies (codemanager.c) */
        psyco_owner_pin((PyObject *)co);
        result = call_with_inline_frame(po, &fncall, recursion);
    }
    else {
        /* non-inlining call: the callee's entry compares the arguments' known
           types, after what this instruction called so far */
        Psyco_ForgetClasses(po);
        mypo = psyco_build_frame(po, &fncall, recursion, &sources);
        if (mypo == NULL) {
            psyco_leave_recursive_call(po);
            goto pyerr;
        }
        argcount = get_arguments_count(&mypo->vlocals);
        finfo = psyco_finfo(po, mypo);

        /* compile the function (this frees mypo) */
        codebuf = compile_callee(mypo, co);

        /* get the run-time argument sources and push them on the stack
           and write the actual CALL */
        result = psyco_call_psyco(po, codebuf, sources, argcount, finfo);
        PyMem_FREE(sources);
    }
    vinfo_decref(vbuiltins, po);
    return result;

fail_to_default:
    /* call the function object itself: rebuilding the call from its code,
       globals and defaults, as upstream's cimpl_call_pyfunc did, would lose
       its name and qualname (generators take co_name), closure and
       keyword-only defaults */
    return psyco_generic_call(po, le_function_tp_call(),
                              CfCommonNewRefPyObject, "vvl", vfunc, arg_tuple,
                              (long)0);

pyerr:
    psyco_virtualize_exception(po);
    return NULL;
}

/* A nested function (CO_NESTED) known only at run time, with its code 'co'
   promoted (nested functions are new function objects on every run of the
   code that makes them, but their code is not). Neither the function nor
   anything it holds is made a constant, which would keep each one alive and
   specialise per object: its cells are loaded from func_closure at each call.
   The globals must be this frame's (a run-time test), as for a closure made
   here, so that they and the builtins stay constants that no promotion holds.
   Defaults are per function object, so a call that would use them is the real
   one; keyword-only arguments never come here (pfunction_call). */
DEFINEFN
vinfo_t *
psyco_call_closure(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                   vinfo_t *arg_tuple)
{
    vinfo_t *vglobals, *vnodefaults, *result;
    condition_code_t cc;
    int n = PsycoTuple_Load(arg_tuple);
    if (n < co->co_argcount)
        goto fallback; /* unknown count, or defaults wanted */
    vglobals = psyco_get_const(po, vfunc, FUNC_globals);
    if (vglobals == NULL)
        return NULL;
    cc = integer_cmp(po, vglobals, LOC_GLOBALS, Py_NE);
    if (cc == CC_ERROR)
        return NULL;
    if (runtime_condition_f(po, cc))
        goto fallback;
    vnodefaults = psyco_vi_Zero();
    result = psyco_call_pyfunc(po, vfunc, co, LOC_GLOBALS, vnodefaults,
                               arg_tuple, po->pr.auto_recursion, true);
    vinfo_decref(vnodefaults, po);
    return result;

fallback:
    return psyco_generic_call(po, le_function_tp_call(),
                              CfCommonNewRefPyObject, "vvl", vfunc, arg_tuple,
                              (long)0);
}

/* Run a pending generator expression (PsycoGenexpr_New) for its consumer:
   its code compiled with 'mode' and 'acc' in the two GENEXPR slots
   (pycheader.h), called like a function of its iterator, returning the
   consumer's result as a new reference. */
DEFINEFN
vinfo_t *
psyco_call_genexpr(PsycoObject *po, vinfo_t *vgen, int mode, vinfo_t *acc)
{
    vinfo_t *vfunc = vgen->array->items[iGENEXPR_FUNC];
    vinfo_t *args = vgen->array->items[iGENEXPR_ARGS];
    vinfo_t *vcode, *vglobals, *vbuiltins, *closure, *vmode, *result, *vts;
    vinfo_t *extra[64];
    PyCodeObject *co;
    CodeBufferObject *codebuf;
    PsycoObject *mypo;
    Source *sources;
    stack_frame_info_t *finfo;
    int i, nfrees, argcount;
    struct fncall_arg_s fncall;

    vcode = vinfo_getitem(vfunc, iFUNC_CODE);
    vglobals = vinfo_getitem(vfunc, iFUNC_GLOBALS);
    extra_assert(vcode != NULL && is_compiletime(vcode->source));
    co = (PyCodeObject *)CompileTime_Get(vcode->source)->value;
    nfrees = PyCode_GetNumFree(co);
    if (vglobals == NULL || nfrees + GENEXPR_SLOTS > 64)
        return NULL; /* XXX */
    for (i = 0; i < nfrees; i++) {
        closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
        if (closure == NULL || PsycoTuple_Load(closure) != nfrees)
            psyco_fatal_msg("genexpr closure"); /* checked by the consumer */
        extra[i] = PsycoTuple_GET_ITEM(closure, i);
    }
    /* fixed: the fused code reads it at compile time (pycompiler.c), so a
       snapshot for another consumer must not match by un-promoting it */
    vmode = vinfo_new(CompileTime_NewSk(sk_new(mode, SkFlagFixed)));
    extra[nfrees] = vmode;
    extra[nfrees + 1] = acc;

    if (!psyco_forking(po, args->array) || !fncall_init(&fncall, co, nfrees))
        psyco_fatal_msg("genexpr call"); /* checked by the consumer */
    /* made by MAKE_FUNCTION here, so with this frame's globals and builtins */
    if (callee_builtins(po, vfunc, vglobals, &vbuiltins) != 1 ||
        !fncall_bind(&fncall, po, vglobals, vbuiltins,
                     &PsycoTuple_GET_ITEM(args, 0), 1, NULL, 0, extra,
                     GENEXPR_SLOTS, extra + nfrees))
        psyco_fatal_msg("genexpr arguments");

    /* as psyco_call_pyfunc's non-inlining call */
    result = NULL;
    vts = load_tstate(po);
    if (vts != NULL && enter_recursive_call(po, vts)) {
        vinfo_decref(vts, po);
        vts = NULL;
        Psyco_ForgetClasses(po);
        mypo = psyco_build_frame(po, &fncall, po->pr.auto_recursion, &sources);
        if (mypo == NULL) {
            psyco_leave_recursive_call(po);
            psyco_virtualize_exception(po);
        }
        else {
            argcount = get_arguments_count(&mypo->vlocals);
            finfo = psyco_finfo(po, mypo);
            codebuf = compile_callee(mypo, co);
            result = psyco_call_psyco(po, codebuf, sources, argcount, finfo);
            PyMem_FREE(sources);
        }
    }
    else
        array_delete(fncall.inputvinfos, po);
    vinfo_xdecref(vts, po);
    vinfo_decref(vmode, po);
    vinfo_decref(vbuiltins, po);
    return result;
}

/* whether psyco_call_genexpr can run 'v' */
DEFINEFN
bool
psyco_genexpr_fusable(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *vfunc, *vcode, *vglobals, *closure, *args, *vbuiltins;
    PyCodeObject *co;
    if (!PsycoGenexpr_Check(v))
        return false;
    /* only a temporary: the stack and the argument tuple hold it. A
       generator something else can still use must stay one object, which
       after a partial run (any, all, an exception) keeps its position, inner
       loops included */
    if (v->refcount > 2)
        return false;
    vfunc = v->array->items[iGENEXPR_FUNC];
    args = v->array->items[iGENEXPR_ARGS];
    if (args == NULL || PsycoTuple_Load(args) != 1)
        return false;
    vcode = vinfo_getitem(vfunc, iFUNC_CODE);
    if (vcode == NULL || !is_compiletime(vcode->source))
        return false;
    co = (PyCodeObject *)CompileTime_Get(vcode->source)->value;
    if (!psyco_is_genexpr(co) || psyco_get_merge_points(co, 0) == Py_None ||
        PyCode_GetNumFree(co) + GENEXPR_SLOTS > 64)
        return false;
    if (PyCode_GetNumFree(co) > 0) {
        closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
        if (closure == NULL ||
            PsycoTuple_Load(closure) != PyCode_GetNumFree(co))
            return false;
    }
    /* psyco_call_genexpr's builtins: of a function still virtual, so known
       now or not, with nothing emitted */
    vglobals = vinfo_getitem(vfunc, iFUNC_GLOBALS);
    if (vglobals == NULL ||
        callee_builtins(po, vfunc, vglobals, &vbuiltins) != 1)
        return false;
    vinfo_decref(vbuiltins, po);
    return true;
}

/* whether a type site in the fused code of 'v' (fusable) has seen too
   many types (mergepoints.h:psyco_fuse_stopped), tested at run time: read
   now, the flag could change between a compile and its replay */
static int
fuse_stopped(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *vcode = vinfo_getitem(v->array->items[iGENEXPR_FUNC], iFUNC_CODE);
    PyCodeObject *co = (PyCodeObject *)CompileTime_Get(vcode->source)->value;
    vinfo_t *vslot = vinfo_new(CompileTime_New(
        (long)psyco_fuse_stopped(psyco_get_merge_points(co, 0))));
    vinfo_t *vflag = psyco_get_field(po, vslot, INT_value);
    condition_code_t cc;
    vinfo_decref(vslot, po);
    if (vflag == NULL)
        return -1;
    cc = integer_non_null(po, vflag);
    vinfo_decref(vflag, po);
    if (cc == CC_ERROR)
        return -1;
    return runtime_condition_f(po, cc);
}

/* the fused body is the generator's frame: a tracer must see it */
DEFINEFN
int
psyco_genexpr_fuse(PsycoObject *po, vinfo_t *v)
{
    int on;
    if (!psyco_genexpr_fusable(po, v))
        return 0;
    on = psyco_tracing_on(po);
    if (on != 0)
        return on < 0 ? -1 : 0;
    on = fuse_stopped(po, v);
    return on < 0 ? -1 : !on;
}

DEFINEFN
void
psycofunction_init(void)
{
    psyco_assert(le_tstate_check());
    psyco_assert(le_eval_breaker_address() != NULL);
    cannot_block_init();
}
