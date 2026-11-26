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
 * The code's own cells are made by the binder, as CPython makes them for
 * the call, and are the callee frame's to free.
 * Keyword arguments are bound the same way, from the call's pending
 * keywords (pdictobject.c). Anything else (keyword-only arguments, a
 * **kwargs dict, unknown arity,
 * recursion depth used up) becomes a residual call of the function object,
 * as does a call made while a trace or profile function is set, or while
 * an eval hook other than LeJIT's is installed (tested at run time:
 * direct_call_refused).
 */
#include "psyfunc.h"
#include "Objects/pdictobject.h"
#include "Objects/pfuncobject.h"
#include "Objects/ptupleobject.h"
#include "Python/frames.h"
#include "Python/genrecord.h"
#include "Python/pycompiler.h"
#include "codemanager.h"
#include "mergepoints.h"
#include "bytecode.h"
#include "pycodegen.h"
#include "vcompiler.h"
#include "segments.h"
/* le_tracing_test */
#include "mechanisms/tracing.h"
/* le_tstate_load and the recursion count */
#include "mechanisms/tstate.h"
#include "mechanisms/recursion.h"
#include "mechanisms/frames.h"
extern int
le_eval_breaker_handler(void);
#if LE_HAS_TSTATE_EVAL_BREAKER
/* ceval's eval_breaker is the running thread's, in its state, and its
   events are its low byte (Python/pyruntime.c, with the handler):
   compiled code loads the thread state and tests that byte */
static inline condition_code_t
le_eval_breaker_test_on(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc;
    vinfo_t *vevents =
        psyco_get_field_offset(po, vts, FMUT(UNSIGNED_ARRAY(unsigned char, 0)),
                               offsetof(PyThreadState, eval_breaker));
    if (vevents == NULL)
        return CC_ERROR;
    cc = integer_non_null(po, vevents);
    vinfo_decref(vevents, po);
    return cc;
}

static inline condition_code_t
le_eval_breaker_test(PsycoObject *po)
{
    condition_code_t cc;
    vinfo_t *vts = le_tstate_load(po);
    if (vts == NULL)
        return CC_ERROR;
    cc = le_eval_breaker_test_on(po, vts);
    vinfo_decref(vts, po);
    return cc;
}

static inline bool
le_eval_breaker_check(void)
{
    return true;
}
#else
/* ceval's eval_breaker is a 32-bit int at an address fixed for the
   process: compiled code compares it with 0 in place. The
   address and the handler are Python/pyruntime.c's. */
extern void *
le_eval_breaker_address(void);

static inline condition_code_t
le_eval_breaker_test(PsycoObject *po)
{
    return psyco_int32_nonzero_at(po, le_eval_breaker_address());
}
/* the same: the thread state 'vts' isn't needed */
static inline condition_code_t
le_eval_breaker_test_on(PsycoObject *po, vinfo_t *vts)
{
    (void)vts;
    return le_eval_breaker_test(po);
}

/* at start-up */
static inline bool
le_eval_breaker_check(void)
{
    return le_eval_breaker_address() != NULL;
}
#endif

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
       bound locals (a known NULL where unbound), the code's own cells and
       the free cells, then any extra slots; each item owned */
    vinfo_array_t *inputvinfos;
    int po_size;
};

/* What LeJIT's frames support, tested before anything is emitted: free
   variables only when the caller has the cells (psyco_call_pyfunc). */
static bool
fncall_init(struct fncall_arg_s *fncall, PyCodeObject *co, int nfrees)
{
    if (le_code_nfrees(co) != nfrees) {
        return false;
    }
    fncall->co = co;
    fncall->merge_points = psyco_get_merge_points(co, 0);
    return fncall->merge_points != Py_None;
}

/* The frame's own cells, as CPython makes them for a call: a cell
   starts with its argument's value, if it is one, else empty. Where the
   cell has a slot of its own (ceval.c:_PyEval_EvalCodeWithName), the
   argument's slot is left unbound; where it is the argument's slot
   (MAKE_CELL), it holds the cell. With 'make' false, as when the bound
   locals are only checked again, no cell is made and each slot is a known
   NULL. False on an error. */
static bool
bind_cells(PsycoObject *po, PyCodeObject *co, vinfo_t **f_localsplus,
           bool make)
{
    int i;
    for (i = 0; i < le_code_ncells(co); i++) {
        int slot = le_code_cell_slot(co, i), arg = le_code_cell_arg(co, i);
        vinfo_t *start = arg >= 0 ? f_localsplus[arg] : NULL;
        vinfo_t *cell;
        if (!make)
            cell = psyco_vi_Zero();
        else if (start != NULL)
            cell = psyco_generic_call(po, PyCell_New,
                                      CfCommonNewRefPyObject | CfNoPython, "v",
                                      start);
        else
            cell = psyco_generic_call(po, PyCell_New,
                                      CfCommonNewRefPyObject | CfNoPython, "l",
                                      (long)0);
        if (cell == NULL)
            return false;
        if (arg >= 0 && arg != slot) {
            vinfo_decref(f_localsplus[arg], po);
            f_localsplus[arg] = NULL;
        }
        vinfo_xdecref(f_localsplus[slot], po);
        f_localsplus[slot] = cell;
    }
    return true;
}

/* the most keyword arguments a call's **kwargs takes when it is compiled;
   more is the real call */
#define LE_MAX_KEYWORDS_LEFT 16

/* The parameter a keyword argument named 'name' binds, as ceval's binder
   finds it: among those after the positional-only ones, the same object
   first, then an equal string (both are exact str constants, so comparing
   runs no code). -1 for none. */
static Py_ssize_t
keyword_slot(PyCodeObject *co, PyObject *name)
{
    Py_ssize_t i, total_args = co->co_argcount + co->co_kwonlyargcount;
#if LE_HAS_CODE_POSONLY_ARGS
    Py_ssize_t first = co->co_posonlyargcount;
#else
    Py_ssize_t first = 0;
#endif
    for (i = first; i < total_args; i++)
        if (le_code_local_name(co, (int)i) == name)
            return i;
    for (i = first; i < total_args; i++)
        if (PyUnicode_Compare(le_code_local_name(co, (int)i), name) == 0)
            return i;
    return -1;
}

/* the arguments' part of ceval.c:_PyEval_EvalCodeWithName: -1 where it
   needs the real call, -2 on an error */
DEFINEFN
int
_PsycoEval_EvalCodeWithName(PsycoObject *po, PyObject *_co,
                            vinfo_t **f_localsplus, vinfo_t **args,
                            Py_ssize_t argcount, PyObject *kwnames,
                            vinfo_t **kwargs, vinfo_t **defs,
                            Py_ssize_t defcount, PyObject *kwdefs,
                            vinfo_t **closure, bool make_cells)
{
    PyCodeObject *co = (PyCodeObject *)_co;
    Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount;
    vinfo_t **fastlocals = f_localsplus;
    Py_ssize_t i, n, m, missing;
    vinfo_t *x;
    /* the keyword arguments **kwargs takes, made last, once the binding is
       known not to need the real call */
    Py_ssize_t left[LE_MAX_KEYWORDS_LEFT], nleft = 0;

    n = argcount > co->co_argcount ? co->co_argcount : argcount;
    for (i = 0; i < n; i++) {
        x = args[i];
        vinfo_incref(x);
        vinfo_xdecref(fastlocals[i], po);
        fastlocals[i] = x;
    }
    if (co->co_flags & CO_VARARGS) {
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
    }
    if (kwnames != NULL) {
        Py_ssize_t k;
        for (k = 0; k < PyTuple_GET_SIZE(kwnames); k++) {
            Py_ssize_t j = keyword_slot(co, PyTuple_GET_ITEM(kwnames, k));
            if (j < 0) {
                /* unexpected, or positional-only: into **kwargs, else the
                   real call */
                if (!(co->co_flags & CO_VARKEYWORDS) ||
                    nleft == LE_MAX_KEYWORDS_LEFT) {
                    return -1;
                }
                left[nleft++] = k;
                continue;
            }
            if (fastlocals[j] != NULL) {
                return -1; /* multiple values: the real call */
            }
            x = kwargs[k];
            vinfo_incref(x);
            fastlocals[j] = x;
        }
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
            PyObject *def;
            if (fastlocals[i] != NULL) {
                continue;
            }
            if (kwdefs == NULL) {
                return -1; /* the defaults aren't known: the real call */
            }
            /* only exact str keys: the lookup runs no code */
            def = PyDict_GetItemWithError(kwdefs,
                                          le_code_local_name(co, (int)i));
            if (def == NULL) {
                /* missing_arguments: the real call */
                return PyErr_Occurred() ? -2 : -1;
            }
            Py_INCREF(def);
            fastlocals[i] =
                vinfo_new(CompileTime_NewSk(sk_new((long)def, SkFlagPyObj)));
        }
    }
    if (co->co_flags & CO_VARKEYWORDS) {
        /* a new dict, as CPython makes for every call, in the call's order;
           not made when the binding is only checked again (bind_cells) */
        Py_ssize_t slot = total_args + ((co->co_flags & CO_VARARGS) ? 1 : 0);
        vinfo_t *d;
        if (!make_cells) {
            d = psyco_vi_Zero();
        }
        else {
            d = psyco_generic_call(po, PyDict_New, CfCommonNewRefPyObject, "");
            if (d == NULL) {
                return -2;
            }
            for (i = 0; i < nleft; i++) {
                if (!PsycoDict_SetItemC(po, d,
                                        PyTuple_GET_ITEM(kwnames, left[i]),
                                        kwargs[left[i]])) {
                    vinfo_decref(d, po);
                    return -2;
                }
            }
        }
        vinfo_xdecref(fastlocals[slot], po);
        fastlocals[slot] = d;
    }
    if (!bind_cells(po, co, f_localsplus, make_cells)) {
        return -2;
    }
    for (i = 0; i < le_code_nfrees(co); i++) {
        x = closure[i];
        vinfo_incref(x);
        f_localsplus[le_code_free_slot(co, (int)i)] = x;
    }
    return 0;
}

/* The callee frame's locals: the call's arguments (keyword ones named by
   'kwnames', or none), defaults, cells and free variables bound as
   _PyEval_EvalCodeWithName binds them (the translated binder), and the
   'nextra' values of 'extra' after them. Its own cells are made only if
   'make_cells' (bind_cells). 0 when that binding needs the real call: a
   wrong count or a keyword it doesn't take (whose TypeError's wording
   CPython's own call gives), or more extra positional arguments than a
   virtual tuple holds; -1 on an error. */
static int
fncall_bind(struct fncall_arg_s *fncall, PsycoObject *po, vinfo_t *vglobals,
            vinfo_t *vbuiltins, vinfo_t **argarray, int argcount,
            PyObject *kwnames, vinfo_t **kwargs, vinfo_t **defarray,
            int defcount, PyObject *kwdefs, vinfo_t **frees, int nextra,
            vinfo_t **extra, bool make_cells)
{
    PyCodeObject *co = fncall->co;
    int i, nlocals = le_code_nlocals(co), nslots = le_code_nlocalsplus(co);
    vinfo_array_t *a = array_new(INDEX_LOC_LOCALS_PLUS + nslots + nextra);
    vinfo_t **locals = a->items + INDEX_LOC_LOCALS_PLUS;
    int bound;

    vinfo_incref(vglobals);
    a->items[INDEX_LOC_GLOBALS] = vglobals;
    vinfo_incref(vbuiltins);
    a->items[INDEX_LOC_BUILTINS] = vbuiltins;
    bound = _PsycoEval_EvalCodeWithName(po, (PyObject *)co, locals, argarray,
                                        argcount, kwnames, kwargs, defarray,
                                        defcount, kwdefs, frees, make_cells);
    if (bound < 0) {
        array_delete(a, po);
        return bound == -1 ? 0 : -1;
    }
    /* LeJIT's unbound local is a known NULL, as LOAD_FAST tests it */
    for (i = 0; i < nlocals; i++)
        if (locals[i] == NULL)
            locals[i] = psyco_vi_Zero();
    for (i = 0; i < nextra; i++) {
        vinfo_incref(extra[i]);
        locals[nslots + i] = extra[i];
    }
    fncall->inputvinfos = a;
    fncall->po_size =
        INDEX_LOC_LOCALS_PLUS + co->co_stacksize + nslots + nextra;
    return 1;
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

/* The cells and the **kwargs dict the binder made are the callee's, which
   frees them with its other locals, in CPython's order
   (pycompiler.c:exit_function): the
   reference goes from the caller's value, so that releasing it emits
   nothing, to the callee's argument. The caller's value goes as the bound
   locals do, before the call is emitted; nothing emitted in between uses
   its register, which the call pushes (psyco_call_psyco). */
static void
give_slot(struct fncall_arg_s *fncall, vinfo_array_t *callee, int slot)
{
    int k = INDEX_LOC_LOCALS_PLUS + slot;
    vinfo_t *mine = fncall->inputvinfos->items[k];
    vinfo_t *theirs = callee->items[k];
    if (mine != NULL && theirs != NULL && has_rtref(mine->source) &&
        is_runtime(theirs->source)) {
        mine->source = remove_rtref(mine->source);
        theirs->source = add_rtref(theirs->source);
    }
}
static void
give_cells(struct fncall_arg_s *fncall, vinfo_array_t *callee)
{
    PyCodeObject *co = fncall->co;
    int i;
    for (i = 0; i < le_code_ncells(co); i++)
        give_slot(fncall, callee, le_code_cell_slot(co, i));
    if (co->co_flags & CO_VARKEYWORDS)
        give_slot(fncall, callee,
                  co->co_argcount + co->co_kwonlyargcount +
                      ((co->co_flags & CO_VARARGS) ? 1 : 0));
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
    give_cells(fncall, arraycopy);
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
        vinfo_t *v = vinfo_new(SOURCE_NOT_IMPORTANT);
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

/* whether the code being compiled may inline a call: not in blocks made
   after one of its merge points reached its cap (vcompiler.c:mp_new_block).
   One-way and read by the block's serial, so a replay decides the same
   (codemanager.h:psyco_block_serial). An inlined call's instruction, which
   restarts with the body's result (LOC_INLINING), is inlining whatever
   the block it restarts in. */
static bool
inline_into(PsycoObject *po)
{
    le_code_extra_t *x = le_code_extra_get(po->pr.co);
    return x == NULL || x->inline_end == 0 ||
           psyco_block_serial(po) + 1 < x->inline_end;
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
    /* in CPython's order, as the end of a called frame (pycompiler.c:
       exit_function) */
    for (i = 0; i < oldcount; i++)
        vinfo_xdecref(
            old[LE_HAS_FRAME_CLEARED_LAST_FIRST ? oldcount - 1 - i : i], po);
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

/* Py_LeaveRecursiveCall after a compiled call, and the inlined frame's
   level psyco_enter_recursive_call added; 'through_c' as there */
DEFINEFN
bool
psyco_leave_recursive_call(PsycoObject *po, int through_c)
{
    vinfo_t *vts = load_tstate(po), *vdepth;
    if (vts == NULL)
        return false;
    if (po->pr.is_inlining) {
        vdepth = add_to_depth(po, -LE_RECURSION_STEP, vts);
        if (vdepth == NULL) {
            vinfo_decref(vts, po);
            return false;
        }
        vinfo_decref(vdepth, po);
    }
    if (through_c != LE_C_NONE)
        return leave_c_recursive_call(po, vts, through_c);
    return leave_recursive_call(po, vts);
}

/* Py_EnterRecursiveCall before a compiled call, on a thread state the
   caller has loaded. From an inlined frame, for that frame too while the
   call runs: CPython counted it as it entered, and the test of the call's
   level stands for its (a call through C from an inlined frame doesn't
   count the frame). With 'through_c', where CPython enters the frame from
   C (mechanisms/recursion.h:enter_c_recursive_levels). False with an
   exception set in 'po'. */
static bool
psyco_enter_recursive_call(PsycoObject *po, vinfo_t *vts, int through_c)
{
    int levels = po->pr.is_inlining ? 2 : 1;
    return through_c != LE_C_NONE
               ? enter_c_recursive_levels(po, vts, levels, through_c)
               : enter_recursive_levels(po, vts, levels);
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
    if (cc == CC_ERROR)
        return false;
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
static int
tracing_on(PsycoObject *po, vinfo_t *vts, PyCodeObject *co)
{
#if LE_HAS_MONITORING
    if (!runtime_condition_f(po, le_tracing_test(po, vts, co)))
        return le_code_tools_on(po, co);
#else
    if (!runtime_condition_f(po, le_tracing_test(po, vts, co)))
        return 0;
#endif
#if LE_HAS_MONITORING
    {
        /* the test above is the version's: the exact one, on its rare
           path, also brings le_quiet_version up to date */
        condition_code_t cc;
        vinfo_t *v = psyco_generic_call(
            po, le_tracing_recheck, CfReturnTypeInt | CfNoBlock | CfNoPython,
            "l", (long)co);
        if (v == NULL)
            return -1;
        cc = integer_non_null(po, v);
        vinfo_decref(v, po);
        if (cc == CC_ERROR)
            return -1;
        return runtime_condition_f(po, cc);
    }
#else
    return 1;
#endif
}

/* Whether a compiled call or inlined frame must be the real call
   instead, which is tracing_on or another eval hook installed over
   LeJIT's (le_direct_call_refused) */
static bool
direct_call_refused(PsycoObject *po, vinfo_t *vts, PyCodeObject *co)
{
    return le_direct_call_refused(po, vts, co);
}

DEFINEFN
int
psyco_tracing_on(PsycoObject *po)
{
    int on;
    vinfo_t *vts;
#if LE_HAS_MONITORING
    vts = NULL; /* le_tracing_test reads none */
#else
    vts = load_tstate(po);
    if (vts == NULL)
        return -1;
#endif
    on = tracing_on(po, vts, po->pr.co);
    vinfo_xdecref(vts, po);
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

/* The cells of the free variables of 'vfunc' (not NULL), whose code has
   'nfrees' of them, in *frees: a function still virtual (MAKE_FUNCTION
   here: a comprehension, say) has them as its closure tuple's items, any
   other in func_closure, read into buf. func_closure can't be set from
   Python, and its length is the code's number of free variables
   (funcobject.c:func_set_code checks it), as ceval assumes. A constant
   function's cells are constants read now: borrowed, since code compiled
   for a cell only ever uses its address, which a new cell at the same
   address may as well share. 1, or 0 where they aren't known, or -1 on an
   error. */
static int
load_frees(PsycoObject *po, vinfo_t *vfunc, int nfrees, vinfo_t **buf,
           vinfo_t ***frees)
{
    vinfo_t *closure;
    int i;
    if (nfrees > MAX_CALL_FREES)
        return 0;
    if (is_virtualtime(vfunc->source)) {
        closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
        if (closure == NULL || PsycoTuple_Load(closure) != nfrees)
            return 0;
        *frees = &PsycoTuple_GET_ITEM(closure, 0);
        return 1;
    }
    closure = psyco_get_const(po, vfunc, FUNC_closure);
    for (i = 0; closure != NULL && i < nfrees; i++)
        if ((buf[i] = psyco_get_nth_const(po, closure, TUPLE_ob_item, i)) ==
            NULL)
            closure = NULL;
    if (closure == NULL)
        return -1;
    *frees = buf;
    return 1;
}

/* The positional arguments of a call that binding gave 'nargs' arguments
   and the last of 'ndefs' defaults: the arguments, then the defaults it
   used. A new reference. */
static vinfo_t *
args_with_defaults(PyCodeObject *co, vinfo_t *arg_tuple, int nargs,
                   vinfo_t **defs, int ndefs)
{
    int i, n = co->co_argcount;
    vinfo_t *t;
    if (nargs >= n) {
        vinfo_incref(arg_tuple);
        return arg_tuple;
    }
    t = PsycoTuple_New(n, NULL);
    for (i = 0; i < n; i++) {
        vinfo_t *x = i < nargs ? PsycoTuple_GET_ITEM(arg_tuple, i)
                               : defs[ndefs - (n - i)];
        vinfo_incref(x);
        PsycoTuple_GET_ITEM(t, i) = x;
    }
    return t;
}

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

/* whether a generator of 'co' can be stepped as a virtual one
   (gen_send), so is worth keeping pending */
static bool
pending_steppable(PyCodeObject *co)
{
#if LE_RESUMES_GENERATORS_DIRECTLY
    return (co->co_flags & CO_GENERATOR) &&
           !(co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) &&
           psyco_is_resumable_generator(co);
#else
    (void)co;
    return false;
#endif
}

/* whether the coroutine of 'co' that the call being compiled makes is
   awaited at once, by the next instruction of a coroutine compiled to run
   its awaits (GET_AWAITABLE), and its body can run as a call there */
static bool
awaited_at_once(PsycoObject *po, PyCodeObject *co)
{
    PyCodeObject *caller = po->pr.co;
    long mode = psyco_generator_mode(po);
    le_code_t c;
    le_instr_t in;
    if (!(caller->co_flags & CO_COROUTINE) ||
        (mode != GENEXPR_RESUMABLE && mode != GENEXPR_DIRECT) ||
        !psyco_is_startable_coroutine(co))
        return false;
    c = le_code(caller);
    in = le_instr(c, po->pr.next_instr);
    return in.next < c.size && le_instr(c, in.next).op == GET_AWAITABLE;
}

DEFINEFN
vinfo_t *
psyco_call_pyfunc(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                  vinfo_t *vglobals, vinfo_t *vdefaults, vinfo_t *arg_tuple,
                  vinfo_t *kw, PyObject *kwdefs, int recursion,
                  bool allow_inline)
{
    PyObject *kwnames = NULL;
    vinfo_t **kwargs = NULL;
    CodeBufferObject *codebuf;
    PsycoObject *mypo;
    Source *sources;
    vinfo_t *result;
    stack_frame_info_t *finfo;
    int tuple_size, argcount, defcount = -2;
    int nfrees;
    bool inlining, generator;
    vinfo_t *vbuiltins;
    vinfo_t **frees = NULL;
    vinfo_t *freebuf[MAX_CALL_FREES];
    struct fncall_arg_s fncall;

    /* a generator is run only by a consumer that knows it
       (psyco_call_genexpr): until then it is a pending generator, of a
       function known now or still virtual, whose arguments bind here; a
       coroutine too, where it is awaited at once (psyco_coro_start) */
    generator = (co->co_flags & (CO_GENERATOR | CO_COROUTINE)) != 0;
    if (--recursion < 0)
        goto fail_to_default;
    if (generator && (vfunc == NULL || is_runtime(vfunc->source) ||
                      !(psyco_is_fusable_generator(co) ||
                        awaited_at_once(po, co) || pending_steppable(co))))
        goto fail_to_default;
    /* keyword arguments, pending (pdictobject.c) for the binder; a
       generator's binding is positional only */
    if (kw != NULL && !psyco_known_NULL(kw)) {
        kwnames = PsycoDict_PendingNames(kw, &kwargs);
        if (kwnames == NULL || generator)
            goto fail_to_default;
    }
    /* closure code: the cells, read below (load_frees) */
    nfrees = PyCode_GetNumFree(co);
    if (nfrees != 0 && (vfunc == NULL || nfrees > MAX_CALL_FREES))
        goto fail_to_default;

    /* a pending generator's arguments are positional only */
    if (generator && co->co_kwonlyargcount != 0)
        goto fail_to_default;

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
    if (!psyco_forking(po, arg_tuple->array) ||
        (kwnames != NULL && !psyco_forking(po, kw->array)))
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
    if (nfrees != 0) {
        switch (load_frees(po, vfunc, nfrees, freebuf, &frees)) {
            case 1:
                break;
            case 0:
                vinfo_decref(vbuiltins, po);
                goto fail_to_default;
            default:
                vinfo_decref(vbuiltins, po);
                return NULL;
        }
    }
    /* not into itself: inlined calls aren't counted in the recursion depth,
       and a closure calling itself through its cell would
       reach RecursionError at twice CPython's depth */
    inlining = !generator && allow_inline && !po->pr.is_inlining &&
               co != po->pr.co &&
               (psyco_mp_flags(fncall.merge_points) & MP_FLAGS_INLINABLE) &&
               (LOC_INLINING != NULL || inline_into(po));
    /* the instruction restarting with the inlined body's result binds again
       only to check it: its cells were made for the body. A generator's
       binding is checked here and done again by its consumer. */
    switch (fncall_bind(&fncall, po, vglobals, vbuiltins,
                        &PsycoTuple_GET_ITEM(arg_tuple, 0), tuple_size,
                        kwnames, kwargs, &PsycoTuple_GET_ITEM(vdefaults, 0),
                        defcount, kwdefs, frees, 0, NULL,
                        !generator && (!inlining || LOC_INLINING == NULL))) {
        case 1:
            break;
        case 0:
            /* the wrong number of arguments: the real call raises CPython's
               TypeError, whose wording depends on the defaults and
               keyword-only arguments (ceval.c:too_many_positional,
               missing_arguments) */
            vinfo_decref(vbuiltins, po);
            goto fail_to_default;
        default:
            vinfo_decref(vbuiltins, po);
            return NULL;
    }
    if (generator) {
        int i;
        vinfo_t *args =
            args_with_defaults(co, arg_tuple, tuple_size,
                               &PsycoTuple_GET_ITEM(vdefaults, 0), defcount);
        /* an argument can be borrowed from an object the caller drops
           before the generator is made (a bound method's self) */
        for (i = PsycoTuple_Load(args); i--;)
            need_reference(po, PsycoTuple_GET_ITEM(args, i));
        array_delete(fncall.inputvinfos, po);
        vinfo_decref(vbuiltins, po);
        result = PsycoGenexpr_New(vfunc, args, vglobals, co);
        vinfo_decref(args, po);
        return result;
    }
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
    /* a traced call, or one under another eval hook, goes through the
       hook (direct_call_refused). Not again when the instruction restarts
       after the inlined body with its result. */
    if (!inlining || LOC_INLINING == NULL) {
        bool refused, entered = true;
        vinfo_t *vts = load_tstate(po);
        if (vts == NULL) {
            array_delete(fncall.inputvinfos, po);
            vinfo_decref(vbuiltins, po);
            return NULL;
        }
        refused = direct_call_refused(po, vts, co);
        /* ceval tests eval_breaker as the callee's frame starts, and a
           direct call skips that test: with an event flagged it is the real
           call instead, whose entry handles the event (LeCode_Run) */
        if (!refused && !inlining && !BREAKER_SKIPS(le_op(le_code(co), 0))) {
            condition_code_t cc = le_eval_breaker_test_on(po, vts);
            if (cc == CC_ERROR)
                entered = false;
            else
                refused = runtime_condition_f(po, cc);
        }
#if LE_STACK_SEGMENTS
        /* with the stack low, the real call instead, on a segment */
        if (!refused && entered && !inlining)
            refused = runtime_condition_f(po, psyco_stack_low(po));
#endif
        /* count the call's depth, as ceval does on entering a frame, so that
           deep recursion raises RecursionError instead of overflowing the C
           stack; psyco_call_psyco leaves it after the call.
           Emitted before the frame is built, so no argument source moves. */
        if (!refused && entered && !inlining)
            entered = psyco_enter_recursive_call(po, vts, LE_C_NONE);
        vinfo_decref(vts, po);
        if (refused || !entered) {
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
            psyco_leave_recursive_call(po, LE_C_NONE);
            goto pyerr;
        }
        argcount = get_arguments_count(&mypo->vlocals);
        finfo = psyco_finfo(po, mypo);

        /* compile the function (this frees mypo) */
        codebuf = compile_callee(mypo, co);

        /* get the run-time argument sources and push them on the stack
           and write the actual CALL */
        result =
            psyco_call_psyco(po, codebuf, sources, argcount, finfo, LE_C_NONE);
        PyMem_FREE(sources);
    }
    vinfo_decref(vbuiltins, po);
    return result;

fail_to_default:
    /* call the function object itself: rebuilding the call from its code,
       globals and defaults, as upstream's cimpl_call_pyfunc did, would lose
       its name and qualname (generators take co_name), closure and
       keyword-only defaults. Only a call that runs the body can recurse. */
    return PsycoFunction_RealCall(po, vfunc, arg_tuple, kw, !generator);

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
    result =
        psyco_call_pyfunc(po, vfunc, co, LOC_GLOBALS, vnodefaults, arg_tuple,
                          NULL, NULL, po->pr.auto_recursion, true);
    vinfo_decref(vnodefaults, po);
    return result;

fallback:
    return PsycoFunction_RealCall(po, vfunc, arg_tuple, NULL, true);
}

/* the code of a pending generator */
static PyCodeObject *
pending_code(vinfo_t *vgen)
{
    vinfo_t *vcode = vgen->array->items[iGENEXPR_CODE];
    return (PyCodeObject *)CompileTime_Get(vcode->source)->value;
}

/* Run a pending generator (PsycoGenexpr_New) for its consumer: its code
   compiled with 'mode' and 'acc' in the two GENEXPR slots (pycheader.h),
   called like a function of its arguments, returning the consumer's result
   as a new reference. */
DEFINEFN
vinfo_t *
psyco_call_genexpr(PsycoObject *po, vinfo_t *vgen, int mode, vinfo_t *acc,
                   vinfo_t *comp)
{
    vinfo_t *vfunc = vgen->array->items[iGENEXPR_FUNC];
    vinfo_t *args = vgen->array->items[iGENEXPR_ARGS];
    vinfo_t *vglobals = vgen->array->items[iGENEXPR_GLOBALS];
    PyCodeObject *co = pending_code(vgen);
    vinfo_t *vbuiltins, *vmode, *result, *vts;
    vinfo_t *extra[GENEXPR_SLOTS], *freebuf[MAX_CALL_FREES];
    vinfo_t **frees = NULL;
    CodeBufferObject *codebuf;
    PsycoObject *mypo;
    Source *sources;
    stack_frame_info_t *finfo;
    int nfrees = PyCode_GetNumFree(co), argcount;
    int through_c;
    /* zeroed: gcc can't see that psyco_fatal_msg below doesn't return */
    struct fncall_arg_s fncall = {0};

    /* each checked by the consumer (psyco_genexpr_fusable) */
    if (nfrees != 0 && load_frees(po, vfunc, nfrees, freebuf, &frees) != 1)
        psyco_fatal_msg("generator closure");
    if (!psyco_forking(po, args->array) || !fncall_init(&fncall, co, nfrees))
        psyco_fatal_msg("generator call");
    /* fixed: the fused code reads it at compile time (pycompiler.c), so a
       snapshot for another consumer must not match by un-promoting it */
    vmode = vinfo_new(CompileTime_NewSk(sk_new(mode, SkFlagFixed)));
    extra[0] = vmode;
    extra[1] = acc;
#if LE_HAS_SUM_COMPENSATED
    if (comp != NULL)
        vinfo_incref(comp);
    else
        comp = psyco_vi_Zero();
    extra[2] = comp;
#endif
    if (callee_builtins(po, vfunc, vglobals, &vbuiltins) != 1)
        psyco_fatal_msg("generator builtins");
    /* the defaults are among the arguments (args_with_defaults), and their
       count bound at the call */
    switch (fncall_bind(&fncall, po, vglobals, vbuiltins,
                        &PsycoTuple_GET_ITEM(args, 0), PsycoTuple_Load(args),
                        NULL, NULL, NULL, 0, NULL, frees, GENEXPR_SLOTS, extra,
                        true)) {
        case 1:
            break;
        case 0:
            psyco_fatal_msg("generator arguments");
            break;
        default:
            vinfo_decref(vmode, po);
#if LE_HAS_SUM_COMPENSATED
            vinfo_decref(comp, po);
#endif
            vinfo_decref(vbuiltins, po);
            return NULL;
    }

    /* as psyco_call_pyfunc's non-inlining call; CPython runs the body from
       C, its consumer's, but awaits a coroutine with no C between */
    result = NULL;
    through_c = mode != GENEXPR_DIRECT ? LE_C_CONSUMER : LE_C_NONE;
    vts = load_tstate(po);
    if (vts != NULL && psyco_enter_recursive_call(po, vts, through_c)) {
        vinfo_decref(vts, po);
        vts = NULL;
        Psyco_ForgetClasses(po);
        mypo = psyco_build_frame(po, &fncall, po->pr.auto_recursion, &sources);
        if (mypo == NULL) {
            psyco_leave_recursive_call(po, through_c);
            psyco_virtualize_exception(po);
        }
        else {
            argcount = get_arguments_count(&mypo->vlocals);
            finfo = psyco_finfo(po, mypo);
            if (mode == GENEXPR_DIRECT)
                finfo->coro_func =
                    (PyFunctionObject *)CompileTime_Get(vfunc->source)->value;
            codebuf = compile_callee(mypo, co);
            result = psyco_call_psyco(po, codebuf, sources, argcount, finfo,
                                      through_c);
            PyMem_FREE(sources);
        }
    }
    else
        array_delete(fncall.inputvinfos, po);
    vinfo_xdecref(vts, po);
    vinfo_decref(vmode, po);
#if LE_HAS_SUM_COMPENSATED
    vinfo_decref(comp, po);
#endif
    vinfo_decref(vbuiltins, po);
    return result;
}

#if LE_RESUMES_GENERATORS_DIRECTLY
/* A generator's fields and its frame's (mechanisms/frames.h), and the
   thread state's that a step changes. The code is the one field known
   once read: the frame never changes it. On 3.14 the word is a
   _PyStackRef, kept here as the object (code_of). */
#define GEN_code CPY_FIELD(PyGenObject, code, OB_type)
#define iGEN_CODE FIELD_INDEX(GEN_code)
#define GEN_code_word FMUT(GEN_code)
#define GEN_globals FMUT(CPY_FIELD(PyGenObject, globals, GEN_code))
#define GEN_builtins FMUT(CPY_FIELD(PyGenObject, builtins, GEN_code))
#define GEN_locals FMUT(CPY_FIELD(PyGenObject, locals, GEN_code))
#define GEN_place FMUT(CPY_FIELD(PyGenObject, place, GEN_code))
#define GEN_previous FMUT(CPY_FIELD(PyGenObject, previous, GEN_code))
#define GEN_frame_state FMUT(CPY_FIELD(PyGenObject, frame_state, GEN_code))
#define GEN_exc_previous FMUT(CPY_FIELD(PyGenObject, exc_previous, GEN_code))
#if LE_HAS_FRAME_STACKREFS
#define GEN_stackpointer FMUT(CPY_FIELD(PyGenObject, stackpointer, GEN_code))
#else
#define GEN_stacktop FMUT(CPY_FIELD(PyGenObject, stacktop, GEN_code))
#endif
#define TSTATE_exc_info FMUT(CPY_FIELD(PyThreadState, exc_info, NO_PREV_FIELD))
#if LE_HAS_TSTATE_CURRENT_FRAME
#define TSTATE_current_frame \
    FMUT(DEF_FIELD(PyThreadState, le_frame_t *, current_frame, NO_PREV_FIELD))
#else
typedef __typeof__(*((PyThreadState *)0)->cframe) le_gen_cframe_t;
#define TSTATE_gen_cframe \
    FMUT(DEF_FIELD(PyThreadState, le_gen_cframe_t *, cframe, NO_PREV_FIELD))
#define CFRAME_current_frame                                     \
    FMUT(DEF_FIELD(le_gen_cframe_t, le_frame_t *, current_frame, \
                   NO_PREV_FIELD))
#endif
/* a code's table of direct entries (psyco.h:le_gen_direct_t) */
#define DIRECT_globals \
    FMUT(DEF_FIELD(le_gen_direct_t, PyObject *, globals, NO_PREV_FIELD))
#define DIRECT_builtins \
    FMUT(DEF_FIELD(le_gen_direct_t, PyObject *, builtins, NO_PREV_FIELD))
/* by le_gen_direct_index less one: the place's unit */
#define DIRECT_entries       \
    FMUT(DEF_ARRAY(code_t *, \
                   offsetof(le_gen_direct_t, entries) + sizeof(code_t *)))

/* What a step reads and writes of a generator, or of a virtual one's
   record (mechanisms/frames.h:le_gen_record_t), which keeps the same
   fields elsewhere; and what ends a step that didn't pause in the usual
   way */
struct gen_layout {
    defield_t state, exc_previous, locals, globals, builtins, place, previous;
    defield_t stack;
    long frame_ofs, exc_ofs;
    void *end, *send_end, *send_status;
};
static struct gen_layout gen_layout, record_layout;
#define REC_state \
    FMUT(DEF_FIELD(le_gen_record_t, int8_t, state, NO_PREV_FIELD))
#define REC_FRAME(ctype, f, prev) \
    FMUT(DEF_FIELD(le_gen_record_t, ctype, f, prev))
#define REC_exc_previous REC_FRAME(void *, exc_state.previous_item, REC_state)
#define REC_locals REC_FRAME(PyObject *, frame.f_locals, REC_exc_previous)
#define REC_globals REC_FRAME(PyObject *, frame.f_globals, REC_locals)
#define REC_builtins REC_FRAME(PyObject *, frame.f_builtins, REC_globals)
#define REC_place \
    REC_FRAME(_Py_CODEUNIT *, frame.LE_FRAME_PLACE_FIELD, REC_builtins)
#define REC_previous REC_FRAME(le_frame_t *, frame.previous, REC_place)
#if LE_HAS_FRAME_STACKREFS
#define REC_stack REC_FRAME(void *, frame.stackpointer, REC_previous)
#else
#define REC_stack REC_FRAME(int, frame.stacktop, REC_previous)
#endif
/* the record's words, as the frame's */
#define REC_words                                                 \
    FMUT(DEF_ARRAY(PyObject *, offsetof(le_gen_record_t, frame) + \
                                   offsetof(le_frame_t, localsplus)))
static void
gen_layouts_init(void)
{
    struct gen_layout g = {GEN_frame_state,
                           GEN_exc_previous,
                           GEN_locals,
                           GEN_globals,
                           GEN_builtins,
                           GEN_place,
                           GEN_previous,
#if LE_HAS_FRAME_STACKREFS
                           GEN_stackpointer,
#else
                           GEN_stacktop,
#endif
                           LE_GEN_FRAME_OFS,
                           offsetof(PyGenObject, gi_exc_state),
                           (void *)le_gen_direct_end,
                           (void *)le_gen_direct_send_end,
                           (void *)le_gen_direct_send_status};
    struct gen_layout r = {REC_state,
                           REC_exc_previous,
                           REC_locals,
                           REC_globals,
                           REC_builtins,
                           REC_place,
                           REC_previous,
                           REC_stack,
                           offsetof(le_gen_record_t, frame),
                           offsetof(le_gen_record_t, exc_state),
                           (void *)le_gen_record_end,
                           (void *)le_gen_record_send_end,
                           (void *)le_gen_record_send_status};
    gen_layout = g;
    record_layout = r;
}

/* the code object in the field 'df' of the generator, or of the record a
   frame word holds (yield_resumable), 'vgen': read once, kept in its
   vinfo, so that it can be promoted */
static vinfo_t *
code_of(PsycoObject *po, vinfo_t *vgen, defield_t df)
{
    vinfo_t *v = vinfo_getitem(vgen, iGEN_CODE);
    if (v != NULL) {
        vinfo_incref(v);
        return v;
    }
    v = psyco_get_field(po, vgen, df);
    if (v == NULL)
        return NULL;
#if LE_HAS_FRAME_STACKREFS
    {
        vinfo_t *vmask = vinfo_new(CompileTime_New(~(long)Py_TAG_BITS));
        vinfo_t *vo = integer_and(po, v, vmask);
        vinfo_decref(vmask, po);
        vinfo_decref(v, po);
        if (vo == NULL)
            return NULL;
        v = vo;
    }
#endif
    vinfo_incref(v);
    vinfo_setitem(po, vgen, iGEN_CODE, v);
    return v;
}
#if LE_HAS_FRAME_STACKREFS || LE_HAS_FRAME_EXECUTABLE
#define REC_code_word \
    FMUT(DEF_FIELD(le_gen_record_t, void *, frame.f_executable, NO_PREV_FIELD))
#else
#define REC_code_word \
    FMUT(DEF_FIELD(le_gen_record_t, void *, frame.f_code, NO_PREV_FIELD))
#endif

/* whether 'cc' holds at run time, -1 on error (or a promotion) */
static int
gen_test(PsycoObject *po, condition_code_t cc)
{
    if (cc == CC_ERROR)
        return -1;
    return runtime_condition_f(po, cc);
}

/* the same, with a constant */
static int
gen_field_test_i(PsycoObject *po, vinfo_t *vi, defield_t df, long value,
                 int op)
{
    condition_code_t cc;
    vinfo_t *vf = psyco_get_field(po, vi, df);
    if (vf == NULL)
        return -1;
    cc = integer_cmp_i(po, vf, value, op);
    vinfo_decref(vf, po);
    return gen_test(po, cc);
}

/* stores the constant 'value' in the field 'df' of 'vi' */
static bool
gen_put_i(PsycoObject *po, vinfo_t *vi, defield_t df, long value)
{
    vinfo_t *v = vinfo_new(CompileTime_New(value));
    bool ok = psyco_put_field(po, vi, df, v);
    vinfo_decref(v, po);
    return ok;
}

/* the thread's innermost frame */
static vinfo_t *
gen_current_frame(PsycoObject *po, vinfo_t *vts)
{
#if LE_HAS_TSTATE_CURRENT_FRAME
    return psyco_get_field(po, vts, TSTATE_current_frame);
#else
    vinfo_t *v, *vcframe = psyco_get_field(po, vts, TSTATE_gen_cframe);
    if (vcframe == NULL)
        return NULL;
    v = psyco_get_field(po, vcframe, CFRAME_current_frame);
    vinfo_decref(vcframe, po);
    return v;
#endif
}

/* the entry in the table 't' for the place 'vplace' of a frame of 'co',
   by its unit, tested: 1 with it in '*ventry' if there is one, 0 if
   none, -1 on error */
static int
gen_entry_at(PsycoObject *po, PyCodeObject *co, vinfo_t *vt, vinfo_t *vplace,
             vinfo_t **ventry)
{
    vinfo_t *vstart, *vdiff, *vindex, *ve;
    int r;
    vstart = vinfo_new(CompileTime_New((long)_PyCode_CODE(co)));
    vdiff = integer_sub(po, vplace, vstart, false);
    vinfo_decref(vstart, po);
    vindex = vdiff == NULL ? NULL : integer_urshift_i(po, vdiff, 1);
    vinfo_xdecref(vdiff, po);
    ve = vindex == NULL
             ? NULL
             : psyco_get_field_array(po, vt, DIRECT_entries, vindex);
    vinfo_xdecref(vindex, po);
    if (ve == NULL)
        return -1;
    r = gen_test(po, integer_cmp_i(po, ve, 0, Py_EQ));
    if (r != 0) {
        vinfo_decref(ve, po);
        return r < 0 ? -1 : 0;
    }
    *ventry = ve;
    return 1;
}

/* 'vg', or the field 'df' of 'vgen' where 'vg' is NULL, compared with
   'v' by 'op', as gen_test */
static int
gen_value_test(PsycoObject *po, vinfo_t *vg, vinfo_t *vgen, defield_t df,
               vinfo_t *v, int op)
{
    condition_code_t cc;
    vinfo_t *vf =
        vg != NULL ? (vinfo_incref(vg), vg) : psyco_get_field(po, vgen, df);
    if (vf == NULL)
        return -1;
    cc = integer_cmp(po, vf, v, op);
    vinfo_decref(vf, po);
    return gen_test(po, cc);
}

/* the tests that don't depend on the generator's state: tracing or
   another eval hook, a pending event, and globals and builtins other than
   those the entries of the table 'vt' were compiled for: 'vg' and 'vb',
   or where those are NULL the fields of 'vgen' laid out as 'L'; as
   gen_test */
static int
gen_run_tests(PsycoObject *po, PyCodeObject *co, vinfo_t *vt, vinfo_t *vts,
              vinfo_t *vgen, const struct gen_layout *L, vinfo_t *vg,
              vinfo_t *vb)
{
    vinfo_t *tg, *tb;
    int r;
    if (le_direct_call_refused(po, vts, co))
        return 1;
    if ((r = gen_test(po, le_eval_breaker_test_on(po, vts))) != 0)
        return r;
    tg = psyco_get_field(po, vt, DIRECT_globals);
    tb = psyco_get_field(po, vt, DIRECT_builtins);
    r = tg == NULL || tb == NULL
            ? -1
            : gen_value_test(po, vg, vgen, L->globals, tg, Py_NE);
    if (r == 0)
        r = gen_value_test(po, vb, vgen, L->builtins, tb, Py_NE);
    vinfo_xdecref(tg, po);
    vinfo_xdecref(tb, po);
    return r;
}

/* The tests before a direct step of the run-time generator, or virtual
   generator's record, 'vgen' (laid out as 'L') of code 'co', whose table
   is 't', on the thread state 'vts': 1 if all pass, with the entry to
   call in '*ventry', 0 for the step's fallback instead (anything
   gen_send_ex2 or le_gen_to_run would do other than run the entry: the
   generator running or finished, or not started when the value sent may
   not be None, frame locals made, gen_run_tests, no entry compiled yet),
   -1 on error. */
static int
gen_step_tests(PsycoObject *po, const struct gen_layout *L, vinfo_t *vgen,
               PyCodeObject *co, le_gen_direct_t *t, vinfo_t *vts,
               bool sends_any, vinfo_t **ventry)
{
    vinfo_t *vt, *vplace;
    int r;
    if ((r = gen_field_test_i(po, vgen, L->state, FRAME_EXECUTING, Py_GE)) !=
            0 ||
        (sends_any && (r = gen_field_test_i(po, vgen, L->state, FRAME_CREATED,
                                            Py_EQ)) != 0) ||
        (r = gen_field_test_i(po, vgen, L->locals, 0, Py_NE)) != 0)
        return r < 0 ? -1 : 0;
    vt = vinfo_new(CompileTime_New((long)t));
    r = gen_run_tests(po, co, vt, vts, vgen, L, NULL, NULL);
    if (r != 0) {
        vinfo_decref(vt, po);
        return r < 0 ? -1 : 0;
    }
    /* the entry at the frame's place */
    vplace = psyco_get_field(po, vgen, L->place);
    r = vplace == NULL ? -1 : gen_entry_at(po, co, vt, vplace, ventry);
    vinfo_xdecref(vplace, po);
    vinfo_decref(vt, po);
    return r;
}

/* whether the pending generator 'vgen' of code 'co' can become virtual
   at its first step: a plain generator whose body runs compiled, bound
   from its positional arguments alone */
static bool
vgen_startable(vinfo_t *vgen, PyCodeObject *co)
{
    vinfo_t *args = vgen->array->items[iGENEXPR_ARGS];
    return pending_steppable(co) && args != NULL &&
           PsycoTuple_Load(args) == co->co_argcount &&
           le_gen_record_start_unit(co) >= 0;
}

/* The first step of the pending generator 'vgen' of code 'co'
   (vgen_startable), whose table is 't': if its start has an entry and nothing
   would stop a direct step (gen_run_tests), it becomes a virtual generator,
   its record made with its arguments in its words, which the entry runs; 1
   with the entry in '*ventry', 0 for the step's fallback (the pending
   generator is made real), -1 on error. */
static int
vgen_start(PsycoObject *po, vinfo_t *vgen, PyCodeObject *co,
           le_gen_direct_t *t, vinfo_t *vts, vinfo_t **ventry)
{
    vinfo_t *vfunc = vgen->array->items[iGENEXPR_FUNC];
    vinfo_t *vglobals = vgen->array->items[iGENEXPR_GLOBALS];
    vinfo_t *args = vgen->array->items[iGENEXPR_ARGS];
    vinfo_t *vt, *vb, *vplace, *vrec;
    long start = le_gen_record_start_unit(co);
    int r, i;
    if (callee_builtins(po, vfunc, vglobals, &vb) != 1)
        return 0;
    vt = vinfo_new(CompileTime_New((long)t));
    r = gen_run_tests(po, co, vt, vts, NULL, &record_layout, vglobals, vb);
    vinfo_decref(vb, po);
    if (r == 0) {
        vplace = vinfo_new(CompileTime_New(
            (long)(_PyCode_CODE(co) + start + LE_HAS_FRAME_INSTR_PTR)));
        r = gen_entry_at(po, co, vt, vplace, ventry);
        vinfo_decref(vplace, po);
    }
    else
        r = r < 0 ? -1 : 0;
    vinfo_decref(vt, po);
    if (r <= 0)
        return r;

    /* the record, which the virtual generator holds in place of its
       arguments, then its words */
    vrec = psyco_generic_call(po, le_gen_record_new, CfCommonNewRefPyObject,
                              "vl", vfunc, start);
    if (vrec == NULL)
        goto error;
    vinfo_incref(args);
    PsycoVgen_FromPending(po, vgen, vrec);
    for (i = 0; i < co->co_argcount; i++) {
        vinfo_t *v = PsycoTuple_GET_ITEM(args, i), *w;
        bool ok;
        if (!compute_vinfo(v, po)) {
            vinfo_decref(args, po);
            goto error;
        }
#if LE_HAS_FRAME_STACKREFS
        w = psyco_stackref_from_steal(po, v);
#else
        w = v;
        vinfo_incref(w);
#endif
        ok = w != NULL && psyco_put_nth_field(po, vrec, REC_words, i, w);
        vinfo_xdecref(w, po);
        if (!ok) {
            vinfo_decref(args, po);
            goto error;
        }
        consume_reference(po, v);
    }
    vinfo_decref(args, po);
    if ((co->co_ncellvars > 0 || co->co_nfreevars > 0) &&
        psyco_generic_call(po, le_gen_record_start,
                           CfReturnTypeInt | CfPyErrIfNeg, "v", vrec) == NULL)
        goto error;
    return 1;

error:
    vinfo_decref(*ventry, po);
    *ventry = NULL;
    return -1;
}

/* le_iter_send of the real generator of the record 'rec' a frame word
   holds (yield_resumable) */
static int
record_iter_send(PyObject *rec, PyObject *v, PyObject *out[])
{
    PyObject *gen = le_gen_record_real(rec);
    int r;
    if (gen == NULL)
        return -1;
    r = le_iter_send(gen, v, out);
    Py_DECREF(gen);
    return r;
}

#if LE_STACK_SEGMENTS
/* le_iter_send, its result widened for le_call_as_direct */
static long
iter_send_long(PyObject *iter, PyObject *v, PyObject *out[])
{
    return le_iter_send(iter, v, out);
}
static long
record_iter_send_long(PyObject *rec, PyObject *v, PyObject *out[])
{
    return record_iter_send(rec, v, out);
}
#endif

/* Compiled code's step of the generator 'vgen' (next(), a for loop, or
   send(varg) when 'varg' isn't NULL), which gen_send_ex2 would run
   compiled (eval_hook.c:le_gen_iternext): a direct call of the compiled
   entry at the frame's place, doing what gen_send_ex2 and ceval's
   FOR_ITER_GEN do around the run, inline. The frame is linked to the
   thread's innermost and the generator's exception state into the
   thread's, but the frame isn't pushed: the call's record says the body
   runs on it (Python/frames.h:on_gen_frame), so that the frame walk finds
   it there. A pending generator's first step makes it a virtual one
   (vgen_start) and a virtual one's steps run on its record the same way.
   Anything else is le_gen_iternext's, or for send() the C function 'send'
   (gen_send), on the real generator. A record a frame word holds
   (yield_resumable), known by its type, is stepped as a virtual one's,
   its fallback on its real generator. With 'out', for yield from's SEND,
   the result is an int, as le_iter_send's: 1 with what the body yielded
   in out's item, 0 with what it returned. 'through_c' how CPython runs the
   step from C (next(), send()), or LE_C_NONE as FOR_ITER_GEN and SEND_GEN
   run it. */
static vinfo_t *
gen_send(PsycoObject *po, vinfo_t *vgen, vinfo_t *varg, PyCFunction send,
         vinfo_array_t *out, int through_c)
{
    const struct gen_layout *L = &gen_layout;
    PyCodeObject *co = NULL;
    vinfo_t *vobj = vgen, *vcode, *vts = NULL, *ventry = NULL, *vf = NULL;
    vinfo_t *v, *vresult = NULL, *vsent = NULL;
    PyObject *o;
    le_code_extra_t *x;
    le_gen_direct_t *t;
    stack_frame_info_t *finfo;
    Source sources[2];
    bool sends_any;
    int r;
    bool as_direct = false, held = false;
    if (PsycoGenexpr_Check(vgen) || PsycoVgen_Check(vgen)) {
        co = pending_code(vgen);
        /* a generator's body pausing would make one it steps real at
           once, its record in the body's frame: such a one is made real
           now and stepped as a real one. yield from's is kept in the
           frame's word as a record (yield_resumable). */
        if (PsycoGenexpr_Check(vgen) &&
            (!vgen_startable(vgen, co) ||
             (psyco_generator_mode(po) == GENEXPR_RESUMABLE && out == NULL))) {
            if (!compute_vinfo(vgen, po))
                return NULL;
        }
        else
            L = &record_layout;
    }
    else if (out != NULL && is_runtime(vgen->source) &&
             Psyco_KnownType(vgen) == &le_gen_record_type) {
        L = &record_layout;
        held = true;
    }
    if (L == &gen_layout && !is_runtime(vgen->source))
        goto generic;
    /* the value sent is made real before anything is changed */
    if (varg != NULL && !compute_vinfo(varg, po))
        return NULL;
    sends_any = varg != NULL &&
                !(is_compiletime(varg->source) &&
                  CompileTime_Get(varg->source)->value == (long)Py_None);
    if (L == &gen_layout || held) {
        vcode = code_of(po, vgen, held ? REC_code_word : GEN_code_word);
        if (vcode == NULL)
            return NULL;
        r = psyco_pyobj_atcompiletime_mega(po, vcode, &o);
        vinfo_decref(vcode, po);
        if (r < 0)
            return NULL;
        if (r == 0)
            goto generic;
        co = (PyCodeObject *)o;
    }
    if (!PyCode_Check(co) || (x = le_code_extra(co)) == NULL) {
        PyErr_Clear(); /* out of memory */
        goto generic;
    }
    if ((t = le_gen_direct_table(co, x)) == NULL)
        goto generic;

    vts = le_tstate_load(po);
    if (vts == NULL)
        return NULL;
    if (PsycoGenexpr_Check(vgen))
        /* not started: CPython's TypeError for anything but None */
        r = sends_any ? 0 : vgen_start(po, vgen, co, t, vts, &ventry);
    else {
        if (PsycoVgen_Check(vgen))
            vobj = vgen->array->items[iVGEN_RECORD];
        r = gen_step_tests(po, L, vobj, co, t, vts, sends_any, &ventry);
    }
#if LE_STACK_SEGMENTS
    /* with the stack low, the step through C instead, on a segment */
    if (r == 1 && through_c == LE_C_NONE &&
        runtime_condition_f(po, psyco_stack_low(po))) {
        vinfo_decref(vts, po);
        vinfo_xdecref(ventry, po);
        as_direct = true;
        goto generic;
    }
#endif
    if (r == 1)
        r = through_c != LE_C_NONE
                ? enter_c_recursive_call_or_not(po, vts, through_c)
                : enter_recursive_call_or_not(po, vts);
    if (r <= 0) {
        vinfo_decref(vts, po);
        vinfo_xdecref(ventry, po);
        if (r < 0)
            return NULL;
        goto generic;
    }
    if (PsycoVgen_Check(vgen))
        vobj = vgen->array->items[iVGEN_RECORD];
    vinfo_incref(vobj); /* until the step ends */

    /* the run, as gen_send_ex2 starts it: nothing above may respawn now */
    v = psyco_get_field(po, vts, TSTATE_exc_info);
    if (v == NULL || !psyco_put_field(po, vobj, L->exc_previous, v))
        goto error;
    vinfo_decref(v, po);
    v = integer_add_i(po, vobj, L->exc_ofs, true);
    if (v == NULL || !psyco_put_field(po, vts, TSTATE_exc_info, v))
        goto error;
    vinfo_decref(v, po);
    if (!gen_put_i(po, vobj, L->state, FRAME_EXECUTING))
        goto error_nov;
    v = gen_current_frame(po, vts);
    if (v == NULL || !psyco_put_field(po, vobj, L->previous, v))
        goto error;
    vinfo_decref(v, po);
    /* as le_gen_frame_run: the place moves off the yield, and the stack
       is held by the body */
    v = psyco_get_field(po, vobj, L->place);
    if (v != NULL) {
        vinfo_t *vnext = integer_add_i(po, v, LE_GEN_RUN_STEP, true);
        vinfo_decref(v, po);
        v = vnext;
    }
    if (v == NULL || !psyco_put_field(po, vobj, L->place, v))
        goto error;
    vinfo_decref(v, po);
#if LE_HAS_FRAME_STACKREFS
    v = integer_add_i(po, vobj,
                      L->frame_ofs + offsetof(le_frame_t, localsplus) +
                          le_code_nlocalsplus(co) * sizeof(_PyStackRef),
                      true);
    if (v == NULL || !psyco_put_field(po, vobj, L->stack, v))
        goto error;
    vinfo_decref(v, po);
#else
    if (!gen_put_i(po, vobj, L->stack,
                   LE_HAS_RUNNING_STACK_HIDDEN ? -1 : le_code_nlocalsplus(co)))
        goto error_nov;
#endif

    /* the value sent, as gen_send_ex2 sends it, and the frame */
    vsent = varg != NULL ? (vinfo_incref(varg), varg) : psyco_vi_None();
    if (!psyco_incref_v(po, vsent))
        goto error_nov;
    v = make_runtime_copy(po, vsent);
    vinfo_decref(vsent, po);
    vsent = v;
    vf = integer_add_i(po, vobj, L->frame_ofs, true);
    if (vsent == NULL || vf == NULL || !compute_vinfo(vf, po) ||
        !compute_vinfo(ventry, po))
        goto error_nov;
    if (!is_runtime(vf->source))
        psyco_fatal_msg("generator frame not at run time");
    sources[0] = vsent->source;
    sources[1] = vf->source;
    finfo = psyco_finfo_gen(po, co);
    finfo_last(finfo)->through_c = through_c != LE_C_NONE;
    vresult = psyco_call_psyco_on_frame(po, ventry, sources, 2, vf, finfo);
    vinfo_decref(vsent, po);
    vsent = NULL;
    vinfo_decref(vf, po);
    vf = NULL;
    vinfo_decref(ventry, po);
    ventry = NULL;
    vinfo_incref(vts);
    if (!(through_c != LE_C_NONE ? leave_c_recursive_call(po, vts, through_c)
                                 : leave_recursive_call(po, vts)))
        goto error_nov;

    /* paused at a yield, compiled: what gen_send_ex2 does then */
    r = gen_field_test_i(po, vobj, L->state, FRAME_EXECUTING, Py_GE);
    if (r == 0)
        r = gen_field_test_i(po, vobj, L->exc_previous, 0, Py_EQ);
    if (r < 0)
        goto error_nov;
    if (r > 0) {
        if (out != NULL)
            v = psyco_generic_call(po, L->send_status,
                                   CfReturnTypeInt | CfPyErrIfNeg, "vvvA", vts,
                                   vobj, vresult, out);
        else if (varg != NULL)
            v = psyco_generic_call(po, L->send_end, CfCommonNewRefPyObject,
                                   "vvv", vts, vobj, vresult);
        else
            v = psyco_generic_call(po, L->end, CfCommonIterNext, "vvv", vts,
                                   vobj, vresult);
        vinfo_decref(vts, po);
        vinfo_decref(vresult, po);
        vinfo_decref(vobj, po);
        return v;
    }
    v = psyco_get_field(po, vobj, L->exc_previous);
    if (v == NULL || !psyco_put_field(po, vts, TSTATE_exc_info, v))
        goto error;
    vinfo_decref(v, po);
    vinfo_decref(vts, po);
    if (!gen_put_i(po, vobj, L->exc_previous, 0) ||
        !gen_put_i(po, vobj, L->previous, 0)) {
        vinfo_decref(vresult, po);
        vinfo_decref(vobj, po);
        return NULL;
    }
    vinfo_decref(vobj, po);
    vresult->source = add_rtref(vresult->source);
    if (out != NULL) {
        out->items[0] = vresult;
        return psyco_vi_One();
    }
    return vresult;

error:
    vinfo_xdecref(v, po);
error_nov:
    vinfo_xdecref(vsent, po);
    vinfo_xdecref(vf, po);
    vinfo_xdecref(ventry, po);
    vinfo_xdecref(vresult, po);
    vinfo_decref(vts, po);
    vinfo_decref(vobj, po);
    return NULL;

generic:
#if LE_STACK_SEGMENTS
    if (as_direct && out != NULL)
        return psyco_generic_call(
            po, le_call_as_direct, CfReturnTypeInt | CfPyErrIfNeg, "lvvA",
            (long)(held ? record_iter_send_long : iter_send_long), vgen, varg,
            out);
    if (as_direct)
        return psyco_generic_call(po, le_call_as_direct, CfCommonIterNext,
                                  "lv", (long)le_gen_iternext, vgen);
#endif
    if (out != NULL)
        return psyco_generic_call(po, held ? record_iter_send : le_iter_send,
                                  CfReturnTypeInt | CfPyErrIfNeg, "vvA", vgen,
                                  varg, out);
    if (varg != NULL)
        return psyco_generic_call(po, send, CfCommonNewRefPyObject, "vv", vgen,
                                  varg);
    return psyco_generic_call(po, le_gen_iternext, CfCommonIterNext, "v",
                              vgen);
}

DEFINEFN
vinfo_t *
psyco_gen_send(PsycoObject *po, vinfo_t *vgen, vinfo_t *varg, PyCFunction send)
{
    return gen_send(po, vgen, varg, send, NULL, LE_C_SEND);
}

DEFINEFN
vinfo_t *
psyco_gen_step(PsycoObject *po, vinfo_t *vgen)
{
    return gen_send(po, vgen, NULL, NULL, NULL, LE_C_NONE);
}

DEFINEFN
vinfo_t *
psyco_gen_next(PsycoObject *po, vinfo_t *vgen)
{
    return gen_send(po, vgen, NULL, NULL, NULL, LE_C_NEXT);
}

DEFINEFN
int
le_iter_send(PyObject *iter, PyObject *v, PyObject *out[])
{
    PySendResult r = PyIter_Send(iter, v, &out[0]);
    if (r == PYGEN_ERROR)
        return -1;
    return r == PYGEN_NEXT;
}

DEFINEFN
vinfo_t *
psyco_gen_send_status(PsycoObject *po, vinfo_t *vgen, vinfo_t *varg,
                      vinfo_array_t *out)
{
    return gen_send(po, vgen, varg, NULL, out, LE_C_NONE);
}
#endif

/* whether psyco_call_genexpr can run 'v', a pending generator or
   coroutine */
static bool
pending_runnable(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *vfunc, *closure, *args, *vbuiltins;
    PyCodeObject *co;
    int nfrees;
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
    if (args == NULL || PsycoTuple_Load(args) < 0)
        return false;
    co = pending_code(v);
    nfrees = PyCode_GetNumFree(co);
    if (psyco_get_merge_points(co, 0) == Py_None || nfrees > MAX_CALL_FREES)
        return false;
    /* a virtual function's cells: its closure tuple's (load_frees) */
    if (nfrees > 0 && is_virtualtime(vfunc->source)) {
        closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
        if (closure == NULL || PsycoTuple_Load(closure) != nfrees)
            return false;
    }
    /* psyco_call_genexpr's builtins: of a function known now or still
       virtual (psyco_call_pyfunc), so with nothing emitted */
    if (callee_builtins(po, vfunc, v->array->items[iGENEXPR_GLOBALS],
                        &vbuiltins) != 1)
        return false;
    vinfo_decref(vbuiltins, po);
    return true;
}

DEFINEFN
bool
psyco_genexpr_fusable(PsycoObject *po, vinfo_t *v)
{
    return pending_runnable(po, v) &&
           (pending_code(v)->co_flags & CO_GENERATOR) &&
           psyco_is_fusable_generator(pending_code(v));
}

/* whether a type site in the fused code of 'v' (fusable) has seen too
   many types (mergepoints.h:psyco_fuse_stopped), tested at run time: read
   now, the flag could change between a compile and its replay */
static int
fuse_stopped(PsycoObject *po, vinfo_t *v)
{
    PyCodeObject *co = pending_code(v);
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

/* the fused body is the generator's frame: a tracer, or another eval
   hook, must see it */
DEFINEFN
int
psyco_genexpr_fuse(PsycoObject *po, vinfo_t *v)
{
    int on;
    vinfo_t *vts;
    if (!psyco_genexpr_fusable(po, v))
        return 0;
    vts = load_tstate(po);
    if (vts == NULL)
        return -1;
    on = direct_call_refused(po, vts, pending_code(v));
    vinfo_decref(vts, po);
    if (on)
        return 0;
    on = fuse_stopped(po, v);
    return on < 0 ? -1 : !on;
}

/* A coroutine made by an await's call (awaited_at_once) can run its body
   as a call from the await (pycompiler.c, YIELD_FROM or SEND): nothing can
   see the coroutine object CPython would have made while the body runs,
   and one is made if the body suspends (pycompiler.c:yield_direct), or
   something could find it (frames.c:psyco_coros_realise). Not where
   CPython's coroutine would record its origin
   (sys.set_coroutine_origin_tracking_depth) or a wrapper would get it,
   nor while tracing, nor once a type site in its body has seen too many
   types (fuse_stopped): there the coroutine is made and sent into. Emits
   those tests: 1 to run the body, 0 for the usual await, -1 with an
   exception. */
#define TSTATE_origin_depth                                             \
    FMUT(DEF_FIELD(PyThreadState, int, coroutine_origin_tracking_depth, \
                   NO_PREV_FIELD))
#if LE_HAS_COROUTINE_WRAPPER
#define TSTATE_coroutine_wrapper                                 \
    FMUT(DEF_FIELD(PyThreadState, PyObject *, coroutine_wrapper, \
                   NO_PREV_FIELD))
#endif
DEFINEFN
int
psyco_coro_start(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *vts, *vfield;
    condition_code_t cc;
    int on;
    /* the function is known, for the coroutine made if it is realised
       (frames.c:psyco_coros_realise) */
    if (!pending_runnable(po, v) ||
        !(pending_code(v)->co_flags & CO_COROUTINE) ||
        !is_compiletime(v->array->items[iGENEXPR_FUNC]->source))
        return 0;
    vts = load_tstate(po);
    if (vts == NULL)
        return -1;
    on = tracing_on(po, vts, pending_code(v));
    if (!on) {
        vfield = psyco_get_field(po, vts, TSTATE_origin_depth);
        cc = vfield == NULL ? CC_ERROR : integer_non_null(po, vfield);
        vinfo_xdecref(vfield, po);
        on = cc == CC_ERROR ? -1 : runtime_condition_f(po, cc);
    }
#if LE_HAS_COROUTINE_WRAPPER
    if (!on) {
        vfield = psyco_get_field(po, vts, TSTATE_coroutine_wrapper);
        cc = vfield == NULL ? CC_ERROR : object_non_null(po, vfield);
        vinfo_xdecref(vfield, po);
        on = cc == CC_ERROR ? -1 : runtime_condition_f(po, cc);
    }
#endif
    vinfo_decref(vts, po);
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
    psyco_assert(le_eval_breaker_check());
#if LE_HAS_MONITORING
    le_tracing_recheck(NULL); /* the first version seen */
#endif
    cannot_block_init();
#if LE_RESUMES_GENERATORS_DIRECTLY
    gen_layouts_init();
#endif
}
