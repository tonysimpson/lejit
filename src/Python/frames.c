/*
 * Makes compiled frames visible to Python. stack_frame_info_t records (code,
 * globals) for each compiled call, and the machine stack is walked through the
 * finfo slot that each Psyco frame keeps (x64/iprocessor.c). sys._getframe
 * is replaced by le_getframe, which counts compiled frames and emulates
 * them. Each real frame LeCode_Run runs has a record on LeCode_Run's C
 * stack (FrameRuntime) in a list per OS thread, innermost first
 * (frame_runs): a lookup scans it for the frame, and nearly all are for
 * the current frame, at the head. A compiled callee's frame object is kept
 * from when it is first asked for until its call returns, which a replaced
 * return address tells (kept_frame, psyco_activation_returned): one object for
 * the call's life, as CPython's, so f_trace and identity stay and ceval goes
 * on with it when tracing starts; it is running for frame.clear(), and its
 * line and locals are read again when asked for. An inlined frame's call has
 * no end that can be seen, so its frame objects are new each time. An emulated
 * frame's f_lasti is the call it is in: each record says where the frame out
 * of it called (back_lasti), and the innermost frame notes its place in its
 * slot before a residual call that may run Python
 * (codegen.c:psyco_note_call_site); before its first, f_lineno is the
 * first line. The note and the record also name a map of where the
 * frame's locals are during that call (struct locals_map_s), read when
 * f_locals is asked for (slot_locals): a stack slot, a constant, a
 * virtual value's parts, or a callee-saved register, which the C frames
 * in between keep or saved; those are found by unwinding them with
 * libunwind (slot_registers). Code naming locals, vars, dir, eval or exec
 * is not compiled; reached by another name, they run with the frame
 * emulated and current (cimpl_with_frame). Every read of a frame's
 * locals (f_locals, those builtins, gc.get_referents) writes them in
 * first, as they are then (view_sync). A traceback's frame gets
 * the locals when the exception leaves the frame (psyco_frame_write_locals);
 * the frame that catches it is held by its catcher word until its call
 * ends and reads them like an emulated frame (CATCHER_NOTE), and gets them
 * at the end while something else holds it (pycompiler.c:catcher_word); a
 * virtual list becomes real as it becomes one of its locals
 * (catcher_local_readable). Known gaps: virtual lists in other frames,
 * inlined frames' locals, a read from another thread or
 * from Python run outside a call that may run it (a finaliser), which
 * see the last written.
 * frame.f_back, frame.f_lineno, frame.f_lasti, frame.f_locals and
 * frame.clear are replaced in PyFrame_Type's dict: a real frame entered
 * through C from compiled code keeps the f_back ceval gave it, which the
 * evaluators put tstate->frame back to, and the getter gives the compiled
 * frames it skips (frame_get_back); a real frame LeCode_Run runs gets its
 * current line from LeJIT's chain (frame_get_lineno). A traceback frame gets
 * the entry's over it as f_back, or the caller for the one that catches
 * (psyco_frame_link_callee, catcher_caller). C code reading f_back,
 * f_lasti or f_localsplus itself bypasses the getters: _warnings.warn runs
 * with every frame linked and placed (le_warn), a warning C raises is
 * given its place when shown (le_showwarnmsg), gc.get_referents runs with
 * its frames' locals written (le_get_referents); other such C code
 * (PyEval_GetFrame, a freshly imported _warnings, a C extension's
 * PyFrame_FastToLocals) still skips compiled frames, and reads the locals
 * last written.
 * When tracing starts, a compiled frame goes on in ceval
 * (psyco_resume_run), in its kept frame or catcher's frame if it has one;
 * pvisitframes then
 * hides its compiled record, since the real frame now stands for it. A real
 * frame LeCode_Run runs is marked resumed instead: its locals and line are
 * then ceval's, and a trace function's f_locals must not hand ceval the locals
 * compiled code had at the call, which PyFrame_LocalsToFast would write back.
 */
#include "frames.h"
#include "genrecord.h"
#include "../Objects/pobject.h"
#include "../codemanager.h"
#include "../vcompiler.h"
#include "pyver.h"
#include "pycheader.h"
#include "pycinternal.h"
/* le_frame_t and its accessors */
#include "../mechanisms/frames.h"
/* le_recursion_suspend and le_recursion_resume */
#include "../codegen.h"
#include "../mechanisms/tracing.h"
#include "pycompiler.h"
#include "../bytecode.h"
#include "../mergepoints.h"
#include "../dispatcher.h"
#include "../processor.h"
#include "../mechanisms/recursion.h"
#include "../segments.h"
/* after Python.h, for _GNU_SOURCE: ucontext_t's REG_* */
#define UNW_LOCAL_ONLY
#include <dlfcn.h>
#include <ipatch.h>
#include <libunwind.h>

/***************************************************************/

DEFINEFN
int
psyco_call_lasti(PyCodeObject *co, int pos)
{
    le_code_t c = le_code(co);
    pos &= ~NEXT_INSTR_POP;
    if (pos < 0 || pos >= c.size)
        return -1;
    return le_instr(c, pos).next - LE_UNIT;
}

/* the place of an inlined frame's parent while it calls it, or -1 */
static int
inline_parent_lasti(PsycoObject *po)
{
    vinfo_t *v = po->vlocals.items[INDEX_LOC_INLINING];
    vinfo_t *co;
    if (v == NULL || v->array->count <= LOC_INLINE_NEXT_INSTR ||
        v->array->items[LOC_INLINE_NEXT_INSTR] == NULL)
        return -1;
    co = v->array->items[LOC_INLINE_CO];
    return psyco_call_lasti(
        (PyCodeObject *)CompileTime_Get(co->source)->value,
        (int)CompileTime_Get(v->array->items[LOC_INLINE_NEXT_INSTR]->source)
            ->value);
}

/* the function of the frame inlined in 'po', where the call knows it */
static PyFunctionObject *
inlined_function(PsycoObject *po)
{
    vinfo_t *v = po->vlocals.items[INDEX_LOC_INLINING];
    return (PyFunctionObject *)CompileTime_Get(
               v->array->items[LOC_INLINE_FUNC]->source)
        ->value;
}

/* the entry after a record's first for the inlined frame 'caller' runs
   in, which made the call */
static void
finfo_inlined_caller(stack_frame_info_t *p, PsycoObject *caller)
{
    Source sglobals = caller->vlocals.items[INDEX_LOC_GLOBALS]->source;
    p->co = caller->pr.co;
    p->func = inlined_function(caller);
    p->nargs = -1;
    p->own_lasti = -1;
    p->back_lasti = inline_parent_lasti(caller);
    if (is_compiletime(sglobals))
        p->globals = (PyObject *)CompileTime_Get(sglobals)->value;
    else
        p->globals = NULL; /* uncommon */
}

/* held by the code that passes it, so it lives as long as that code's
   owner (codemanager.h). Each entry says where the frame out of it is,
   for f_lineno: the innermost compiled frame's own place isn't known. */
DEFINEFN
stack_frame_info_t *
psyco_finfo(PsycoObject *caller, PsycoObject *callee)
{
    Source sglobals;
    stack_frame_info_t *p;
    int inlining = caller != NULL && caller->pr.is_inlining;

    p = psyco_owner_alloc((inlining + 1) * sizeof(stack_frame_info_t));
    memset(p, 0, (inlining + 1) * sizeof(stack_frame_info_t));
    p->link_stack_depth = -inlining;
    p->nargs = -1;
    p->own_lasti = -1;
    p->co = callee->pr.co;
    p->ret_below = psyco_ret_below(caller != NULL ? caller : callee);
    p->back_lasti =
        caller != NULL ? psyco_call_lasti(caller->pr.co, caller->pr.next_instr)
                       : inline_parent_lasti(callee);
    sglobals = callee->vlocals.items[INDEX_LOC_GLOBALS]->source;
    if (is_compiletime(sglobals))
        p->globals = (PyObject *)CompileTime_Get(sglobals)->value;
    else
        p->globals = NULL; /* uncommon */
    if (inlining)
        finfo_inlined_caller(p + 1, caller);
    else if (caller == NULL && callee->pr.is_inlining)
        p->func = inlined_function(callee);
    return p;
}

#if LE_RESUMES_GENERATORS_DIRECTLY
/* its globals are its frame's (pvisitframes) */
DEFINEFN
stack_frame_info_t *
psyco_finfo_gen(PsycoObject *caller, PyCodeObject *co)
{
    stack_frame_info_t *p;
    int inlining = caller->pr.is_inlining;
    p = psyco_owner_alloc((inlining + 1) * sizeof(stack_frame_info_t));
    memset(p, 0, (inlining + 1) * sizeof(stack_frame_info_t));
    p->link_stack_depth = -inlining;
    p->nargs = -1;
    p->own_lasti = -1;
    p->co = co;
    p->ret_below = psyco_ret_below(caller);
    p->back_lasti = psyco_call_lasti(caller->pr.co, caller->pr.next_instr);
    p->on_gen_frame = 1;
    if (inlining)
        finfo_inlined_caller(p + 1, caller);
    return p;
}
#endif

/* Read and written at every entry into compiled code, so kept in the
   static TLS block (initial-exec), read straight off %fs, not looked up
   through __tls_get_addr. Both take a few bytes of glibc's reserve for
   modules loaded later. */
#define LE_FAST_TLS __attribute__((tls_model("initial-exec")))

/* the records of the real frames LeCode_Run runs on this thread,
   innermost first */
static _Thread_local FrameRuntime *frame_runs LE_FAST_TLS;

/* Every thread's frame_runs, for the owner scan (psyco_frames_running):
   its address is in a capsule in the thread state's dict, which goes when
   the thread state is cleared, before the thread ends. */
static _Thread_local char runs_listed LE_FAST_TLS;
static PyObject *runs_key; /* a key no one else has */
static bool runs_lost;     /* a thread's frame_runs couldn't be listed */

static void
runs_unlisted(PyObject *capsule)
{
    if (PyCapsule_GetPointer(capsule, NULL) == (void *)&frame_runs)
        runs_listed = 0; /* on its own thread: listed again if it runs on */
}

static __attribute__((noinline)) void
runs_list(void)
{
    PyObject *t, *v, *tb, *d, *c = NULL;
    PyErr_Fetch(&t, &v, &tb);
    if (runs_key == NULL)
        runs_key = PyCapsule_New(&runs_key, NULL, NULL);
    d = PyThreadState_GetDict();
    if (runs_key == NULL || d == NULL ||
        (c = PyCapsule_New((void *)&frame_runs, NULL, runs_unlisted)) ==
            NULL ||
        PyDict_SetItem(d, runs_key, c) < 0) {
        PyErr_Clear();
        runs_lost = true;
    }
    Py_XDECREF(c);
    runs_listed = 1;
    PyErr_Restore(t, v, tb);
}

/* at most this many runs of compiled code on one thread are unwound */
#define MAX_RUNS 64
/* the code objects of one compiled frame and the frames inlined in it */
#define LE_FRAME_CODES_MAX 8
static int
runs_here(stack_frame_info_t **roots[MAX_RUNS],
          stack_frame_info_t **inners[MAX_RUNS], stack_frame_info_t **upto);
static int
runs_unwind(stack_frame_info_t **roots[], stack_frame_info_t **inners[], int n,
            code_t *ips[], long regs[REG_TOTAL]);

/* the code objects of the compiled frame whose slot holds 'rec', a
   record of a compiled call it makes or its inlined frame's own: those of
   the frames inlined in it, after its own 'co' */
static int
frame_codes(PyCodeObject *co, stack_frame_info_t *rec, PyCodeObject **cos)
{
    stack_frame_info_t *fd, *last;
    int n = 0;
    cos[n++] = co;
    if (rec == NULL)
        return n;
    last = finfo_last(rec);
    for (fd = rec->link_stack_depth == 0 ? rec : rec + 1; fd <= last; fd++)
        if (n < LE_FRAME_CODES_MAX)
            cos[n++] = fd->co;
    return n;
}

DEFINEFN int
psyco_frames_running(void (*mark)(PyCodeObject **cos, int ncos, code_t *at,
                                  void *arg),
                     void *arg, bool where)
{
    PyThreadState *ts, *here = PyThreadState_GET();
    stack_frame_info_t **roots[MAX_RUNS], **inners[MAX_RUNS];
    code_t *ips[MAX_RUNS];
    int nhere = 0, nfound = 0;
    if (runs_lost)
        return -1;
    if (runs_key == NULL)
        return 0;
    /* this thread's innermost frames are in C calls whose return
       addresses only unwinding finds; another thread's are not known */
    if (where) {
        nhere = runs_here(roots, inners, NULL);
        nfound = runs_unwind(roots, inners, nhere, ips, NULL);
    }
    for (ts = PyInterpreterState_ThreadHead(PyInterpreterState_Main());
         ts != NULL; ts = PyThreadState_Next(ts)) {
        PyObject *c =
            ts->dict == NULL ? NULL : PyDict_GetItem(ts->dict, runs_key);
        FrameRuntime *fr;
        if (c == NULL)
            continue; /* it never ran compiled code */
        for (fr = *(FrameRuntime **)PyCapsule_GetPointer(c, NULL); fr != NULL;
             fr = fr->next) {
            stack_frame_info_t **f1;
            PyCodeObject *co = fr->psy_code, *cos[LE_FRAME_CODES_MAX];
            code_t *at;
            int i;
            if (fr->resumed) {
                mark(&co, 1, NULL, arg);
                continue;
            }
            /* each frame making a compiled call goes on where the call's
               record says it returns */
            for (f1 = *fr->psy_frames_start;
                 (((long)*f1) & 1) == 0 && (*f1)->link_stack_depth != 0;
                 f1 = psyco_next_stack_frame(f1)) {
                mark(cos, frame_codes(co, *f1, cos), (*f1)->ret_addr, arg);
                co = (*f1)->co;
            }
            /* the innermost, in a call to C */
            at = NULL;
            if (ts == here)
                for (i = 0; i < nfound; i++)
                    if (inners[i] == f1) {
                        at = ips[i];
                        break;
                    }
            mark(cos,
                 frame_codes(co, (((long)*f1) & 1) == 0 ? *f1 : NULL, cos), at,
                 arg);
        }
    }
    return 0;
}

/* Every thread's runs by frame, for psyco_frame_start_get: a frame that
   isn't a run's is looked up too, often, as a traceback's are when a deep
   one has its locals written, and a search of this thread's list goes all
   through it then */
#define RUN_HASHES 1024
static FrameRuntime *runs_by_frame[RUN_HASHES];

static FrameRuntime **
runs_hashed(le_frame_t *f)
{
    uintptr_t a = (uintptr_t)f;
    return &runs_by_frame[((a >> 4) ^ (a >> 14)) % RUN_HASHES];
}

/* the run is no longer found by its frame */
static void
run_unhash(FrameRuntime *fr)
{
    FrameRuntime **p;
    if (fr->frame == NULL)
        return;
    for (p = runs_hashed(fr->frame); *p != fr;) p = &(*p)->same_hash;
    *p = fr->same_hash;
}

DEFINEFN void
psyco_frame_start_register(FrameRuntime *fr, le_frame_t *f,
                           stack_frame_info_t ***psy_frames_start)
{
    fr->frame = f;
    fr->psy_frames_start = psy_frames_start;
    fr->psy_code = le_frame_code(f);
    fr->psy_globals = le_frame_globals(f);
    fr->entry_locals = NULL;
    fr->n_entry_locals = 0;
    fr->resumed = 0;
    fr->exposed = 0;
    fr->walked = NULL;
    fr->word = NULL;
    fr->next = frame_runs;
    frame_runs = fr;
    fr->list = &frame_runs;
    fr->same_hash = *runs_hashed(f);
    *runs_hashed(f) = fr;
    if (fr->next == NULL && !runs_listed) {
        runs_list();
#if LE_STACK_SEGMENTS
        le_stack_init();
#endif
    }
}

#if LE_STACK_SEGMENTS
DEFINEFN long
psyco_direct_calls_size(const char *lo, const char *hi,
                        const le_direct_entry_t *e)
{
    FrameRuntime *fr;
    long size = 0;
    bool spanned = false;
#define ON_STACK(p) \
    ((p) != NULL && (const char *)(p) >= lo && (const char *)(p) < hi)
    for (fr = frame_runs; ON_STACK(fr); fr = fr->next) {
        stack_frame_info_t **f1;
        bool inside = spanned;
        /* a run that le_call_as_direct entered: all from the run it was
           called from, whose own calls that leaves no need to count */
        while (ON_STACK(e) && (const char *)e < (const char *)fr) e = e->next;
        spanned = false;
        if (ON_STACK(e)) {
            if (!ON_STACK(fr->next))
                size += (const char *)e - (const char *)fr;
            else if ((const char *)e < (const char *)fr->next) {
                size += (const char *)fr->next - (const char *)fr;
                spanned = true;
            }
        }
        if (inside || fr->resumed)
            continue;
        for (f1 = *fr->psy_frames_start; (((long)*f1) & 1) == 0;
             f1 = psyco_next_stack_frame(f1)) {
            stack_frame_info_t *last = finfo_last(*f1);
            if (last->link_stack_depth == 0)
                break; /* stack top is an inline frame */
            if (!last->through_c)
                size += last->link_stack_depth;
        }
    }
    return size;
#undef ON_STACK
}
#endif

static void
walked_end(struct walked_calls_s *w);

DEFINEFN FrameRuntime *
psyco_frame_start_get(le_frame_t *f)
{
    FrameRuntime *fr;
    for (fr = *runs_hashed(f); fr != NULL; fr = fr->same_hash)
        if (fr->frame == f && fr->list == &frame_runs)
            return fr;
    return NULL;
}

DEFINEFN void
psyco_frame_start_unregister(FrameRuntime *fr)
{
    int i;
    /* fr is the innermost unless the C stack was switched under it */
    if (frame_runs == fr)
        frame_runs = fr->next;
    else {
        FrameRuntime **p = &frame_runs;
        while (*p != fr) p = &(*p)->next;
        *p = fr->next;
    }
    run_unhash(fr);
    if (fr->walked != NULL)
        walked_end(fr->walked);
#ifdef PyFrameLocalsProxy_Check
    if (fr->exposed)
        psyco_frames_exposed--;
#endif
    if (fr->entry_locals == NULL)
        return;
    for (i = 0; i < fr->n_entry_locals; i++) Py_XDECREF(fr->entry_locals[i]);
    PyMem_Free(fr->entry_locals);
}

#if LE_RESUMES_GENERATORS_DIRECTLY
static PyObject ***
gen_keep_of(le_frame_t *f, int **n);
#endif

/* where what the compiled code running the real frame 'f' borrows from it
   is kept alive (psyco_frame_write_locals): its FrameRuntime's, or the
   activation's of a generator's body called directly on it; NULL if it
   has neither */
static PyObject ***
run_keep_of(le_frame_t *f, int **n)
{
    FrameRuntime *fr = psyco_frame_start_get(f);
    if (fr != NULL) {
        *n = &fr->n_entry_locals;
        return &fr->entry_locals;
    }
#if LE_RESUMES_GENERATORS_DIRECTLY
    if (le_frame_is_gen(f))
        return gen_keep_of(f, n);
#endif
    return NULL;
}

/* The real frame's f_localsplus holds what compiled code was started
   with, which it borrows (arguments, cells) until it returns: the first
   write keeps those alive in its FrameRuntime, released with it. */
DEFINEFN
void
psyco_frame_write_locals(PyFrameObject *f, int start, PyObject **values,
                         int count)
{
    PyCodeObject *co = le_pyframe_code(f);
    le_frame_t *frame = le_frame_of(f);
    int n = le_code_nlocalsplus(co);
    int i, *nkeep = NULL;
    /* an emulated frame isn't executing, and the first write is from 0 */
    PyObject ***kept = start == 0 && le_frame_is_executing(f)
                           ? run_keep_of(le_frame_of(f), &nkeep)
                           : NULL;
    if (kept != NULL && *kept == NULL) {
        PyObject **keep = PyMem_Malloc(n * sizeof(PyObject *) + 1);
        if (keep == NULL)
            return; /* the locals stay as they were */
        for (i = 0; i < n; i++) {
            keep[i] = LE_FRAME_WORD(frame, i);
            Py_XINCREF(keep[i]);
        }
        *kept = keep;
        *nkeep = n;
    }
    for (i = 0; i < count && start + i < n; i++) {
        if (values[i] == LE_LOCAL_UNKNOWN)
            continue;
        /* NULL: unbound, or deleted since the last write */
        Py_XINCREF(values[i]);
        le_frame_word_set(frame, start + i, values[i]);
    }
}

/* What Python wrote in the locals the run holds while the body ran
   (psyco_frame_write_locals) is released as the run ends
   (psyco_frame_start_unregister), not here, where it could run code: the
   slots take the compiled code's values after this. */
#if LE_RESUMES_ON_FRAME
DEFINEFN void
psyco_gen_take_written(le_frame_t *f, const le_gen_taken_t *t)
{
    int *nkeep = NULL;
    PyObject ***kept = run_keep_of(f, &nkeep);
    int i;
    PyObject **keep;
    if (kept == NULL || *kept == NULL)
        return;
    keep = PyMem_Realloc(*kept, (*nkeep + t->ntaken) * sizeof(PyObject *));
    for (i = 0; i < t->ntaken; i++) {
        PyObject *o = le_frame_word_take(f, t->taken[i]);
        if (o == NULL)
            continue;
        if (keep != NULL)
            keep[(*nkeep)++] = o;
        else
            Py_DECREF(o); /* out of memory */
    }
    if (keep != NULL)
        *kept = keep;
}

#else
DEFINEFN void
psyco_gen_take_written(FrameRuntime *fr)
{
    le_frame_t *f = fr->frame;
    int i, n = le_code_nlocals(le_frame_code(f));
    PyObject **keep;
    if (fr->entry_locals == NULL)
        return;
    keep = PyMem_Realloc(fr->entry_locals,
                         (fr->n_entry_locals + n) * sizeof(PyObject *));
    for (i = 0; i < n; i++) {
        PyObject *o = le_frame_word_take(f, i);
        if (o == NULL)
            continue;
        if (keep != NULL)
            keep[fr->n_entry_locals++] = o;
        else
            Py_DECREF(o); /* out of memory */
    }
    if (keep != NULL)
        fr->entry_locals = keep;
}
#endif

/* The runs of compiled code on this thread, innermost first (from the
   real frame LeCode_Run runs), up to 'upto' if it is one's innermost
   slot: each one's first slot in roots[], its innermost compiled frame's
   slot in inners[]; how many */
static int
runs_here(stack_frame_info_t **roots[MAX_RUNS],
          stack_frame_info_t **inners[MAX_RUNS], stack_frame_info_t **upto)
{
    le_frame_t *f = le_frame_current(PyThreadState_GET());
    stack_frame_info_t **f1;
    int n = 0;
    for (; f != NULL && n < MAX_RUNS; f = le_frame_back(f)) {
        FrameRuntime *fr = psyco_frame_start_get(f);
        if (fr == NULL || !le_frame_executing(f))
            continue;
        roots[n] = *fr->psy_frames_start;
        for (f1 = roots[n]; (((long)(*f1)) & 1) == 0;
             f1 = psyco_next_stack_frame(f1))
            if ((*f1)->link_stack_depth == 0)
                break;
        inners[n] = f1;
        if (inners[n++] == upto)
            break;
    }
    return n;
}

/* whether 'ip' is in a loaded object, not compiled code. dladdr searches
   the object's symbols, so the addresses found in one are remembered: the
   same few return addresses come up in every unwinding. One remembered
   is not trusted in a region of compiled code, where an object unloaded
   since may have been. */
#define LOADED_IPS 4096
static bool
in_loaded_object(unw_word_t ip)
{
    static unw_word_t loaded[LOADED_IPS];
    unw_word_t *seen = &loaded[(ip ^ (ip >> 12)) % LOADED_IPS];
    Dl_info info;
    if (*seen == ip && !psyco_in_code_regions((void *)ip))
        return true;
    if (dladdr((void *)ip, &info) == 0)
        return false;
    *seen = ip;
    return true;
}

/* Where the innermost compiled frame of each of the first 'n' runs
   runs_here found called C, found by unwinding the C frames from here
   down to it: the call's return address in ips[i], and for the last
   run the callee-saved registers it called with, which C keeps or saves
   in its frames. LeJIT's code has no unwind information, so each run is
   stepped over from where its glue saved the registers of the C code
   that called it (iprocessor.c:write_glue_run_code_fn). How many runs
   were found, from the innermost: fewer if the walk doesn't come out
   where expected. */
static int
runs_unwind(stack_frame_info_t **roots[], stack_frame_info_t **inners[], int n,
            code_t *ips[], long regs[REG_TOTAL])
{
    static bool cached;
    unw_context_t uc;
    unw_cursor_t c;
    int i, steps;
    /* libunwind's global cache blocks signals around each use of it, a
       system call each time */
    if (!cached) {
        unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
        cached = true;
    }
    if (unw_getcontext(&uc) < 0 || unw_init_local(&c, &uc) < 0)
        return 0;
    for (i = 0; i < n; i++) {
        unw_word_t sp, ip, v;
        long *saved = (long *)roots[i], *ret;
        /* the first frame outside every loaded object is compiled code */
        for (steps = 0;; steps++) {
            if (unw_get_reg(&c, UNW_REG_IP, &ip) < 0)
                return i;
            if (!in_loaded_object(ip))
                break;
            if (steps > 4096 || unw_step(&c) <= 0)
                return i;
        }
        if (unw_get_reg(&c, UNW_X86_64_RSP, &sp) < 0 ||
            sp >= (unw_word_t)inners[i] || (unw_word_t)inners[i] - sp > 65536)
            return i;
        ips[i] = (code_t *)ip;
        if (i == n - 1) {
            static const int nums[] = {UNW_X86_64_RBX, UNW_X86_64_RBP,
                                       UNW_X86_64_R12, UNW_X86_64_R13,
                                       UNW_X86_64_R14, UNW_X86_64_R15};
            static const int regnums[] = {REG_X64_RBX, REG_X64_RBP,
                                          REG_X64_R12, REG_X64_R13,
                                          REG_X64_R14, REG_X64_R15};
            int k;
            if (regs == NULL)
                return n;
            for (k = 0; k < REG_TOTAL; k++) regs[k] = 0;
            for (k = 0; k < 6; k++) {
                if (unw_get_reg(&c, nums[k], &v) < 0)
                    return i;
                regs[regnums[k]] = (long)v;
            }
            return n;
        }
        /* above the slot: R15, R14, R13, R12, RBX, RBP, [pad,] return */
        ret = saved + 7;
        if (*ret == LE_GLUE_PAD)
            ret++;
        uc.uc_mcontext.gregs[REG_R15] = saved[1];
        uc.uc_mcontext.gregs[REG_R14] = saved[2];
        uc.uc_mcontext.gregs[REG_R13] = saved[3];
        uc.uc_mcontext.gregs[REG_R12] = saved[4];
        uc.uc_mcontext.gregs[REG_RBX] = saved[5];
        uc.uc_mcontext.gregs[REG_RBP] = saved[6];
        uc.uc_mcontext.gregs[REG_RIP] = *ret;
        uc.uc_mcontext.gregs[REG_RSP] = (long)(ret + 1);
        if (unw_init_local2(&c, &uc, 0) < 0)
            return i + 1;
    }
    return n;
}

/* The callee-saved registers of the compiled frame whose slot is
   'slot', as it called C, and where that call returns to (runs_unwind).
   False if that frame isn't the innermost of a run, or the walk doesn't
   come out where expected. */
static bool
slot_registers(stack_frame_info_t **slot, long regs[REG_TOTAL],
               code_t **ret_ip)
{
    stack_frame_info_t **roots[MAX_RUNS], **inners[MAX_RUNS];
    code_t *ips[MAX_RUNS];
    int n = runs_here(roots, inners, slot);
    if (n == 0 || inners[n - 1] != slot ||
        runs_unwind(roots, inners, n, ips, regs) < n)
        return false;
    *ret_ip = ips[n - 1];
    return true;
}

static vinfo_t *
lm_vinfo(long **p, const long *regs)
{
    long *it = *p;
    vinfo_t *vi;
    int i;
    *p += 3;
    switch (it[0]) {
        case LM_STACK:
            return vinfo_new(RunTime_NewStack((int)it[1], false, false));
        case LM_REG:
            if (regs == NULL || it[1] < 0 || it[1] >= REG_TOTAL)
                return NULL;
            return vinfo_new(CompileTime_New(regs[it[1]]));
        case LM_CONST:
            return vinfo_new(CompileTime_New(it[1]));
        case LM_VIRTUAL:
            vi = vinfo_new(VirtualTime_New((source_virtual_t *)it[1]));
            vi->array = array_new((int)it[2]);
            for (i = 0; i < it[2]; i++)
                vi->array->items[i] = lm_vinfo(p, regs);
            return vi;
        default:
            return NULL;
    }
}

static int
n_locals_plus(PyCodeObject *co)
{
    return le_code_nlocalsplus(co);
}

/* The locals of the compiled frame of code 'co' whose stack slot is
   'slot', as new references in values[n_locals_plus(co)], NULL where
   unknown (unbound, dead, or a virtual value not computable here). False
   if the slot doesn't say: the frame is between calls that may run
   Python. Read while the frame is in that call. With 'own', the frame is
   inlined in the frame whose slot it is, and its own record says. */
static bool
slot_locals(stack_frame_info_t **slot, PyCodeObject *co, PyObject **values,
            int own)
{
    long v = (long)*slot;
    struct locals_map_s *map;
    PyObject *t, *val, *tb;
    long *p, regs[REG_TOTAL], *pregs = NULL;
    char *data;
    int i, n = n_locals_plus(co);
    if (v == -1 || v == FINFO_RETURNING)
        return false;
    if (own) {
        stack_frame_info_t *rec = (stack_frame_info_t *)v, *in;
        code_t *ip;
        if ((v & 1) != 0)
            return false;
        in = rec->link_stack_depth == 0 ? rec : rec + 1;
        map = in->own_locals;
        if ((rec->link_stack_depth != 0 && rec->link_stack_depth != -1) ||
            in->co != co || map == NULL || map->nvalues != n)
            return false;
        /* at a call through C, as for its parent's (below) */
        if (in == rec && rec->back_locals->retaddr != NULL) {
            if (!slot_registers(slot, regs, &ip) ||
                ip != rec->back_locals->retaddr)
                return false;
            pregs = regs;
        }
    }
    else if ((v & 1) != 0) {
        /* a note is for the call the frame is in only if that call returns
           where the note's did */
        code_t *ip;
        map = psyco_note_map(v);
        if (map->nvalues != n || !slot_registers(slot, regs, &ip) ||
            ip != map->retaddr)
            return false;
        pregs = regs;
    }
    else {
        code_t *ip;
        map = ((stack_frame_info_t *)v)->back_locals;
        if (map == NULL || map->nvalues != n)
            return false;
        /* an inlined frame's call through C, which may have left the
           parent's locals in callee-saved registers, as for a note */
        if (map->retaddr != NULL) {
            if (!slot_registers(slot, regs, &ip) || ip != map->retaddr)
                return false;
            pregs = regs;
        }
    }
    data = (char *)slot - map->below;
    p = map->items;
    PyErr_Fetch(&t, &val, &tb);
    for (i = 0; i < n; i++) {
        vinfo_t *vi = lm_vinfo(&p, pregs);
        values[i] = NULL;
        if (vi == NULL)
            continue;
        values[i] = direct_xobj_vinfo(vi, data);
        vinfo_decref(vi, NULL);
        if (values[i] == NULL)
            PyErr_Clear();
        else if (i >= le_code_nlocals(co) && !PyCell_Check(values[i]))
            Py_CLEAR(values[i]);
    }
    PyErr_Restore(t, val, tb);
    return true;
}

/* a generator's, coroutine's or async generator's frame keeps its own
   locals, needing no catcher word to stand in */
static bool
keeps_own_locals(PyCodeObject *co)
{
    return (co->co_flags &
            (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) != 0;
}

/* the catcher word (pycompiler.c:catcher_word) of the compiled frame of
   code 'co' whose own slot is 'slot', while it is in the call its slot
   says, if its frame has one and it is free; else NULL. Any map the slot
   names places the word, which stays where the frame's header put it; a
   note, which may be left from a call that has returned, is checked as
   slot_locals checks it, by unwinding, only for a free word. */
static PyObject **
slot_word(stack_frame_info_t **slot, PyCodeObject *co)
{
    long v = (long)*slot, regs[REG_TOTAL];
    struct locals_map_s *map;
    PyObject **word;
    code_t *ip;
    if (v == -1 || v == FINFO_RETURNING || keeps_own_locals(co))
        return NULL;
    map = (v & 1) != 0 ? psyco_note_map(v)
                       : ((stack_frame_info_t *)v)->back_locals;
    if (map == NULL || map->word < 0 || map->nvalues != n_locals_plus(co))
        return NULL;
    word = (PyObject **)((char *)slot - map->below + map->word);
    if (*word != NULL)
        return NULL;
    if ((v & 1) != 0 ? !slot_registers(slot, regs, &ip) || ip != map->retaddr
                     : map->retaddr != NULL)
        return NULL;
    return word;
}

/* A frame object made for a compiled call that is running, which may be
   held past the call's end, keeps the call's locals as they end, as
   CPython's frame does: the call's catcher word holds it, where the frame
   has one that is free, and the call's end gives it the locals
   (pycompiler.c:catcher_flush). */
static void
stand_in_word(PyFrameObject *f, PyObject **word)
{
    if (word == NULL || *word != NULL)
        return;
    Py_INCREF(f);
    *word = (PyObject *)f;
}

/* the frame object of the compiled callee whose own slot is 'own' stands
   for its call (stand_in_word) */
static void
stand_for_call(PyFrameObject *f, stack_frame_info_t **own)
{
    stand_in_word(f, slot_word(own, le_pyframe_code(f)));
}

/* f's locals now, from the compiled frame whose slot is 'slot', if it
   says (slot_locals): an unknown cell or free variable is left as it is */
static void
locals_from_slot(PyFrameObject *f, stack_frame_info_t **slot, int own)
{
    int i, n = n_locals_plus(le_pyframe_code(f));
    PyObject **values = PyMem_Malloc(n * sizeof(PyObject *) + 1);
    if (values == NULL)
        return;
    if (slot_locals(slot, le_pyframe_code(f), values, own)) {
        for (i = le_code_nlocals(le_pyframe_code(f)); i < n; i++)
            if (values[i] == NULL)
                values[i] = LE_LOCAL_UNKNOWN;
        psyco_frame_write_locals(f, 0, values, n);
        for (i = 0; i < n; i++)
            if (values[i] != LE_LOCAL_UNKNOWN)
                Py_XDECREF(values[i]);
    }
    PyMem_Free(values);
}

/* An emulated frame's locals are read from its compiled frame each time
   they are asked for (view_sync), not made with it: most emulated
   frames are walked over for their code and line. Its empty value stack
   holds LOCALS_NOTE and its tag meanwhile. Once the compiled frame no
   longer runs (its code and tag no longer in LeJIT's chain), the frame
   keeps what was read last. */
static char locals_note;
#define LOCALS_NOTE ((PyObject *)&locals_note)

/* one frame pvisitframes visits: a real frame of its own ('code' NULL), a
   compiled frame ('frame' NULL: its code, globals, tag and the place it is
   at), or the real frame whose code LeCode_Run runs (its code, globals,
   frame and place); all borrowed. A real frame's object is 'object', or
   NULL if none is made yet (visited_object makes it). */
typedef struct {
    PyObject *code, *globals;
    le_frame_t *frame;
    PyFrameObject *object;
    long tag;
    int lasti;
} visited_frame_t;

/* the visited real frame's object (borrowed), made if it has none, or
   NULL */
static PyFrameObject *
visited_object(const visited_frame_t *v)
{
    return v->object != NULL ? v->object : le_frame_object(v->frame);
}

static PyObject *
pvisitframes(PyObject *(*callback)(const visited_frame_t *, void *),
             void *data);

static PyObject *
visit_tag(const visited_frame_t *v, void *f)
{
    PyFrameObject *frame = (PyFrameObject *)f;
    if (v->frame == NULL && v->code == (PyObject *)le_pyframe_code(frame) &&
        v->tag == (long)le_pyframe_stack(frame)[1]) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    return NULL;
}

/* the slot that says where the locals of the frame of a walk's 'tag'
   (pvisitframes) are, and whether it is that of the frame it is inlined
   in (slot_locals) */
static stack_frame_info_t **
tag_locals_slot(long tag, int *own)
{
    stack_frame_info_t **slot = (stack_frame_info_t **)(tag + ((-tag) & 7));
    stack_frame_info_t **next;
    *own = 1;
    if ((stack_frame_info_t **)tag != slot)
        return slot;
    next = psyco_next_stack_frame(slot);
    *own = next == slot;
    return next;
}

static void
fill_noted_locals(PyFrameObject *f)
{
    PyObject *found;
    stack_frame_info_t **slot;
    long tag;
    int own;
    if (le_frame_is_executing(f) || !le_frame_stack_empty(f) ||
        le_pyframe_code(f)->co_stacksize < 2 ||
        le_pyframe_stack(f)[0] != LOCALS_NOTE)
        return;
    tag = (long)le_pyframe_stack(f)[1];
    found = pvisitframes(visit_tag, f);
    if (found == NULL) {
        le_pyframe_stack(f)[0] = le_pyframe_stack(f)[1] = NULL;
        return;
    }
    Py_DECREF(found);
    slot = tag_locals_slot(tag, &own);
    locals_from_slot(f, slot, own);
}

#if LE_HAS_INTERPRETER_FRAME
/* a frame object owning its frame, placed at its first traceable
   instruction, as PyFrame_New's */
static PyFrameObject *
frame_object_of_call(PyCodeObject *co, PyFunctionObject *func)
{
    PyFrameObject *f = PyObject_GC_NewVar(
        PyFrameObject, &PyFrame_Type, co->co_nlocalsplus + co->co_stacksize);
    if (f == NULL)
        return NULL;
    /* every field before its frame is NULL or 0, but f_trace_lines: the
       first, f_back, holds the size PyObject_GC_NewVar wrote, as the frame
       object's header has none */
    memset((char *)f + sizeof(PyObject), 0,
           offsetof(PyFrameObject, _f_frame_data) - sizeof(PyObject));
    f->f_trace_lines = 1;
    f->f_frame = (le_frame_t *)f->_f_frame_data;
    le_frame_init(f->f_frame, co, func, co->_co_firsttraceable,
                  FRAME_OWNED_BY_FRAME_OBJECT);
    PyObject_GC_Track(f);
    return f;
}
#endif

#if ALL_CHECKS
long le_stat_frame_objects = 0;
#endif

DEFINEFN PyFrameObject *
le_pyframe_new(PyThreadState *ts, PyCodeObject *co, PyObject *globals,
               PyFunctionObject *func)
{
#if ALL_CHECKS
    le_stat_frame_objects++;
#endif
#if LE_HAS_INTERPRETER_FRAME
    if (func != NULL)
        return frame_object_of_call(co, func);
#else
    (void)func;
#endif
    return le_pyframe_new_by_cpython(ts, co, globals);
}

PSY_INLINE PyFrameObject *
psyco_build_pyframe(PyObject *co, PyObject *globals, PyFunctionObject *func,
                    PyThreadState *tstate)
{
    le_frame_t *back;
    PyFrameObject *result;

    /* frame objects are not created in stack order
       with Psyco, so it's probably better not to
       create plain wrong chained lists */
    back = le_frame_current(tstate);
    le_frame_set_current(tstate, NULL);
    result = le_pyframe_new(tstate, (PyCodeObject *)co, globals, func);
    if (result == NULL)
        OUT_OF_MEMORY();
    le_frame_set_lasti(le_frame_of(result), -1);
    /* a frame reused from the code's free one may hold a note */
    if (((PyCodeObject *)co)->co_stacksize >= 1)
        le_pyframe_stack(result)[0] = NULL;
    le_frame_set_current(tstate, back);
    return result;
}

/* The frame object of a compiled callee's call, made when something first
   asks for it and kept until the call returns, so that it is one object
   for the call's life as CPython's is: f_trace, f_back, identity and what
   is stored in f_locals stay, and ceval goes on with it when tracing
   starts (psyco_resume_begin) or an exception passes (psyco_innermost_
   activation). The call's return address is replaced by
   psyco_return_barrier, which lets the frame go when it returns
   (psyco_activation_returned): nothing is stored or tested while no frame
   is asked for. A kept frame's locals and line are read from the call
   when asked for (view_sync, kept_lasti). Keyed by the callee's own
   slot, a stack address, unique among the calls running in every thread.
   An inlined frame has no call of its own whose return could be seen: its
   frame is kept until the code it is inlined in leaves it, which then
   lets it go (psyco_inline_frame_left); keyed by its parent's slot plus
   one. */
typedef struct {
    stack_frame_info_t **own;  /* the callee's slot; NULL: a free entry */
    stack_frame_info_t **slot; /* its caller's, holding the callee's record */
    code_t *ret;               /* the call's return address */
    PyFrameObject *frame;
    int resumed;   /* ceval runs it now */
    int exposed;   /* expose */
    int suspended; /* now a suspended coroutine's (psyco_resume_suspend) */
    /* the coroutine of a body started by its await, made when something
       could see it (psyco_coros_realise): the table's reference */
    PyObject *coro;
    /* the call runs a generator's body on its frame (on_gen_frame), whose
       frame object this is; what Python wrote in the locals the body
       holds is kept until the call ends, as a FrameRuntime's
       entry_locals are (psyco_frame_write_locals, psyco_gen_take_written) */
    int gen;
    PyObject **keep;
    int nkeep;
    int inlined; /* an inlined frame's */
} activation_t;

#define INLINE_KEY(own) ((stack_frame_info_t **)((char *)(own) + 1))

/* how many inlined frames' frames are kept: the code leaving an inlined
   frame that ran Python tests it */
DEFINEVAR int psyco_inline_frames_kept = 0;

static activation_t *acts = NULL;
static size_t acts_mask = 0, acts_count = 0;

/* in a kept frame's empty value stack, with its slot after it */
static char activation_note;
#define ACTIVATION_NOTE ((PyObject *)&activation_note)

static size_t
act_hash(stack_frame_info_t **own)
{
    return (size_t)((((unsigned long)own >> 3) * 0x9E3779B97F4A7C15UL) >> 32);
}

static activation_t *
act_find(stack_frame_info_t **own)
{
    size_t i;
    if (acts_count == 0)
        return NULL;
    for (i = act_hash(own) & acts_mask; acts[i].own != NULL;
         i = (i + 1) & acts_mask)
        if (acts[i].own == own)
            return &acts[i];
    return NULL;
}

static activation_t *
act_add(stack_frame_info_t **own)
{
    size_t i;
    if (2 * (acts_count + 1) > acts_mask + 1) {
        size_t n = acts_mask == 0 ? 16 : 2 * (acts_mask + 1), j;
        activation_t *old = acts, *t = PyMem_Calloc(n, sizeof(activation_t));
        if (t == NULL)
            OUT_OF_MEMORY();
        acts = t;
        j = acts_mask == 0 ? 0 : acts_mask + 1;
        acts_mask = n - 1;
        while (j--)
            if (old[j].own != NULL) {
                for (i = act_hash(old[j].own) & acts_mask; acts[i].own != NULL;
                     i = (i + 1) & acts_mask);
                acts[i] = old[j];
            }
        PyMem_Free(old);
    }
    for (i = act_hash(own) & acts_mask; acts[i].own != NULL;
         i = (i + 1) & acts_mask);
    acts_count++;
    acts[i].own = own;
    return &acts[i];
}

/* linear probing: later entries of the run move back into the hole */
static void
act_remove(activation_t *a)
{
    size_t i = a - acts, j = i, k;
    acts[i].own = NULL;
    acts_count--;
    for (;;) {
        j = (j + 1) & acts_mask;
        if (acts[j].own == NULL)
            return;
        k = act_hash(acts[j].own) & acts_mask;
        if (i <= j ? (k <= i || k > j) : (k <= i && k > j)) {
            acts[i] = acts[j];
            acts[j].own = NULL;
            i = j;
        }
    }
}

/* the kept frame of the compiled callee whose record is in 'slot', if
   any: a call record's first entry, for code 'co' */
static activation_t *
act_at(stack_frame_info_t **slot, PyCodeObject *co)
{
    stack_frame_info_t *rec = *slot;
    activation_t *a;
    if (acts_count == 0 || (((long)rec) & 1) != 0 || rec->nargs < 0 ||
        rec->co != co)
        return NULL;
    a = act_find(psyco_next_stack_frame(slot));
    return a != NULL && a->slot == slot && le_pyframe_code(a->frame) == co
               ? a
               : NULL;
}

static activation_t *
activation_of(PyFrameObject *f)
{
    size_t i;
    activation_t *a;
    if (acts_count == 0)
        return NULL;
    /* a generator's frame holds its own stack, and no note */
    if (le_pyframe_code(f)->co_stacksize >= 2 &&
        !le_frame_is_gen(le_frame_of(f))) {
        if (le_pyframe_stack(f)[0] != ACTIVATION_NOTE)
            return NULL;
        a = act_find((stack_frame_info_t **)le_pyframe_stack(f)[1]);
        return a != NULL && a->frame == f ? a : NULL;
    }
    for (i = 0; i <= acts_mask; i++)
        if (acts[i].own != NULL && acts[i].frame == f)
            return &acts[i];
    return NULL;
}

#if LE_RESUMES_GENERATORS_DIRECTLY
static activation_t *
gen_activation_find(PyFrameObject *f);

/* whether the generator's frame 'f' may be running its body as a direct
   call from compiled code (psyfunc.c:psyco_gen_step): running, but
   neither run from C (with a FrameRuntime) nor in the thread's chain,
   where ceval or LeGen_Run put the frames they run */
static bool
gen_runs_direct(le_frame_t *f)
{
    le_frame_t *t;
    if (!le_frame_is_gen(f) || !le_frame_executing(f) ||
        psyco_frame_start_get(f) != NULL)
        return false;
    for (t = le_frame_current(PyThreadState_GET()); t != NULL;
         t = le_frame_back(t))
        if (t == f)
            return false;
    return true;
}
#endif

/* a kept frame the compiled code still runs, not ceval */
static activation_t *
live_activation(PyFrameObject *f)
{
    activation_t *a = activation_of(f);
#if LE_RESUMES_GENERATORS_DIRECTLY
    if (a == NULL && gen_runs_direct(le_frame_of(f)))
        a = gen_activation_find(f);
#endif
    return a != NULL && !a->resumed ? a : NULL;
}

/* where the call is now, as pvisitframes finds it: the record of the
   call it is in, or its note; -1 if neither says */
static int
kept_lasti(activation_t *a)
{
    long v = (long)*(a->inlined ? a->slot : a->own);
    if (a->inlined) {
        /* in the parent's slot: the inlined frame's own record, or that of
           the compiled call it makes, after the callee's entry */
        stack_frame_info_t *rec = (stack_frame_info_t *)v;
        if ((v & 1) != 0)
            return -1;
        if (rec->link_stack_depth == 0)
            return rec->own_lasti;
        return finfo_last(rec) == rec + 1 ? rec->back_lasti : -1;
    }
    if (v == -1 || v == FINFO_RETURNING)
        return -1;
    if ((v & 1) != 0)
        return psyco_note_map(v)->lasti;
    return finfo_last((stack_frame_info_t *)v)->back_lasti;
}

/* innermost_slot's last walk, kept for the next: a run's compiled calls
   from its root slot in, each by its own slot. A call whose return comes
   to psyco_activation_returned (through a barrier put in its return
   address, or its kept frame's) is running until then, and so are the
   calls out of it, whose slots keep their records: the next walk goes on
   from the innermost such call. Each walk watches the call out of the
   innermost one, so a walk from a call that frame returns to, or from
   one it calls, is short, and unwinding a deep stack walks each call
   once. */
typedef struct {
    stack_frame_info_t **own;
    code_t *ret; /* what it returns to, if the barrier is one put here */
    bool seen;   /* its return comes to psyco_activation_returned */
} walked_call_t;

struct walked_calls_s {
    size_t n, size; /* the root's first, the innermost's last */
    walked_call_t calls[1];
};

/* the seen calls but the roots in this thread's walks */
static _Thread_local long walked_seen LE_FAST_TLS;

/* a run's walk after its first, which is not kept: most runs that walk at
   all walk once */
static struct walked_calls_s walked_once;

static void
walked_forget(struct walked_calls_s *w)
{
    size_t i;
    for (i = 1; i < w->n; i++)
        if (w->calls[i].seen)
            walked_seen--;
    w->n = 0;
}

static __attribute__((noinline)) void
walked_end(struct walked_calls_s *w)
{
    if (w == &walked_once)
        return;
    walked_forget(w);
    PyMem_Free(w);
}

/* the seen call whose own slot is 'own', in any of this thread's runs, or
   NULL; 'in' is set to its run. The slots go down the stack. */
static walked_call_t *
walked_find(stack_frame_info_t **own, FrameRuntime **in)
{
    FrameRuntime *fr;
    if (walked_seen == 0)
        return NULL;
    for (fr = frame_runs; fr != NULL; fr = fr->next) {
        struct walked_calls_s *w = fr->walked;
        size_t lo = 1, hi;
        if (w == NULL || w->n < 2 || own > w->calls[1].own ||
            own < w->calls[w->n - 1].own)
            continue;
        hi = w->n - 1;
        while (lo <= hi) {
            size_t mid = (lo + hi) / 2;
            if (w->calls[mid].own == own) {
                if (!w->calls[mid].seen)
                    break;
                if (in != NULL)
                    *in = fr;
                return &w->calls[mid];
            }
            if (w->calls[mid].own > own)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    return NULL;
}

static walked_call_t *
walked_call(stack_frame_info_t **own)
{
    return walked_find(own, NULL);
}

/* Where the return address of the compiled callee whose slot is 'own' may
   be: between the slot and the callee's own callee, or the next run of
   compiled code nearer here (or here), down from the slot. */
#define RET_SEARCH_MAX 8192
static code_t **
return_search_end(stack_frame_info_t **own)
{
    code_t **lo;
    le_frame_t *t;
    if ((((long)*own) & 1) == 0)
        return (code_t **)psyco_next_stack_frame(own) + 1;
    lo = (code_t **)&lo;
    for (t = le_frame_current(PyThreadState_GET()); t != NULL;
         t = le_frame_back(t)) {
        FrameRuntime *fr = psyco_frame_start_get(t);
        code_t **root;
        if (fr == NULL || fr->resumed || !le_frame_executing(t))
            continue;
        root = (code_t **)*fr->psy_frames_start;
        if (root < (code_t **)own && root + 1 > lo)
            lo = root + 1;
    }
    return lo;
}

/* Where the return address of the compiled callee whose slot is 'own' is:
   where the record or note in that slot says the callee keeps it, or after
   the arguments the call pushed if the slot says nothing. A unification
   may have moved it, and the word it left still holds it. A note may be
   left from an earlier call: if the word doesn't hold the address, it is
   looked for as return_search_end says: NULL if it isn't there once, so
   that nothing but it is replaced. Where innermost_slot put the barrier,
   any word holding it. */
static code_t **
return_address(stack_frame_info_t **own, stack_frame_info_t *rec)
{
    long v = (long)*own, below = (rec->nargs + 1) * (long)sizeof(long);
    code_t **p, **lo, **q, **found = NULL;
    walked_call_t *w = walked_call(own);
    bool barrier = w != NULL && w->ret != NULL;
    code_t *want = barrier ? psyco_return_barrier : rec->ret_addr;
    if ((v & 1) == 0)
        below = ((stack_frame_info_t *)v)->ret_below;
    else if (v != -1 && v != FINFO_RETURNING)
        below = psyco_note_map(v)->ret_below;
    p = (code_t **)((char *)own - below);
    if (*p == want)
        return p;
    lo = return_search_end(own);
    if ((code_t **)own - lo > RET_SEARCH_MAX)
        lo = (code_t **)own - RET_SEARCH_MAX;
    for (q = (code_t **)own - 1; q >= lo; q--)
        if (*q == want) {
            if (found != NULL)
                return NULL;
            if (barrier)
                return q;
            found = q;
        }
    return found;
}

/* A kept frame's barrier goes in the call's return address at 'ret': what
   the call returns to, which a barrier innermost_slot put there keeps */
static code_t *
barrier_taken(stack_frame_info_t **own, code_t **ret)
{
    code_t *to = *ret;
    walked_call_t *w;
    if (to == psyco_return_barrier) {
        w = walked_call(own);
        if (w == NULL || w->ret == NULL)
            psyco_fatal_msg("a kept frame's call has an unknown barrier");
        to = w->ret;
        w->ret = NULL; /* the kept frame's barrier sees the return */
    }
    *ret = psyco_return_barrier;
    return to;
}

/* call 'k' of a walk, not the root, is watched: its return comes to
   psyco_activation_returned, or it is left unseen */
static void
walked_watch(struct walked_calls_s *w, size_t k)
{
    walked_call_t *c = &w->calls[k];
    stack_frame_info_t **slot, *rec;
    activation_t *a;
    code_t **lo, **q;
    if (c->seen)
        return;
    slot = c[-1].own;
    rec = *slot;
    if ((((long)rec) & 1) != 0 || rec->nargs < 0)
        return;
    a = act_find(c->own);
    if (a != NULL && a->slot != slot)
        return;
    if ((((long)*c->own) & 1) != 0)
        return;
    /* every copy a move left in its frame, above its callee's slot, beside
       a kept frame's barrier: the call returns from one of them */
    lo = (code_t **)psyco_next_stack_frame(c->own) + 1;
    if ((code_t **)c->own - lo > RET_SEARCH_MAX)
        return;
    if (a != NULL) {
        /* a kept frame whose call returned past its barrier is left over
           from an earlier call */
        for (q = (code_t **)c->own - 1; q >= lo; q--)
            if (*q == psyco_return_barrier)
                break;
        if (q < lo)
            return;
    }
    for (q = (code_t **)c->own - 1; q >= lo; q--)
        if (*q == rec->ret_addr) {
            *q = psyco_return_barrier;
            c->ret = rec->ret_addr;
        }
    c->seen = a != NULL || c->ret != NULL;
    if (c->seen)
        walked_seen++;
}

/* the call whose own slot is 'own' has returned: 'run' is set to its run
   if it was watched, whose walk then ends with its caller's calls, all
   still running. What it returns to if it came through a barrier put
   here, else NULL. */
static code_t *
walked_returned(stack_frame_info_t **own, FrameRuntime **run)
{
    walked_call_t *c = walked_find(own, run);
    struct walked_calls_s *w;
    code_t *ret;
    if (c == NULL) {
        *run = NULL;
        return NULL;
    }
    w = (*run)->walked;
    ret = c->ret;
    w->n = c - w->calls;
    walked_seen--;
    return ret;
}

#if LE_RESUMES_GENERATORS_DIRECTLY
/* the generator's frame the call whose record is in 'slot' runs on, if
   the record says it runs on one (on_gen_frame): the word above the
   callee's own slot, which its caller pushed (psyco_call_psyco_gen) */
static le_frame_t *
gen_frame_at(stack_frame_info_t **slot)
{
    stack_frame_info_t *rec = *slot;
    le_frame_t *f;
    if ((((long)rec) & 1) != 0 || !rec->on_gen_frame)
        return NULL;
    f = *(le_frame_t **)(psyco_next_stack_frame(slot) + 1);
    return f;
}

/* The frame object of that call: the generator's own, kept as a kept
   frame is until the call returns, so that the same activation answers
   for it; its place is set from the call's if 'lasti' gives it. A new
   reference, or NULL. */
static PyFrameObject *
gen_kept_frame(stack_frame_info_t **slot, int lasti)
{
    stack_frame_info_t **own = psyco_next_stack_frame(slot);
    le_frame_t *gf = gen_frame_at(slot);
    activation_t *a = act_find(own);
    code_t **ret;
    PyFrameObject *f;
    if (a != NULL) {
        if (a->slot != slot || !a->gen)
            return NULL;
        if (!a->resumed && lasti >= 0)
            le_frame_set_lasti(gf, le_lasti_of(lasti));
        Py_INCREF(a->frame);
        return a->frame;
    }
    ret = return_address(own, *slot);
    if (ret == NULL)
        return NULL;
    /* complete, or the frame object made would be its caller's */
    if (lasti >= 0 || !le_frame_complete(gf))
        le_frame_set_lasti(gf, le_lasti_of(lasti));
    f = le_frame_object(gf);
    if (f == NULL)
        return NULL;
    a = act_add(own);
    a->slot = slot;
    a->ret = barrier_taken(own, ret);
    a->frame = f; /* the table's reference */
    a->resumed = 0;
    a->exposed = 0;
    a->suspended = 0;
    a->coro = NULL;
    a->gen = 1;
    a->keep = NULL;
    a->nkeep = 0;
    a->inlined = 0;
    Py_INCREF(f);
    Py_INCREF(f);
    return f;
}

/* the generator's frame a frame walk's item for a compiled frame stands
   for (pvisitframes), or NULL */
static le_frame_t *
visited_gen_frame(const visited_frame_t *v)
{
    if (v->frame != NULL || (v->tag & (sizeof(long) - 1)) != 0)
        return NULL;
    return gen_frame_at((stack_frame_info_t **)v->tag);
}

/* where Python's writes into the locals a generator's body holds are kept
   while the body runs on its frame 'f' as a direct call, or NULL */
static PyObject ***
gen_keep_of(le_frame_t *f, int **n)
{
    size_t i;
    if (acts_count == 0 || f->frame_obj == NULL)
        return NULL;
    for (i = 0; i <= acts_mask; i++)
        if (acts[i].own != NULL && acts[i].gen &&
            acts[i].frame == f->frame_obj) {
            *n = &acts[i].nkeep;
            return &acts[i].keep;
        }
    return NULL;
}
static PyFrameObject *
emulate_frame(const visited_frame_t *v);

static PyObject *
visit_gen_frame(const visited_frame_t *v, void *f)
{
    if (visited_gen_frame(v) == le_frame_of((PyFrameObject *)f))
        return (PyObject *)emulate_frame(v);
    return NULL;
}

/* the activation of a generator's frame 'f' that runs its body as a
   direct call, made now if the call is found, or NULL */
static activation_t *
gen_activation_find(PyFrameObject *f)
{
    PyObject *k = pvisitframes(visit_gen_frame, f);
    if (k == NULL) {
        PyErr_Clear();
        return NULL;
    }
    Py_DECREF(k);
    return activation_of(f);
}
#else
#define gen_frame_at(slot) ((le_frame_t *)NULL)
#define gen_kept_frame(slot, lasti) ((PyFrameObject *)NULL)
#define visited_gen_frame(v) ((le_frame_t *)NULL)
#endif

/* a kept frame no call can see any more goes */
static void
drop_activation(activation_t *a)
{
    PyFrameObject *f = a->frame;
    PyObject *t, *v, *tb;
#ifdef PyFrameLocalsProxy_Check
    if (a->exposed)
        psyco_frames_exposed--;
#endif
    if (a->inlined)
        psyco_inline_frames_kept--;
    le_pyframe_set_executing(f, 0);
    act_remove(a);
    if (le_pyframe_code(f)->co_stacksize >= 2 &&
        le_pyframe_stack(f)[0] == ACTIVATION_NOTE)
        le_pyframe_stack(f)[0] = le_pyframe_stack(f)[1] = NULL;
    PyErr_Fetch(&t, &v, &tb);
    Py_DECREF(f);
    PyErr_Restore(t, v, tb);
}

static stack_frame_info_t **
innermost_slot(le_frame_t **real);

/* the slot of the innermost compiled frame, as innermost_slot gives it */
static stack_frame_info_t **
innermost_own(stack_frame_info_t **slot, le_frame_t *real)
{
    FrameRuntime *fr;
    if (slot != NULL)
        return (*slot)->link_stack_depth == 0 ? slot
                                              : psyco_next_stack_frame(slot);
    fr = real == NULL ? NULL : psyco_frame_start_get(real);
    return fr == NULL ? NULL : *fr->psy_frames_start;
}

/* the kept frame of the frame of code 'co' inlined in the innermost
   compiled frame, if any and ceval doesn't run it */
static activation_t *
inline_kept_at(stack_frame_info_t **slot, le_frame_t *real, PyCodeObject *co)
{
    stack_frame_info_t **own = innermost_own(slot, real);
    activation_t *a;
    if (psyco_inline_frames_kept == 0 || own == NULL ||
        (a = act_find(INLINE_KEY(own))) == NULL || a->resumed ||
        le_pyframe_code(a->frame) != co)
        return NULL;
    return a;
}

DEFINEFN int
psyco_inline_frame_left(void)
{
    le_frame_t *real;
    stack_frame_info_t **slot = innermost_slot(&real), **own;
    activation_t *a;
    own = innermost_own(slot, real);
    if (own != NULL && (a = act_find(INLINE_KEY(own))) != NULL)
        drop_activation(a);
    return 0;
}

/* The traceback entries' frames catching callees hold on this thread
   (psyco_frame_catch), by the callee's own slot, so that a walk keeps
   one as its call's frame (keep_frame). An entry is live while its word
   holds its frame; past CATCHERS_MAX, a frame is left out and a walk
   makes another. */
#define CATCHERS_MAX 32
typedef struct {
    stack_frame_info_t **own;
    PyObject **word;
    PyFrameObject *frame;
} catcher_t;
static _Thread_local catcher_t catchers[CATCHERS_MAX] LE_FAST_TLS;
static _Thread_local int ncatchers LE_FAST_TLS;

static PyFrameObject *
catcher_take(stack_frame_info_t **own, PyCodeObject *co);

/* the activation with key 'own' for the frame of code 'co' whose record
   is in 'slot', made if need be. A compiled callee's call's return address
   is at 'ret' (NULL: not found, and none is made); an inlined frame's has
   none. The frame, a new reference, or NULL. */
static PyFrameObject *
keep_frame(stack_frame_info_t **own, stack_frame_info_t **slot, int inlined,
           code_t **ret, PyObject *co, PyObject *globals,
           PyFunctionObject *func, int lasti)
{
    activation_t *a = act_find(own);
    PyFrameObject *f;
    if (a != NULL) {
        if (a->slot != slot || le_pyframe_code(a->frame) != (PyCodeObject *)co)
            return NULL;
        f = a->frame;
        Py_INCREF(f);
        if (!a->resumed) {
            le_frame_set_lasti(le_frame_of(f), le_lasti_of(lasti));
            if (!inlined)
                stand_for_call(f, own);
        }
        return f;
    }
    if (!inlined && ret == NULL)
        return NULL;
    /* a traceback's frame stands for the call already: it is the one */
    f = inlined || ncatchers == 0 ? NULL
                                  : catcher_take(own, (PyCodeObject *)co);
    if (f != NULL)
        Py_INCREF(f);
    else
        f = psyco_build_pyframe(co, globals, func, PyThreadState_GET());
    le_frame_set_lasti(le_frame_of(f), le_lasti_of(lasti));
    /* running, as CPython's: frame.clear() refuses it */
    le_pyframe_set_executing(f, 1);
    if (((PyCodeObject *)co)->co_stacksize >= 2) {
        le_pyframe_stack(f)[0] = ACTIVATION_NOTE;
        le_pyframe_stack(f)[1] = (PyObject *)own;
    }
    a = act_add(own);
    a->slot = slot;
    a->inlined = inlined;
    a->frame = f; /* the table's reference */
    a->resumed = 0;
    a->exposed = 0;
    a->suspended = 0;
    a->coro = NULL;
    a->gen = 0;
    a->keep = NULL;
    a->nkeep = 0;
    if (inlined)
        psyco_inline_frames_kept++;
    else {
        a->ret = barrier_taken(own, ret);
        stand_for_call(f, own);
    }
    Py_INCREF(f);
    return f;
}

/* the function of the compiled frame of 'tag' (kept_frame) where its
   record knows it, else NULL */
static PyFunctionObject *
tag_function(long tag)
{
    long entry = (-tag) & (sizeof(long) - 1);
    return (*(stack_frame_info_t **)(tag + entry))[entry].func;
}

/* 'tag' is the slot holding the frame's record, less one per inlined
   frame before it (pvisitframes) */
static PyFrameObject *
kept_frame(long tag, PyObject *co, PyObject *globals, int lasti)
{
    long entry = (-tag) & (sizeof(long) - 1);
    stack_frame_info_t **slot = (stack_frame_info_t **)(tag + entry), **own;
    stack_frame_info_t *rec = *slot;
    if (entry == 0 && rec->nargs >= 0 && rec->co == (PyCodeObject *)co) {
        /* a compiled callee */
        if (gen_frame_at(slot) != NULL)
            return gen_kept_frame(slot, lasti);
        /* a frame already kept has its barrier in the return address */
        own = psyco_next_stack_frame(slot);
        return keep_frame(
            own, slot, 0,
            act_find(own) != NULL ? NULL : return_address(own, rec), co,
            globals, rec->func, lasti);
    }
    if ((entry == 1 && rec->nargs >= 0 && finfo_last(rec) == rec + 1 &&
         (rec + 1)->co == (PyCodeObject *)co) ||
        (entry == 0 && rec->link_stack_depth == 0 &&
         rec->co == (PyCodeObject *)co))
        /* an inlined frame, in the compiled call it makes or in its own
           record: its parent's slot is the one holding them */
        return keep_frame(INLINE_KEY(slot), slot, 1, NULL, co, globals,
                          rec[entry].func, lasti);
    return NULL;
}

/* CPython's public constructors make the generator around a frame object,
   copying the frame object's own frame into the generator where the
   generator holds its frame: the frame object is then its frame object,
   which holds the reference given. The origin a coroutine records now is
   dropped: it stands for one made at a call where the origin depth was 0
   (psyfunc.c:psyco_coro_start). */
DEFINEFN PyObject *
le_gen_of_frame(PyFrameObject *f, PyFunctionObject *func)
{
#if LE_HAS_GEN_NEW_AROUND_FRAME
    int flags = ((PyCodeObject *)func->func_code)->co_flags;
    PyObject *gen;
#if LE_HAS_INTERPRETER_FRAME
    /* what the constructor checks: f->f_frame owned by f and not yet
       linked to it, and the frame it copies (f's own) linked to f; it reads
       f->f_frame only for the code, before pointing it at the copy. A
       frame compiled code runs is marked as the thread's
       (le_pyframe_set_executing): the coroutine's frame is marked so
       instead (coro_set_running). */
    le_frame_t *own = le_frame_of(f), unlinked;
    PyFrameObject *own_obj = own->frame_obj;
    char own_owner = own->owner;
    extra_assert(le_pyframe_owns(f));
    own->owner = FRAME_OWNED_BY_FRAME_OBJECT;
    memcpy(&unlinked, own, offsetof(le_frame_t, localsplus));
    unlinked.frame_obj = NULL;
    /* a collection as the constructor allocates visits f's words through
       f->f_frame: the copy has none */
#if LE_HAS_FRAME_STACKREFS
    unlinked.stackpointer = unlinked.localsplus;
#else
    unlinked.stacktop = 0;
#endif
    f->f_frame = &unlinked;
    own->frame_obj = f;
    Py_INCREF(f);
#endif
    if (flags & CO_COROUTINE)
        gen = PyCoro_New(f, func->func_name, func->func_qualname);
    else if (flags & CO_ASYNC_GENERATOR)
        gen = PyAsyncGen_New(f, func->func_name, func->func_qualname);
    else
        gen = PyGen_NewWithQualName(f, func->func_name, func->func_qualname);
#if LE_HAS_INTERPRETER_FRAME
    if (gen == NULL) {
        f->f_frame = own;
        own->frame_obj = own_obj;
        own->owner = own_owner;
        Py_DECREF(f);
    }
    else if (flags & CO_COROUTINE)
        Py_CLEAR(((PyCoroObject *)gen)->cr_origin_or_finalizer);
#else
    if (gen != NULL && (flags & CO_COROUTINE))
        Py_CLEAR(((PyCoroObject *)gen)->cr_origin);
#endif
    return gen;
#else
    /* not called: no body starts without it
       (mergepoints.c:psyco_is_startable_coroutine) */
    (void)func;
    Py_DECREF(f);
    PyErr_SetString(PyExc_SystemError,
                    "no generator can be made around a frame object");
    return NULL;
#endif
}

#if LE_HAS_INTERPRETER_FRAME
/* The generator, coroutine or async generator RETURN_GENERATOR would have
   made at the call of 'func', written as CPython's make_gen and
   _Py_MakeCoro write it. A coroutine's origin is none, as the call's
   origin tracking depth was 0 (psyco_coro_start). Its frame, left cleared
   (FRAME_CLEARED), is for the caller to fill; the caller tracks it then. A
   new reference, or NULL. */
DEFINEFN PyTypeObject *
le_gen_type(PyCodeObject *co)
{
    return (co->co_flags & CO_COROUTINE)          ? &PyCoro_Type
           : (co->co_flags & CO_ASYNC_GENERATOR) ? &PyAsyncGen_Type
                                                 : &PyGen_Type;
}

DEFINEFN void
le_gen_init(PyGenObject *gen, PyFunctionObject *func)
{
#if LE_HAS_GEN_CODE_FIELD
    gen->gi_code = (PyCodeObject *)Py_NewRef(func->func_code);
#endif
    gen->gi_weakreflist = NULL;
    gen->gi_name = Py_NewRef(func->func_name);
    gen->gi_qualname = Py_NewRef(func->func_qualname);
    gen->gi_exc_state.exc_value = NULL;
    gen->gi_exc_state.previous_item = NULL;
    gen->gi_origin_or_finalizer = NULL;
    gen->gi_hooks_inited = 0;
    gen->gi_closed = 0;
    gen->gi_running_async = 0;
    gen->gi_frame_state = FRAME_CLEARED;
}

DEFINEFN PyObject *
le_gen_make(PyFunctionObject *func)
{
    PyCodeObject *co = (PyCodeObject *)func->func_code;
    PyGenObject *gen = PyObject_GC_NewVar(
        PyGenObject, le_gen_type(co), co->co_nlocalsplus + co->co_stacksize);
    if (gen == NULL)
        return NULL;
    le_gen_init(gen, func);
    return (PyObject *)gen;
}
#endif

/* a realised coroutine runs (psyco_coros_realise) or not, as gen_send_ex
   marks it: on a frame with no state its own flag says so, and its frame
   has no stack top while it runs */
static void
coro_set_running(PyObject *coro, int on)
{
#if LE_HAS_INTERPRETER_FRAME
    /* off, it has paused or ended, which set its state */
    if (on)
        ((PyGenObject *)coro)->gi_frame_state = FRAME_EXECUTING;
#elif LE_HAS_FRAME_STATE
    (void)coro;
    (void)on;
#else
    ((PyGenObject *)coro)->gi_running = (char)on;
    if (on)
        ((PyGenObject *)coro)->gi_frame->f_stacktop = NULL;
#endif
}

/* the body of a realised coroutine has returned or raised: as gen_send_ex
   leaves it, closed, with no frame; the table's reference goes */
static void
coro_finish(PyObject *coro)
{
    PyGenObject *gen = (PyGenObject *)coro;
#if LE_HAS_INTERPRETER_FRAME
    le_gen_frame_give_back(gen);
#else
    PyFrameObject *f = gen->gi_frame;
    coro_set_running(coro, 0);
    if (f != NULL) {
        f->f_gen = NULL;
        gen->gi_frame = NULL;
        Py_DECREF(f);
    }
#endif
    Py_DECREF(coro);
}

/* the bodies ceval goes on with as their coroutines, while it runs them
   (psyco_resume_run_coro), by the slot of their compiled call: the call's
   record stays on the stack, and its coroutine is that one */
struct coro_run_s {
    stack_frame_info_t **slot;
    PyObject *coro;
    struct coro_run_s *next;
};
static struct coro_run_s *coro_runs = NULL;

/* The coroutine of the body whose call's record is in 'slot' (its
   record's coro_func set), made as CPython made it at the call, around the
   call's kept frame, and running: the same object each time. Borrowed;
   NULL if it can't be made. */
static PyObject *
coro_realise(stack_frame_info_t **slot)
{
    stack_frame_info_t *rec = *slot;
    PyFunctionObject *func = rec->coro_func;
    activation_t *a = act_find(psyco_next_stack_frame(slot));
    PyFrameObject *f;
    PyObject *coro;
    struct coro_run_s *r;
    for (r = coro_runs; r != NULL; r = r->next)
        if (r->slot == slot)
            return r->coro;
    if (a != NULL && a->coro != NULL)
        return a->coro;
    f = kept_frame((long)slot, (PyObject *)rec->co, func->func_globals, -1);
    if (f == NULL)
        return NULL;
    a = act_find(psyco_next_stack_frame(slot));
    if (a == NULL || a->resumed) {
        Py_DECREF(f);
        return NULL;
    }
    Py_CLEAR(
        f->f_back); /* a running frame's caller is found by f_back's getter */
    coro = le_gen_of_frame(f, func);
    if (coro == NULL) {
        PyErr_Clear();
        return NULL;
    }
    coro_set_running(coro, 1);
    a->coro = coro;
    return coro;
}

/* Realises the coroutine of every body started by its await that is
   running now, in every thread (psyco_frames_running), before something
   that could find it looks */
DEFINEFN void
psyco_coros_realise(void)
{
    /* making one can collect, and a collector's callback can look again
       (codemanager.c:owners_scan): that look finds none being made */
    static int realising = 0;
    PyThreadState *ts;
    PyObject *t, *v, *tb;
    if (runs_key == NULL || realising)
        return;
    realising = 1;
    PyErr_Fetch(&t, &v, &tb);
    for (ts = PyInterpreterState_ThreadHead(PyInterpreterState_Main());
         ts != NULL; ts = PyThreadState_Next(ts)) {
        PyObject *c =
            ts->dict == NULL ? NULL : PyDict_GetItem(ts->dict, runs_key);
        FrameRuntime *fr;
        if (c == NULL)
            continue;
        for (fr = *(FrameRuntime **)PyCapsule_GetPointer(c, NULL); fr != NULL;
             fr = fr->next) {
            stack_frame_info_t **f1;
            if (fr->resumed)
                continue;
            for (f1 = *fr->psy_frames_start; (((long)*f1) & 1) == 0;
                 f1 = psyco_next_stack_frame(f1)) {
                if ((*f1)->coro_func != NULL && (*f1)->nargs >= 0)
                    coro_realise(f1);
                if ((*f1)->link_stack_depth == 0)
                    break; /* stack top is an inline frame */
            }
        }
    }
    PyErr_Restore(t, v, tb);
    realising = 0;
}

static PyFrameObject *
activation_caller(activation_t *a);

static PyFrameObject *
walked_caller(FrameRuntime *fr, stack_frame_info_t **own, bool *found);

DEFINEFN
code_t *
psyco_activation_returned(long *sp)
{
    FrameRuntime *walked;
    code_t *ret = walked_returned((stack_frame_info_t **)(sp + 1), &walked);
    activation_t *a = act_find((stack_frame_info_t **)(sp + 1));
    PyFrameObject *f;
    PyObject *t, *v, *tb, **keep;
    int gen, nkeep;
    PyObject *coro;
    if (a == NULL) {
        if (walked != NULL && ret != NULL)
            return ret;
        psyco_fatal_msg("a kept frame's call returned unknown");
    }
    f = a->frame;
    ret = a->ret;
    gen = a->gen;
    keep = a->keep;
    nkeep = a->nkeep;
    if (f->f_back == NULL && Py_REFCNT(f) > 1 && !gen && !a->suspended &&
        !(LE_HAS_FINISHED_GENERATOR_UNLINKED &&
          (le_pyframe_code(f)->co_flags &
           (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)))) {
        /* it outlives its call: linked to its caller, as CPython's is,
           while the caller's record still says where the call was; where
           CPython leaves an ended generator's without one, so is this */
        bool found = false;
        PyErr_Fetch(&t, &v, &tb);
        if (walked != NULL)
            f->f_back = walked_caller(walked, a->own, &found);
        if (!found)
            f->f_back = activation_caller(a);
        PyErr_Clear();
        PyErr_Restore(t, v, tb);
        a = act_find((stack_frame_info_t **)(sp + 1));
    }
    /* the call is over, as its caller is about to say: the frame's
       finaliser may walk the frames */
    *a->slot = (stack_frame_info_t *)-1;
#ifdef PyFrameLocalsProxy_Check
    if (a->exposed)
        psyco_frames_exposed--;
#endif
    if (!a->suspended && !gen)
        le_pyframe_set_executing(f, 0);
    coro = a->coro;
    act_remove(a);
    if (!gen && le_pyframe_code(f)->co_stacksize >= 2 &&
        le_pyframe_stack(f)[0] == ACTIVATION_NOTE)
        le_pyframe_stack(f)[0] = le_pyframe_stack(f)[1] = NULL;
    PyErr_Fetch(&t, &v, &tb);
    if (keep != NULL) {
        int i;
        for (i = 0; i < nkeep; i++) Py_XDECREF(keep[i]);
        PyMem_Free(keep);
    }
    if (coro != NULL)
        coro_finish(coro);
    Py_DECREF(f);
    PyErr_Restore(t, v, tb);
    return ret;
}

static stack_frame_info_t **
innermost_slot(le_frame_t **real);

DEFINEFN
PyFrameObject *
psyco_innermost_activation(PyCodeObject *co)
{
    le_frame_t *real;
    PyFrameObject *g;
    stack_frame_info_t **slot;
    activation_t *a;
    slot = innermost_slot(&real);
    /* the frame inlined in it, until its code leaves it */
    if ((a = inline_kept_at(slot, real, co)) != NULL)
        return a->frame;
    if (slot == NULL)
        return NULL;
    /* a generator's frame stands for its body's call from the start */
    if (gen_frame_at(slot) != NULL && (*slot)->co == co &&
        (g = gen_kept_frame(slot, -1)) != NULL)
        Py_DECREF(g); /* the table holds it */
    if (acts_count == 0 || (a = act_at(slot, co)) == NULL || a->resumed)
        return NULL;
    return a->frame;
}

DEFINEFN
PyFunctionObject *
psyco_innermost_function(PyCodeObject *co)
{
    le_frame_t *real;
    stack_frame_info_t **slot = innermost_slot(&real);
    if (slot != NULL)
        return (*slot)->co == co ? (*slot)->func : NULL;
    return real != NULL && le_frame_code(real) == co ? le_frame_function(real)
                                                     : NULL;
}

/* A frame object for a frame pvisitframes visits: a real frame, or a new
   emulated frame for a compiled one. New reference. */
static PyFrameObject *
emulate_frame(const visited_frame_t *v)
{
    if (v->frame != NULL) {
        /* a real Python frame, or the one whose code LeCode_Run runs */
        PyFrameObject *o = visited_object(v);
        if (o == NULL)
            return NULL;
        psyco_frame_stands(o);
        Py_INCREF(o);
        return o;
    }
    else {
        /* a Psyco frame: emulate it */
        PyObject *co = v->code;
        PyObject *globals = v->globals;
        PyFrameObject *result;
        long tag = v->tag;
        int lasti = v->lasti;
        extra_assert(co != NULL);
        extra_assert(globals != NULL);
        if ((result = kept_frame(tag, co, globals, lasti)) != NULL)
            return result;
        result = psyco_build_pyframe(co, globals, tag_function(tag),
                                     PyThreadState_GET());
        le_frame_set_lasti(le_frame_of(result), le_lasti_of(lasti));
        if (((PyCodeObject *)co)->co_stacksize >= 2) {
            le_pyframe_stack(result)[0] = LOCALS_NOTE;
            le_pyframe_stack(result)[1] = (PyObject *)tag;
        }
        else {
            int own;
            stack_frame_info_t **slot = tag_locals_slot(tag, &own);
            if (slot_locals(slot, (PyCodeObject *)co,
                            le_frame_args(le_frame_of(result)), own))
                le_frame_words_own(le_frame_of(result),
                                   n_locals_plus((PyCodeObject *)co));
        }
        return result;
    }
}

struct sfitmp_s {
    stack_frame_info_t **fi;
    struct sfitmp_s *next;
};

/* The compiled frames now running interpreted (psyco_resume_run): each is
   the first entry of the record in a stack slot, which pvisitframes skips,
   since the real frame stands for it. Slots are stack addresses, so the
   list can mix threads. */
struct hidden_s {
    stack_frame_info_t **slot;
    struct hidden_s *next;
};
static struct hidden_s *hidden_frames = NULL;

static bool
is_hidden(stack_frame_info_t **slot)
{
    struct hidden_s *h;
    for (h = hidden_frames; h != NULL; h = h->next)
        if (h->slot == slot)
            return true;
    return false;
}

/* the next frame out of 'f', whose frame object is '*fo' or NULL if it
   has none, as le_frame_object_back gives it, without making a frame
   object: the one its object was linked to, else its caller Python sees */
static void
walk_back(le_frame_t **f, PyFrameObject **fo)
{
    PyFrameObject *o = *fo != NULL ? *fo : le_frame_object_made(*f);
    le_frame_t *p;
    if (o != NULL && o->f_back != NULL) {
        *fo = o->f_back;
        *f = le_frame_of(*fo);
        return;
    }
    p = le_frame_back(o != NULL ? le_frame_of(o) : *f);
    while (p != NULL && !le_frame_complete(p)) p = le_frame_back(p);
    *f = p;
    *fo = p != NULL ? le_frame_object_made(p) : NULL;
}

static PyObject *
pvisitframes(PyObject *(*callback)(const visited_frame_t *, void *),
             void *data)
{
    /* Whenever we run Psyco-produced machine code, we mark the current
       Python frame as the starting point of a chained list of Psyco
       frames. The machine code will update this chained list so that
       psyco_next_stack_frame() can be used to visit the list from
       the outermost to the innermost frames. Note that the list does
       not contain the first Psyco frame, the one directly run by a
       call to psyco_processor_run(). This still gives the expected
       result, because psyco_processor_run() is only called by
       eval_hook.c:LeCode_Run() on a real Python frame, which replaces
       the missing Psyco frame.
       XXX this would no longer work if we filled the emulated frames
           with more information, like local variables */

    PyObject *result = NULL;
    FrameRuntime *fstart;
    /* the next run out, as the runs nest */
    FrameRuntime *run = frame_runs;
    le_frame_t *f = le_frame_current(PyThreadState_Get());
    /* f's frame object, or NULL if it has none: none is made for a frame
       the walk only passes */
    PyFrameObject *fo = NULL;
    bool linked = false; /* the previous frame was emulated, so the compiled
                            frames up to this one are already in the chain
                            (linked_frame) */
    le_recursion_suspend();
    while (f != NULL && !le_frame_complete(f)) f = le_frame_back(f);
    if (f != NULL)
        fo = le_frame_object_made(f);
    /* each frame's caller is as frame.f_back gives it: an emulated frame
       made current (le_warn) is linked to the next only by f_back */
    while (f != NULL) {
        /* is this Python frame the starting point of a chained
           list of Psyco frames ? */
        fstart = NULL;
        if (!linked) {
            fstart = run != NULL && run->frame == f ? run
                                                    : psyco_frame_start_get(f);
            if (fstart != NULL)
                run = fstart->next;
        }
        linked = fo != NULL &&
                 (!le_frame_is_executing(fo) || live_activation(fo) != NULL);
        if (fstart != NULL && !fstart->resumed) {
            /* Yes. Get the list start. */
            struct sfitmp_s *revlist;
            struct sfitmp_s *p;
            visited_frame_t v;
            PyObject *g;
            long tag;
            stack_frame_info_t **f1;
            stack_frame_info_t **finfo;
            stack_frame_info_t *fdata;
            stack_frame_info_t *flimit;
            int lasti = -1; /* the place of the frame visited next */
            bool returning; /* the innermost frame is clearing its locals on
                               the way out (ABOUT_TO_RETURN): skipped */
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
            /* the innermost frame's place, if it noted one */
            returning = (long)*f1 == FINFO_RETURNING;
            if ((((long)*f1) & 1) != 0 && (long)*f1 != -1 && !returning)
                lasti = psyco_note_map((long)*f1)->lasti;
            else if ((((long)*f1) & 1) == 0)
                lasti = (*f1)->own_lasti;

            /* now actually visit them in the correct order */
            while (revlist) {
                p = revlist;
                /* a Psyco frame is visited as its code, globals, tag
                   (the address of *fi) and place */
                if (result == NULL) {
                    tag = (long)(p->fi);
                    fdata = *p->fi;
                    flimit = finfo_last(fdata);
                    if (returning ||
                        (hidden_frames != NULL && is_hidden(p->fi))) {
                        returning = false;
                        lasti = fdata->back_lasti;
                        if (fdata == flimit)
                            goto next_record;
                        fdata++, tag--;
                    }
                    while (1) {
                        le_frame_t *gf =
                            fdata == *p->fi ? gen_frame_at(p->fi) : NULL;
                        g = fdata->globals;
                        if (g == NULL)
                            g = le_frame_globals(gf != NULL ? gf : f);
                        v.code = (PyObject *)fdata->co;
                        v.globals = g;
                        v.frame = NULL;
                        v.object = NULL;
                        v.tag = tag;
                        v.lasti = lasti;
                        lasti = fdata->back_lasti;
                        result = callback(&v, data);
                        if (result != NULL)
                            break;
                        if (fdata == flimit)
                            break;
                        fdata++, tag--;
                    }
                }
            next_record:
                revlist = p->next;
                PyMem_FREE(p);
            }
            if (result != NULL)
                break;

            /* there is still the real Python frame
               which is shadowed by a Psyco frame, i.e. a
               proxy function. Visited as its code, globals, frame and
               place */
            if (returning) {
                walk_back(&f, &fo);
                continue;
            }
            v.code = (PyObject *)fstart->psy_code;
            v.globals = fstart->psy_globals;
            v.frame = f;
            v.object = fo;
            v.tag = 0;
            v.lasti = lasti;
            result = callback(&v, data);
        }
        else {
            /* a real unshadowed Python frame */
            visited_frame_t real = {NULL, NULL, f, fo, 0, -1};
            result = callback(&real, data);
        }
        if (result != NULL)
            break;
        walk_back(&f, &fo);
    }
    le_recursion_resume();
    return result;
}

static PyObject *
visit_get_globals(const visited_frame_t *v, void *ignored)
{
    if (v->code == NULL)
        return le_frame_globals(v->frame);
    else
        return v->globals;
}

DEFINEFN
PyObject *
psyco_get_globals(void)
{
    PyObject *result = pvisitframes(visit_get_globals, NULL);
    if (result == NULL)
        psyco_fatal_msg("sorry, don't know what to do with no globals");
    return result;
}

/* sys._getframe, replaced (as upstream's support.py did) by a version that
   also counts compiled frames, which have no real frame object: they are
   emulated, and kept while their call runs (kept_frame). */

/* the frame 'depth' calls below the top, or NULL (no exception set),
   with the emulated frames from it out linked by f_back to the next frame
   out (a kept frame already is, from when it was made). A real frame
   keeps its own
   f_back, as the evaluators put tstate->frame back to it; frame.f_back
   gives the compiled frames it skips (frame_get_back). With 'undo', a
   list, for C code that reads f_back itself: real frames are relinked
   too, each as (frame, old f_back) in it, and a frame LeCode_Run runs
   gets its place, as (frame, old f_lasti), for restore_links. */
struct link_s {
    int depth;
    PyFrameObject *top, *prev;
    PyObject *undo;
};

static PyObject *
visit_link_frame(const visited_frame_t *v, void *data)
{
    struct link_s *l = (struct link_s *)data;
    PyFrameObject *f, *back;
    PyObject *t, *old;
    if (l->depth > 0) {
        l->depth--;
        return NULL;
    }
    f = emulate_frame(v);
    if (l->undo != NULL && v->code != NULL && v->frame != NULL &&
        v->lasti >= 0 &&
        (t = Py_BuildValue("Oi", f, le_frame_lasti(le_frame_of(f)))) != NULL) {
        /* a frame LeCode_Run runs, at its current place for the call */
        if (PyList_Append(l->undo, t) == 0)
            le_frame_set_lasti(le_frame_of(f), le_lasti_of(v->lasti));
        Py_DECREF(t);
        PyErr_Clear();
    }
    if (l->top == NULL)
        l->top = f; /* the reference returned */
    else if ((back = le_frame_object_back(l->prev)) == f)
        Py_DECREF(f);
    else if (back == NULL)
        l->prev->f_back = f; /* takes the reference */
    else if (l->undo == NULL ||
             (t = PyTuple_Pack(2, l->prev,
                               old = l->prev->f_back != NULL
                                         ? (PyObject *)l->prev->f_back
                                         : Py_None)) == NULL ||
             PyList_Append(l->undo, t) < 0) {
        Py_DECREF(f);
        PyErr_Clear();
        return Py_None; /* stop: the rest is in the real frame's own chain */
    }
    else {
        Py_DECREF(t);
        Py_XDECREF(l->prev->f_back); /* the tuple holds it */
        l->prev->f_back = f;         /* takes the reference */
    }
    l->prev = f;
    return NULL;
}

static PyFrameObject *
linked_frame(int depth, PyObject *undo)
{
    struct link_s l = {depth, NULL, NULL, undo};
    pvisitframes(visit_link_frame, &l);
    return l.top;
}

static void
restore_links(PyObject *undo)
{
    Py_ssize_t i = PyList_GET_SIZE(undo);
    while (i--) {
        PyObject *t = PyList_GET_ITEM(undo, i);
        PyFrameObject *frame = (PyFrameObject *)PyTuple_GET_ITEM(t, 0);
        PyFrameObject *old, *cur;
        if (PyLong_Check(PyTuple_GET_ITEM(t, 1))) {
            le_frame_set_lasti(le_frame_of(frame),
                               (int)PyLong_AsLong(PyTuple_GET_ITEM(t, 1)));
            continue;
        }
        /* None: it had none, its caller being the thread's chain's */
        old = PyTuple_GET_ITEM(t, 1) == Py_None
                  ? NULL
                  : (PyFrameObject *)PyTuple_GET_ITEM(t, 1);
        cur = frame->f_back;
        Py_XINCREF(old);
        frame->f_back = old;
        Py_XDECREF(cur);
    }
}

/* whether LeCode_Run is running 'f' (its f_lasti is -1, or the place of
   the last traceback entry made for it) */
static bool
runs_compiled(PyFrameObject *f)
{
    FrameRuntime *fr = le_frame_is_executing(f)
                           ? psyco_frame_start_get(le_frame_of(f))
                           : NULL;
    return fr != NULL && !fr->resumed;
}

/* the real frame object 'f', if compiled code runs its code, stands for
   that run (stand_in_word): the word's place is found once a run */
DEFINEFN void
psyco_frame_stands(PyFrameObject *f)
{
    FrameRuntime *fr;
    if (f == NULL || keeps_own_locals(le_pyframe_code(f)) || !runs_compiled(f))
        return;
    fr = psyco_frame_start_get(le_frame_of(f));
    if (fr->psy_code != le_pyframe_code(f))
        return;
    if (fr->word == NULL)
        fr->word = slot_word(*fr->psy_frames_start, fr->psy_code);
    stand_in_word(f, fr->word);
}

/* frame.f_back, replaced in PyFrame_Type's dict. A running real frame
   entered through C from compiled code has as f_back the real frame
   LeCode_Run runs, which skips any compiled frames in
   between: they are found where the frame is in LeJIT's chain and given,
   emulated and linked to the real one. The stored f_back stays, since
   the evaluators put tstate->frame back to it. */
struct back_s {
    PyFrameObject *frame, *back, *top, *prev;
    bool found;
    /* with 'frame' NULL: the compiled callee to find, by its own slot */
    stack_frame_info_t **own;
    PyCodeObject *co;
};

/* a compiled callee's frame kept while its call runs, which finds its
   caller when asked (frame_get_back) */
static bool
kept_callee(PyFrameObject *f)
{
    activation_t *a = live_activation(f);
    return a != NULL && !a->inlined && !a->gen;
}

static PyObject *
visit_back_frame(const visited_frame_t *v, void *data)
{
    struct back_s *b = (struct back_s *)data;
    PyFrameObject *f;
    if (!b->found) {
        if (b->frame == NULL)
            b->found = v->frame == NULL && v->code == (PyObject *)b->co &&
                       (v->tag & (sizeof(long) - 1)) == 0 &&
                       (stack_frame_info_t **)v->tag != b->own &&
                       psyco_next_stack_frame((stack_frame_info_t **)v->tag) ==
                           b->own;
        else
            b->found = v->frame != NULL
                           ? v->frame == le_frame_of(b->frame)
                           : visited_gen_frame(v) == le_frame_of(b->frame);
        return NULL;
    }
    f = emulate_frame(v);
    if (f == NULL)
        return Py_None; /* stop */
    if (b->prev != NULL && b->prev->f_back != NULL) {
        /* a kept frame, linked when it was first asked for */
        Py_DECREF(f);
        return Py_None;
    }
    if (v->frame != NULL) {
        /* a real frame: the one out, as a rule, or the one past it while
           that one returns (pvisitframes) */
        if (b->prev != NULL)
            b->prev->f_back = f; /* takes the reference */
        else
            b->top = f;
        return Py_None;
    }
    if (b->prev == NULL)
        b->top = f; /* the reference returned */
    else
        b->prev->f_back = f;
    /* a generator's frame its body runs on finds its own caller when
       asked, and keeps none; so does a compiled callee's kept frame
       (frame_get_back), whose callers get frame objects only then, as
       CPython's do */
    if (visited_gen_frame(v) != NULL || kept_callee(f))
        return Py_None;
    b->prev = f;
    return NULL;
}

/* The traceback frame of a compiled callee that catches an exception
   stands for it until its call ends: the callee's catcher word, in its
   stack (pycompiler.c:catcher_word), holds a reference to it, and its
   empty value stack holds CATCHER_NOTE, the callee's own slot and the
   word's address. A read finds the callee among this thread's running
   compiled frames by its slot (catcher_slot), and takes its locals
   (view_sync) and caller (frame_get_back) then, so that it shows
   them as they are when read, as CPython's frame does. The note goes
   when the word lets the frame go, so it never outlives the call. */
static char catcher_note;
#define CATCHER_NOTE ((PyObject *)&catcher_note)

static bool
has_catcher_note(PyFrameObject *f)
{
    return !le_frame_is_executing(f) && le_frame_stack_empty(f) &&
           le_pyframe_code(f)->co_stacksize >= 3 &&
           le_pyframe_stack(f)[0] == CATCHER_NOTE;
}

static void
catcher_add(PyFrameObject *f, PyObject **word, stack_frame_info_t **own)
{
    int i, n = 0;
    for (i = 0; i < ncatchers; i++)
        if (*catchers[i].word == (PyObject *)catchers[i].frame)
            catchers[n++] = catchers[i];
    ncatchers = n;
    if (n < CATCHERS_MAX) {
        catchers[n].own = own;
        catchers[n].word = word;
        catchers[n].frame = f;
        ncatchers = n + 1;
    }
}

static void
clear_catcher_note(PyFrameObject *f)
{
    int i;
    for (i = ncatchers; i-- > 0;)
        if (catchers[i].frame == f) {
            catchers[i] = catchers[--ncatchers];
            break;
        }
    if (has_catcher_note(f))
        le_pyframe_stack(f)[0] = le_pyframe_stack(f)[1] =
            le_pyframe_stack(f)[2] = NULL;
}

/* the frame of a traceback entry the catching callee of code 'co' with
   own slot 'own' holds, taken out with its note, or NULL */
static PyFrameObject *
catcher_take(stack_frame_info_t **own, PyCodeObject *co)
{
    int i;
    PyFrameObject *f;
    for (i = ncatchers; i-- > 0;) {
        f = catchers[i].frame;
        if (catchers[i].own == own && *catchers[i].word == (PyObject *)f &&
            !le_frame_is_executing(f) && le_pyframe_code(f) == co) {
            clear_catcher_note(f);
            return f;
        }
    }
    return NULL;
}

/* the run of this thread the compiled callee whose own slot is 'own' is
   in, or NULL */
static FrameRuntime *
run_holding(stack_frame_info_t **own)
{
    FrameRuntime *fr;
    stack_frame_info_t **f1;
    for (fr = frame_runs; fr != NULL; fr = fr->next) {
        if (fr->resumed || !le_frame_executing(fr->frame))
            continue;
        for (f1 = *fr->psy_frames_start;
             (((long)*f1) & 1) == 0 && (*f1)->link_stack_depth != 0;
             f1 = psyco_next_stack_frame(f1))
            if (psyco_next_stack_frame(f1) == own)
                return fr;
    }
    return NULL;
}

/* the catching callee's own slot, if it runs on this thread, with its
   run; its word holds 'f' while the note is there, and the word is in the
   callee's stack */
static stack_frame_info_t **
catcher_slot(PyFrameObject *f, FrameRuntime **run)
{
    stack_frame_info_t **own = (stack_frame_info_t **)le_pyframe_stack(f)[1];
    PyObject **word = (PyObject **)le_pyframe_stack(f)[2];
    if (*word != (PyObject *)f || (*run = run_holding(own)) == NULL)
        return NULL;
    return own;
}

/* the caller of the innermost compiled frame, a compiled callee whose own
   slot is 'own', as a walk visits it, found from the calls innermost_slot
   keeps for its run without a walk; *found false if they don't say */
static PyFrameObject *
caller_in_walk(FrameRuntime *fr, stack_frame_info_t **last,
               stack_frame_info_t **own, size_t n, bool *found);

static PyFrameObject *
innermost_caller(stack_frame_info_t **own, bool *found)
{
    le_frame_t *real;
    stack_frame_info_t **last = innermost_slot(&real);
    struct walked_calls_s *w;
    *found = false;
    if (last == NULL || hidden_frames != NULL ||
        psyco_next_stack_frame(last) != own)
        return NULL;
    w = psyco_frame_start_get(real)->walked;
    return caller_in_walk(psyco_frame_start_get(real), last, own,
                          w == NULL || w == &walked_once ? 0 : w->n, found);
}

/* the caller of a compiled callee whose own slot is 'own' as a walk
   visits it, its record in slot 'last' of run 'fr', whose walk's first
   'n' calls, the callee's the last, are running; *found false if they
   don't say */
static PyFrameObject *
caller_in_walk(FrameRuntime *fr, stack_frame_info_t **last,
               stack_frame_info_t **own, size_t n, bool *found)
{
    stack_frame_info_t **p;
    stack_frame_info_t *rec, *in;
    struct walked_calls_s *w = fr->walked;
    le_frame_t *gf = NULL;
    visited_frame_t v;
    rec = *last;
    v.frame = NULL;
    v.object = NULL;
    v.lasti = rec->back_lasti;
    if (finfo_last(rec) != rec) {
        /* an inlined frame of the frame whose slot is 'last' */
        in = rec + 1;
        v.tag = (long)last - 1;
    }
    else if (last == *fr->psy_frames_start) {
        v.code = (PyObject *)fr->psy_code;
        v.globals = fr->psy_globals;
        v.frame = fr->frame;
        v.object = le_frame_object(fr->frame);
        v.tag = 0;
        *found = true;
        return v.object == NULL ? NULL : emulate_frame(&v);
    }
    else {
        if (n < 3 || w->calls[n - 1].own != own || w->calls[n - 2].own != last)
            return NULL;
        p = w->calls[n - 3].own;
        in = *p;
        v.tag = (long)p;
        gf = gen_frame_at(p);
    }
    v.code = (PyObject *)in->co;
    v.globals = in->globals;
    if (v.globals == NULL)
        v.globals = le_frame_globals(gf != NULL ? gf : fr->frame);
    *found = true;
    return emulate_frame(&v);
}

/* the caller of the call whose own slot is 'own', which has just
   returned, from what run 'fr' walked of it: emulated, or NULL; *found
   false if the walk doesn't say */
static PyFrameObject *
walked_caller(FrameRuntime *fr, stack_frame_info_t **own, bool *found)
{
    struct walked_calls_s *w = fr->walked;
    size_t n = w->n + 1; /* with the call just ended */
    PyFrameObject *f;
    *found = false;
    if (hidden_frames != NULL || n < 2 || w->calls[n - 1].own != own ||
        psyco_next_stack_frame(w->calls[n - 2].own) != own)
        return NULL;
    f = caller_in_walk(fr, w->calls[n - 2].own, own, n, found);
    /* the caller's return is seen too, so that the walk finds its caller
       if its frame outlives it */
    if (f != NULL && n >= 3)
        walked_watch(w, n - 2);
    return f;
}

/* the caller of the compiled callee of code 'co' whose own slot is 'own',
   on this thread: emulated, or NULL. A callee 'returning' is the
   innermost frame, which walks skip. */
static PyFrameObject *
caller_at(stack_frame_info_t **own, PyCodeObject *co, bool returning)
{
    struct back_s b = {NULL, NULL, NULL, NULL, returning, NULL, co};
    PyFrameObject *top;
    FrameRuntime *fr;
    bool found;
    top = innermost_caller(own, &found);
    if (found) {
        PyErr_Clear();
        return top;
    }
    if ((fr = run_holding(own)) == NULL)
        return NULL;
    b.own = own;
    b.back = le_frame_object(fr->frame);
    pvisitframes(visit_back_frame, &b);
    PyErr_Clear();
    if (b.top == NULL && b.found) {
        psyco_frame_stands(b.back);
        Py_INCREF(b.back);
        return b.back;
    }
    return b.top;
}

/* the catching callee's caller, emulated and linked, or NULL */
static PyFrameObject *
catcher_caller(PyFrameObject *f)
{
    stack_frame_info_t **own = (stack_frame_info_t **)le_pyframe_stack(f)[1];
    PyObject **word = (PyObject **)le_pyframe_stack(f)[2];
    if (*word != (PyObject *)f)
        return NULL;
    return caller_at(own, le_pyframe_code(f), false);
}

/* a kept callee's caller, as a walk links it: emulated and linked, or
   NULL if its run isn't on this thread */
static PyFrameObject *
activation_caller(activation_t *a)
{
    return caller_at(a->own, le_pyframe_code(a->frame),
                     (long)*a->own == FINFO_RETURNING);
}

#ifdef PyFrameLocalsProxy_Check
DEFINEVAR int psyco_frames_exposed = 0;

/* the exposed flag of the compiled frame frame object f stands for, if
   it is one that can go on in ceval (psyco_resume_begin finds it): the
   real frame LeCode_Run runs, or a kept frame */
static int *
exposed_flag(PyFrameObject *f)
{
    FrameRuntime *fr;
    activation_t *a;
    if (runs_compiled(f)) {
        fr = psyco_frame_start_get(le_frame_of(f));
        if (fr->psy_code == le_pyframe_code(f) && !fr->resumed)
            return &fr->exposed;
        return NULL;
    }
    if ((a = live_activation(f)) != NULL)
        return &a->exposed;
    return NULL;
}
#endif

/* A frame object that stands for a compiled frame, brought up to date
   with it for each way Python reads it (its getters; locals and vars by
   another name, cimpl_with_frame; gc.get_referents, le_get_referents; the
   C API's readers). VIEW_PLACE: where the compiled frame is now (bytes),
   returned, or -1 where it doesn't say or the frame is traced; a kept
   frame's f_lasti is set there too, not the real frame's, which ceval
   resumes from. VIEW_LOCALS: its locals, read while the compiled frame is
   in a call that says where they are (slot_locals); a real frame compiled
   code runs, a kept frame, an emulated frame and a catching callee's
   traceback frame. Else they stay as last written. An exposed frame's are
   its frame object's already (expose). */
#define VIEW_PLACE 1
#define VIEW_LOCALS 2
static int
root_lasti(PyFrameObject *frame);

static int
view_sync(PyFrameObject *f, int what)
{
    activation_t *a;
    stack_frame_info_t **own;
    FrameRuntime *fr;
    int lasti = -1;
#ifdef PyFrameLocalsProxy_Check
    int *exposed = exposed_flag(f);
    if (exposed != NULL && *exposed)
        what &= ~VIEW_LOCALS;
#endif
    if (f->f_trace != NULL)
        what &= ~VIEW_PLACE;
    if ((what & VIEW_PLACE) && (a = live_activation(f)) != NULL) {
        if ((lasti = kept_lasti(a)) >= 0)
            le_frame_set_lasti(le_frame_of(f), le_lasti_of(lasti));
    }
    else if ((what & VIEW_PLACE) && runs_compiled(f))
        lasti = root_lasti(f);
    if (!(what & VIEW_LOCALS))
        return lasti;
    if (runs_compiled(f)) {
        fr = psyco_frame_start_get(le_frame_of(f));
        if (fr->psy_code == le_pyframe_code(f))
            locals_from_slot(f, *fr->psy_frames_start, 0);
    }
    else if ((a = live_activation(f)) != NULL) {
        locals_from_slot(f, a->inlined ? a->slot : a->own, a->inlined);
    }
    else if (has_catcher_note(f)) {
        if ((own = catcher_slot(f, &fr)) != NULL)
            locals_from_slot(f, own, 0);
    }
    else {
        fill_noted_locals(f);
    }
    return lasti;
}

#ifdef PyFrameLocalsProxy_Check
/* PEP 667's frame-locals proxy reads and writes the frame object's locals
   directly. Each use brings them up to date first. A write makes them the
   compiled frame's (exposed): they are left to the proxy from then on, and
   the compiled frame goes on in ceval with them at its next resume test,
   which follows the instruction (pycompiler.c:resume_if_tracing). A frame
   that can't go on there (an emulated or inlined frame) does not see what
   is written. */
static void
expose(PyFrameObject *f, int write)
{
    int *exposed = exposed_flag(f);
    if (exposed != NULL && *exposed)
        return;
    view_sync(f, VIEW_LOCALS);
    if (write && exposed != NULL) {
        *exposed = 1;
        psyco_frames_exposed++;
#if LE_HAS_MONITORING
        le_quiet_version = (uint64_t)-1; /* compiled code's test fails */
#endif
    }
}

/* the compiled frame for code 'co' that a resume would go on with
   (psyco_resume_begin) is exposed */
static int
exposed_here(PyCodeObject *co)
{
    le_frame_t *real;
    stack_frame_info_t **slot = innermost_slot(&real);
    activation_t *a;
    FrameRuntime *fr;
    if ((a = inline_kept_at(slot, real, co)) != NULL)
        return a->exposed;
    if (slot == NULL && real != NULL && le_frame_code(real) == co) {
        fr = psyco_frame_start_get(real);
        return fr != NULL && fr->exposed;
    }
    if (slot != NULL && (a = act_at(slot, co)) != NULL && !a->resumed)
        return a->exposed;
    return 0;
}

DEFINEFN int
psyco_frame_goes_interpreted(PyCodeObject *co)
{
    return le_tracing_now(PyThreadState_GET(), co) || exposed_here(co);
}
#endif

/* a field a member descriptor reads and writes, as a getset's functions
   ('field' is its definition) */
static PyObject *
member_get(PyObject *obj, void *field)
{
    return PyMember_GetOne((const char *)obj, (PyMemberDef *)field);
}
static int
member_set(PyObject *obj, PyObject *v, void *field)
{
    return PyMember_SetOne((char *)obj, (PyMemberDef *)field, v);
}

/* LeJIT's getter or setter for type 'tp''s attribute 'name', given in
   'ours' (NULL keeps CPython's), which takes the rest of CPython's
   definition. A getset descriptor is pointed at 'ours': it stays the one
   CPython's caches and specialised instructions may hold, so the type
   needn't be given a new version tag, which a static builtin type mustn't
   be. A member descriptor (a field, as f_back and f_lasti are where the
   frame object has them) is replaced in the dict by a getset descriptor
   for 'ours', and the type is given a new version tag. Returns CPython's
   definition, or NULL if the type has neither. */
static PyGetSetDef *
getset_replace(PyTypeObject *tp, const char *name, PyGetSetDef *ours)
{
    PyObject *d = le_type_dict(tp);
    PyObject *descr = PyDict_GetItemString(d, name), *fn;
    PyGetSetDef *cpython;
    if (descr != NULL && Py_TYPE(descr) == &PyGetSetDescr_Type)
        cpython = ((PyGetSetDescrObject *)descr)->d_getset;
    else if (descr != NULL && Py_TYPE(descr) == &PyMemberDescr_Type) {
        PyMemberDef *field = ((PyMemberDescrObject *)descr)->d_member;
        cpython = PyMem_Malloc(sizeof(PyGetSetDef));
        if (cpython == NULL)
            OUT_OF_MEMORY();
        cpython->name = field->name;
        cpython->get = member_get;
        cpython->set = member_set;
        cpython->doc = field->doc;
        cpython->closure = field;
    }
    else
        return NULL;
    ours->name = cpython->name;
    ours->doc = cpython->doc;
    ours->closure = cpython->closure;
    if (ours->get == NULL)
        ours->get = cpython->get;
    if (ours->set == NULL)
        ours->set = cpython->set;
    if (Py_TYPE(descr) == &PyGetSetDescr_Type) {
        ((PyGetSetDescrObject *)descr)->d_getset = ours;
        return cpython;
    }
    fn = PyDescr_NewGetSet(tp, ours);
    if (fn == NULL || PyDict_SetItemString(d, name, fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    PyType_Modified(tp);
    return cpython;
}

/* the same for a method descriptor, whose function 'meth' must take its
   arguments as CPython's does ('flags'), as the descriptor calls it */
static PyMethodDef *
method_replace(PyTypeObject *tp, const char *name, int flags, PyCFunction meth,
               PyMethodDef *ours)
{
    PyObject *descr = PyDict_GetItemString(le_type_dict(tp), name);
    PyMethodDef *cpython;
    if (descr == NULL || Py_TYPE(descr) != &PyMethodDescr_Type ||
        ((PyMethodDescrObject *)descr)->d_method->ml_flags != flags)
        return NULL;
    cpython = ((PyMethodDescrObject *)descr)->d_method;
    *ours = *cpython;
    ours->ml_meth = meth;
    ((PyMethodDescrObject *)descr)->d_method = ours;
    return cpython;
}

static PyObject *
frame_get_back(PyFrameObject *frame, void *closure)
{
    PyFrameObject *back = le_frame_object_back(frame);
    activation_t *a;
    if (back == NULL && has_catcher_note(frame))
        back = frame->f_back = catcher_caller(frame); /* takes the reference */
    else if (back == NULL && kept_callee(frame))
        back = frame->f_back = activation_caller(live_activation(frame));
    if (back == NULL)
        Py_RETURN_NONE;
    /* a generator's frame its body runs on as a direct call is linked
       only to the frame the call was made from */
    if (le_frame_is_executing(frame) && runs_compiled(back) &&
        ((a = live_activation(frame)) == NULL || a->gen)) {
        struct back_s b = {frame, back, NULL, NULL, false, NULL, NULL};
        pvisitframes(visit_back_frame, &b);
        PyErr_Clear();
        if (b.top != NULL)
            return (PyObject *)b.top;
    }
    Py_INCREF(back);
    return (PyObject *)back;
}

static PyGetSetDef back_getset = {NULL, (getter)frame_get_back};

/* frame.f_generator, where frames have it, replaced too: a running
   coroutine's frame gives the coroutine, so those of the bodies started
   by their awaits are made first (psyco_coros_realise) */
static PyGetSetDef *generator_cpython;

static PyObject *
frame_get_generator(PyFrameObject *frame, void *closure)
{
    psyco_coros_realise();
    return generator_cpython->get((PyObject *)frame,
                                  generator_cpython->closure);
}

static PyGetSetDef generator_getset = {NULL, (getter)frame_get_generator};

/* frame.f_lineno, replaced too: a frame LeCode_Run runs doesn't keep its
   f_lasti, so its place comes from LeJIT's chain, which visits it with
   it. The setter (a jump, under tracing) is ceval's. */
static PyGetSetDef *lineno_cpython;

/* the place of the frame LeCode_Run runs, 'frame' in it, or -1 */
struct root_lasti_s {
    PyFrameObject *frame;
    int lasti;
};

static PyObject *
visit_root_lasti(const visited_frame_t *v, void *data)
{
    struct root_lasti_s *r = (struct root_lasti_s *)data;
    if (v->code != NULL && v->frame == le_frame_of(r->frame)) {
        r->lasti = v->lasti;
        return Py_None; /* stop */
    }
    return NULL;
}

static int
root_lasti(PyFrameObject *frame)
{
    struct root_lasti_s r = {frame, -1};
    pvisitframes(visit_root_lasti, &r);
    return r.lasti;
}

/* an instruction with no line (3.10's line table) has the line None */
static PyObject *
line_object(int line)
{
    if (line < 0)
        Py_RETURN_NONE;
    return PyLong_FromLong(line);
}

static PyObject *
frame_get_lineno(PyFrameObject *frame, void *closure)
{
    int lasti = view_sync(frame, VIEW_PLACE);
    if (lasti >= 0 && runs_compiled(frame))
        return line_object(PyCode_Addr2Line(le_pyframe_code(frame), lasti));
    return line_object(PyFrame_GetLineNumber(frame));
}

static int
frame_set_lineno(PyFrameObject *frame, PyObject *v, void *c)
{
    return lineno_cpython->set((PyObject *)frame, v, lineno_cpython->closure);
}

static PyGetSetDef lineno_getset = {NULL, (getter)frame_get_lineno,
                                    (setter)frame_set_lineno};

/* frame.f_lasti, replaced too, from the same place as f_lineno */
static PyGetSetDef *lasti_cpython;

static PyObject *
frame_get_lasti(PyFrameObject *frame, void *closure)
{
    int lasti = view_sync(frame, VIEW_PLACE);
    if (lasti >= 0 && runs_compiled(frame))
        return PyLong_FromLong(lasti);
    return lasti_cpython->get((PyObject *)frame, lasti_cpython->closure);
}

static PyGetSetDef lasti_getset = {NULL, (getter)frame_get_lasti};

/* frame.f_locals, replaced too: PyFrame_FastToLocals
   reads f_localsplus, which view_sync brings up to date */
static PyGetSetDef *locals_cpython;

static PyObject *
frame_get_locals(PyFrameObject *frame, void *closure)
{
    view_sync(frame, VIEW_LOCALS);
    return locals_cpython->get((PyObject *)frame, locals_cpython->closure);
}

static PyGetSetDef locals_getset = {NULL, (getter)frame_get_locals};

/* frame.clear(), replaced too: a frame object that stands for a call
   compiled code still runs, without being running itself (the catching
   callee's traceback frame, an emulated frame), is refused as CPython
   refuses its running frames, by CPython's method with the frame marked
   running for the call */
static PyMethodDef *clear_cpython, clear_def;

static bool
stands_for_running(PyFrameObject *f)
{
    FrameRuntime *fr;
    PyObject *found;
    if (le_frame_is_executing(f))
        return false; /* CPython's method refuses it itself */
    if (has_catcher_note(f))
        return catcher_slot(f, &fr) != NULL;
    if (!le_frame_stack_empty(f) || le_pyframe_code(f)->co_stacksize < 2 ||
        le_pyframe_stack(f)[0] != LOCALS_NOTE)
        return false;
    found = pvisitframes(visit_tag, f);
    Py_XDECREF(found);
    return found != NULL;
}

static PyObject *
frame_clear(PyFrameObject *frame, PyObject *unused)
{
    PyObject *result;
    int old = 0, mark;
#if LE_RESUMES_GENERATORS_DIRECTLY
    le_frame_t *f = le_frame_of(frame);
    /* a record's frame: running, CPython's method refuses it as the
       thread's; paused, its generator's, which the clear closes */
    if (le_frame_is_record(f) && le_frame_executing(f)) {
        f->owner = FRAME_OWNED_BY_THREAD;
        result = clear_cpython->ml_meth((PyObject *)frame, NULL);
        f->owner = LE_FRAME_OWNED_BY_RECORD;
        return result;
    }
    if (le_frame_is_record(f)) {
        PyObject *gen = le_gen_record_escape_frame(f);
        if (gen == NULL)
            return NULL;
        Py_DECREF(gen);
    }
#endif
    mark = stands_for_running(frame);
    if (mark)
        old = le_pyframe_mark_running(frame);
    result = clear_cpython->ml_meth((PyObject *)frame, NULL);
    if (mark)
        le_pyframe_unmark_running(frame, old);
    return result;
}

/* A traceback entry for a compiled frame, 'f', is being added over
   'callee''s, the frame the exception came out of: that frame, if it has
   returned, gets 'f' as f_back. It is an emulated frame, whose f_back is
   none or another emulated frame of the same code made when its entry
   was, or a real frame entered through C whose f_back is the frame
   LeCode_Run runs, while the code that one runs is in a compiled callee
   or inlined frame, which 'f' is. A fused generator's or coroutine's frame is
   left without one, as it has no caller once it has stopped. */
DEFINEFN
void
psyco_frame_link_callee(PyFrameObject *callee, PyFrameObject *f)
{
    PyFrameObject *back = callee->f_back;
    FrameRuntime *fr;
    if (callee == f || back == f || le_frame_is_executing(callee) ||
        (le_pyframe_code(callee)->co_flags &
         (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)))
        return;
    if (back != NULL && !(le_pyframe_code(back) == le_pyframe_code(f) &&
                          !le_frame_is_executing(back))) {
        if (!runs_compiled(back))
            return;
        /* the root slot is even while that code is in a compiled callee or
           inlined frame (psyco_running_real_frame) */
        fr = psyco_frame_start_get(le_frame_of(back));
        if ((((long)**fr->psy_frames_start) & 1) != 0)
            return;
    }
    Py_INCREF(f);
    callee->f_back = f;
    Py_XDECREF(back);
}

/* A frame that has just run and outlives its call keeps as f_back the
   frame it was called from. Where that is a real frame LeCode_Run runs
   while its code is in a compiled callee or inlined frame, which made
   the call, f_back is that frame instead, emulated and linked to the ones
   out of it, as CPython's would be: frame_get_back can't find it once the
   call is over. */
DEFINEFN
int
psyco_frame_called_from_callee(le_frame_t *back)
{
    FrameRuntime *fr = back == NULL ? NULL : psyco_frame_start_get(back);
    /* the root slot is even while the code is in a compiled callee or
       inlined frame (psyco_running_real_frame) */
    return fr != NULL && !fr->resumed && le_frame_executing(back) &&
           (((long)**fr->psy_frames_start) & 1) == 0;
}

DEFINEFN
PyFrameObject *
psyco_frame_callee_back(void)
{
    PyFrameObject *top;
    PyObject *t, *v, *tb;
    PyErr_Fetch(&t, &v, &tb);
    top = linked_frame(0, NULL);
    PyErr_Clear();
    PyErr_Restore(t, v, tb);
    return top;
}

/* 'f' or the first complete frame out of it, or NULL */
static le_frame_t *
complete_frame(le_frame_t *f)
{
    while (f != NULL && !le_frame_complete(f)) f = le_frame_back(f);
    return f;
}

DEFINEFN
void
psyco_frame_back_stands(le_frame_t *back)
{
    PyFrameObject *fo;
    back = complete_frame(back);
    if (back == NULL)
        return;
#if LE_HAS_INTERPRETER_FRAME
    fo = back->frame_obj;
#else
    fo = back;
#endif
    if (fo != NULL && Py_REFCNT(fo) > 1)
        psyco_frame_stands(fo);
}

DEFINEFN
void
psyco_frame_returned(le_frame_t *f)
{
    PyFrameObject *top;
    le_frame_t *back = le_frame_back(f);
    if (!psyco_frame_called_from_callee(back)) {
        /* the frame it was called from is its f_back */
        back = complete_frame(back);
        if (back != NULL)
            psyco_frame_stands(le_frame_object(back));
        return;
    }
    top = psyco_frame_callee_back();
    if (top != NULL)
        le_frame_set_back_at_exit(f, top);
}

DEFINEFN
void
psyco_frame_catch(PyFrameObject *f, PyObject **word, stack_frame_info_t **own)
{
    PyFrameObject *old = (PyFrameObject *)*word;
    if (old != f) {
        Py_INCREF(f);
        *word = (PyObject *)f;
        if (old != NULL) {
            /* another frame object stood for the call (a kept frame made
               since): it keeps what it was last given */
            clear_catcher_note(old);
            Py_DECREF(old);
        }
    }
    /* a traceback frame made for the entry (cimpl_new_frame), not the
       real frame or a kept one, which are running; nor a generator's
       where CPython leaves one that has ended without a caller */
    if (!le_frame_is_executing(f) && le_frame_stack_empty(f) &&
        le_pyframe_code(f)->co_stacksize >= 3 &&
        !(LE_HAS_FINISHED_GENERATOR_UNLINKED &&
          (le_pyframe_code(f)->co_flags &
           (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)))) {
#if ALL_CHECKS
        le_frame_t *real;
        stack_frame_info_t **last = innermost_slot(&real);
        extra_assert(last != NULL && psyco_next_stack_frame(last) == own);
#endif
        le_pyframe_stack(f)[0] = CATCHER_NOTE;
        le_pyframe_stack(f)[1] = (PyObject *)own;
        le_pyframe_stack(f)[2] = (PyObject *)word;
    }
    if (!le_frame_is_executing(f) && le_frame_stack_empty(f))
        catcher_add(f, word, own);
}

/* what holds a catcher's frame besides its word: a real or kept frame,
   running, is held by its caller or the kept frames' table too */
static Py_ssize_t
catcher_holders(PyFrameObject *f)
{
    return le_frame_is_executing(f) ? 2 : 1;
}

static void
catcher_release(PyObject **word)
{
    PyFrameObject *f = (PyFrameObject *)*word;
    PyObject *t, *v, *tb;
    *word = NULL;
    clear_catcher_note(f);
    PyErr_Fetch(&t, &v, &tb);
    Py_DECREF(f);
    PyErr_Restore(t, v, tb);
}

DEFINEFN
long
psyco_frame_catcher_shared(PyObject **word)
{
    PyFrameObject *f = (PyFrameObject *)*word;
    if (f == NULL)
        return 0;
    if (Py_REFCNT(f) > catcher_holders(f))
        return 1;
    catcher_release(word);
    return 0;
}

DEFINEFN
void
psyco_frame_catcher_end(PyObject **word)
{
    PyFrameObject *f = (PyFrameObject *)*word;
    PyObject *t, *v, *tb;
    if (f == NULL)
        return;
    if (f->f_back == NULL && has_catcher_note(f)) {
        PyErr_Fetch(&t, &v, &tb);
        f->f_back = catcher_caller(f);
        PyErr_Restore(t, v, tb);
    }
    catcher_release(word);
}
DEFINEFN
PyFrameObject *
psyco_running_real_frame(PyCodeObject *co)
{
    le_frame_t *f = le_frame_current(PyThreadState_GET());
    FrameRuntime *fstart;
    if (f == NULL || le_frame_code(f) != co)
        return NULL;
    fstart = psyco_frame_start_get(f);
    /* the top compiled frame's finfo slot is odd (-1) except while it is in a
       compiled sub-call (ABOUT_TO_CALL_SUBFUNCTION) */
    if (fstart == NULL || fstart->resumed ||
        (((long)**fstart->psy_frames_start) & 1) == 0)
        return NULL;
    return le_frame_object(f);
}

static PyObject *original_getframe = NULL;

static PyObject *
getframe_at(int depth);

/* sys._getframe called with anything but no argument or an exact int:
   the arguments as a tuple and a dict, as PyArg_ParseTuple takes them */
static PyObject *
getframe_parsed(PyObject *args, PyObject *kwds)
{
    int depth = 0;
    if ((kwds != NULL && PyDict_GET_SIZE(kwds) != 0) ||
        !PyArg_ParseTuple(args, "|i:_getframe", &depth)) {
        /* arguments sys._getframe refuses (it is replaced only once read):
           its own parsing raises, with the version's messages, from 3.8
           Argument Clinic's */
        PyObject *r;
        PyErr_Clear();
        r = PyObject_Call(original_getframe, args, kwds);
        if (r != NULL) { /* never: both refuse the same arguments */
            Py_DECREF(r);
            PyErr_BadArgument();
        }
        return NULL;
    }
    return getframe_at(depth);
}

/* METH_FASTCALL, so that a call makes no argument tuple, as sys._getframe's
   own makes none from 3.8 */
static PyObject *
le_getframe(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
            PyObject *kwnames)
{
    PyObject *tuple, *kwds = NULL, *r;
    Py_ssize_t i, nkw = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    if (nkw == 0 && nargs == 0)
        return getframe_at(0);
    if (nkw == 0 && nargs == 1 && PyLong_CheckExact(args[0])) {
        int overflow;
        long depth = PyLong_AsLongAndOverflow(args[0], &overflow);
        if (!overflow && depth >= INT_MIN && depth <= INT_MAX)
            return getframe_at((int)depth);
    }
    tuple = PyTuple_New(nargs);
    if (tuple == NULL)
        return NULL;
    for (i = 0; i < nargs; i++) {
        Py_INCREF(args[i]);
        PyTuple_SET_ITEM(tuple, i, args[i]);
    }
    if (nkw > 0 && (kwds = PyDict_New()) == NULL) {
        Py_DECREF(tuple);
        return NULL;
    }
    for (i = 0; i < nkw; i++)
        if (PyDict_SetItem(kwds, PyTuple_GET_ITEM(kwnames, i),
                           args[nargs + i]) < 0) {
            Py_DECREF(kwds);
            Py_DECREF(tuple);
            return NULL;
        }
    r = getframe_parsed(tuple, kwds);
    Py_XDECREF(kwds);
    Py_DECREF(tuple);
    return r;
}

static PyObject *
getframe_at(int depth)
{
    PyFrameObject *top;
    if (depth < 0)
        depth = 0;
#if LE_HAS_AUDIT_HOOKS && !LE_HAS_GETFRAME_AUDITS_RESULT
    {
        /* sys._getframe's audit event names the current frame */
        PyFrameObject *cur = linked_frame(0, NULL);
        int err = PySys_Audit("sys._getframe", "O",
                              cur != NULL ? (PyObject *)cur : Py_None);
        Py_XDECREF(cur);
        if (err < 0)
            return NULL;
    }
#endif
    top = linked_frame(depth, NULL);
    if (top == NULL) {
        PyErr_SetString(PyExc_ValueError, LE_MSG_CALL_STACK_NOT_DEEP);
        return NULL;
    }
#if LE_HAS_GETFRAME_AUDITS_RESULT
    /* the audit event names the frame returned */
    if (PySys_Audit("sys._getframe", "(O)", (PyObject *)top) < 0) {
        Py_DECREF(top);
        return NULL;
    }
#endif
    return (PyObject *)top;
}

#if LE_HAS_FRAME_FUNCOBJ
/* sys._getframemodulename, where there is one, walks the thread's frames
   in C: replaced, as sys._getframe is, by a version that counts compiled
   frames. The module is the frame's function's, as there; a frame
   LeJIT emulates has the function PyFrame_New made it, whose module is
   its globals' __name__. */
static PyObject *original_getframemodulename = NULL;

static PyObject *
le_getframemodulename(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"depth", NULL};
    int depth = 0;
    PyFrameObject *f;
    PyObject *func, *r = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i:_getframemodulename",
                                     kwlist, &depth)) {
        PyErr_Clear();
        r = PyObject_Call(original_getframemodulename, args, kwds);
        if (r != NULL) { /* never: both refuse the same arguments */
            Py_DECREF(r);
            PyErr_BadArgument();
        }
        return NULL;
    }
    if (PySys_Audit("sys._getframemodulename", "i", depth) < 0)
        return NULL;
    f = linked_frame(depth < 0 ? 0 : depth, NULL);
    if (f == NULL) {
        if (PyErr_Occurred())
            return NULL;
        Py_RETURN_NONE;
    }
    func = LE_FRAME_FUNCOBJ(le_frame_of(f));
    if (func != NULL && PyFunction_Check(func)) {
        r = PyFunction_GetModule(func);
        if (r == NULL) {
            PyErr_Clear();
            r = Py_None;
        }
    }
    Py_INCREF(r);
    Py_DECREF(f);
    return r;
}

static PyMethodDef le_getframemodulename_def = {
    "_getframemodulename", (PyCFunction)(void (*)(void))le_getframemodulename,
    METH_VARARGS | METH_KEYWORDS,
    "_getframemodulename(depth=0) -> str or None\n\n"
    "Return the name of the module of a frame on the call stack, counting\n"
    "frames of code compiled by LeJIT (emulated)."};

static void
getframemodulename_init(PyObject *sysmod)
{
    PyObject *name, *fn = NULL;
    original_getframemodulename =
        PyObject_GetAttrString(sysmod, "_getframemodulename");
    if (original_getframemodulename == NULL) {
        PyErr_Clear();
        return;
    }
    name = PyUnicode_FromString("sys");
    if (name != NULL)
        fn = PyCFunction_NewEx(&le_getframemodulename_def, NULL, name);
    if (fn == NULL ||
        PyObject_SetAttrString(sysmod, "_getframemodulename", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    Py_XDECREF(name);
}
#endif

static PyFrameObject *
root_copy(PyFrameObject *f);
static bool
capi_compiled_innermost(PyThreadState *ts);
static PyObject *
visit_innermost(const visited_frame_t *v, void *unused);

/* set while a frame is found: the C API calls made meanwhile are
   CPython's own */
static _Thread_local int capi_finding LE_FAST_TLS;

/* For the length of one call that reads the thread's frames itself (C's
   readers, the warnings module, the frame builtins): the innermost frame
   Python sees is made a frame object and made the thread's current frame,
   and with LINK_ALL every frame out from it is linked to the next by
   f_back too, as tstate's chain would have them; frames_unlinked puts it
   all back. LINK_LOCALS: the innermost frame's locals are its compiled
   frame's now; LINK_COPY_ROOT: a real frame compiled code runs is given
   to the call as a copy with them, which the call may write. Any
   exception set stays. */
#define LINK_ALL 1
#define LINK_LOCALS 2
#define LINK_COPY_ROOT 4
typedef struct {
    le_frame_t *saved;  /* the thread's current frame before */
    PyFrameObject *top; /* the innermost frame (a reference), or NULL */
    PyObject *undo;     /* the links to put back (restore_links), or NULL */
} linked_t;

static void
frames_linked(linked_t *l, int how)
{
    PyThreadState *ts = PyThreadState_GET();
    PyObject *t, *v, *tb;
    l->saved = le_frame_current(ts);
    l->top = NULL;
    l->undo = NULL;
    PyErr_Fetch(&t, &v, &tb);
    capi_finding = 1;
    if (!(how & LINK_ALL))
        l->top = (PyFrameObject *)pvisitframes(visit_innermost, NULL);
    else if ((l->undo = PyList_New(0)) != NULL)
        l->top = linked_frame(0, l->undo);
    if (l->top != NULL && (how & LINK_COPY_ROOT) && runs_compiled(l->top))
        l->top = root_copy(l->top);
    else if (l->top != NULL)
        view_sync(l->top, VIEW_PLACE | (how & (LINK_LOCALS | LINK_COPY_ROOT)
                                            ? VIEW_LOCALS
                                            : 0));
    if (l->top != NULL && le_frame_of(l->top) != l->saved)
        le_pyframe_make_current(ts, l->top);
    capi_finding = 0;
    PyErr_Clear();
    PyErr_Restore(t, v, tb);
}

static void
frames_unlinked(linked_t *l)
{
    le_frame_set_current(PyThreadState_GET(), l->saved);
    if (l->top != NULL && le_frame_of(l->top) != l->saved)
        le_pyframe_current_end(l->top);
    if (l->undo != NULL) {
        restore_links(l->undo);
        Py_DECREF(l->undo);
    }
}

/* fn(arg) run with every frame linked and the innermost current
   (frames_linked), for the signal handlers ceval would run here
   (pyruntime.c:le_eval_breaker_handler): a handler is given the frame the
   signal landed in */
int
le_with_frames_linked(int (*fn)(void *), void *arg)
{
    linked_t l = {NULL, NULL, NULL};
    bool compiled =
        frame_runs != NULL && capi_compiled_innermost(PyThreadState_GET());
    int r;
    if (compiled)
        frames_linked(&l, LINK_ALL);
    r = fn(arg);
    if (compiled)
        frames_unlinked(&l);
    Py_XDECREF(l.top);
    return r;
}

/* _warnings.warn walks tstate->frame in C: it runs with the compiled
   frames over the current real frame emulated and current. */
static PyObject *original_warn = NULL;
DEFINEVAR PyCFunction le_c_warn = NULL;

DEFINEFN
PyObject *
le_warn_with_frames(PyObject *warn, PyObject *args, PyObject *kwds)
{
    linked_t l;
    PyObject *result;
    frames_linked(&l, LINK_ALL);
    result = PyObject_Call(warn, args, kwds);
    frames_unlinked(&l);
    Py_XDECREF(l.top);
    return result;
}

static PyObject *
le_warn(PyObject *self, PyObject *args, PyObject *kwds)
{
    (void)self;
    return le_warn_with_frames(original_warn, args, kwds);
}

/* A warning C raises (_warnings.c:setup_context) names the frame
   PyThreadState_GetFrame gives at the line PyFrame_GetLineNumber reads
   from its fields, so under compiled code the frame or line is wrong: C
   inside CPython may see the innermost real frame (tstate's own), and a
   frame LeCode_Run runs reads as at its first line. C shows the finished
   message through warnings._showwarnmsg while the compiled frames are
   still on the stack, so a replacement of it puts the place right, as
   sys._getframe would see it, before the original shows it. Only a message
   naming the place C could have found (in the real frame or the true one,
   at the line its fields give), with no source line given, is changed. The
   filters and the registries were applied at the place C found. */
static PyObject *original_showwarnmsg = NULL;
/* the message's attribute names, interned once: a name made for each
   lookup is kept by the type's attribute cache */
static PyObject *msg_line, *msg_filename, *msg_lineno, *msg_module;

/* the thread's innermost frame as sys._getframe sees it: its code,
   globals, current line (None if not known), and its frame object if it
   is a real frame */
struct warning_frame_s {
    PyFrameObject *frame;
    PyCodeObject *code;
    PyObject *globals;
    PyObject *line;
};

static PyObject *
visit_warning_frame(const visited_frame_t *v, void *data)
{
    struct warning_frame_s *in = (struct warning_frame_s *)data;
    in->frame = v->frame != NULL ? visited_object(v) : NULL;
    if (v->code == NULL) {
        in->code = le_frame_code(v->frame);
        in->globals = le_frame_globals(v->frame);
    }
    else {
        in->code = (PyCodeObject *)v->code;
        in->globals = v->globals;
    }
    if (in->frame != NULL)
        in->line = frame_get_lineno(in->frame, NULL);
    else if (v->lasti >= 0)
        in->line = line_object(PyCode_Addr2Line(in->code, v->lasti));
    else
        Py_INCREF(Py_None), in->line = Py_None;
    return Py_None;
}

/* the module a warning names for a frame's globals, as setup_context
   finds it */
static PyObject *
warning_module(PyObject *globals)
{
    PyObject *name = PyDict_GetItemString(globals, "__name__");
    if (name != NULL && PyUnicode_Check(name)) {
        Py_INCREF(name);
        return name;
    }
    return PyUnicode_FromString("<string>");
}

/* whether a message's attribute equals 'expected': -1 with an exception
   set if it can't be read */
static int
message_attr_is(PyObject *msg, PyObject *name, PyObject *expected)
{
    PyObject *value = PyObject_GetAttr(msg, name);
    int r;
    if (value == NULL)
        return -1;
    r = PyObject_RichCompareBool(value, expected, Py_EQ);
    Py_DECREF(value);
    return r;
}

/* whether the message names 'f' at the line its fields give: -1 with an
   exception set if it can't be read */
static int
names_fields_place(PyObject *msg, PyFrameObject *f)
{
    PyObject *line;
    int r = message_attr_is(msg, msg_filename,
                            le_frame_code(le_frame_of(f))->co_filename);
    if (r != 1)
        return r;
    if ((line = PyLong_FromLong(PyFrame_GetLineNumber(f))) == NULL)
        return -1;
    r = message_attr_is(msg, msg_lineno, line);
    Py_DECREF(line);
    return r;
}

static int
correct_place(PyObject *msg, PyFrameObject *real, struct warning_frame_s *top)
{
    PyObject *real_mod = NULL, *top_mod = NULL, *line;
    PyObject *real_globals = le_frame_globals(le_frame_of(real));
    bool found_top = false;
    int r, same;
    if (top->line == Py_None)
        return 0;
    if ((line = PyObject_GetAttr(msg, msg_line)) == NULL)
        return -1;
    same = line != Py_None;
    Py_DECREF(line);
    if (same)
        return 0; /* a source line given: not setup_context's */
    /* already the true place */
    if ((same = message_attr_is(msg, msg_filename, top->code->co_filename)) ==
        1)
        same = message_attr_is(msg, msg_lineno, top->line);
    if (same != 0)
        return same < 0 ? -1 : 0;
    if ((same = names_fields_place(msg, real)) == 0 && top->frame != NULL &&
        top->frame != real) {
        found_top = true;
        same = names_fields_place(msg, top->frame);
    }
    if (same != 1)
        return same < 0 ? -1 : 0;
    if (PyObject_SetAttr(msg, msg_filename, top->code->co_filename) < 0 ||
        PyObject_SetAttr(msg, msg_lineno, top->line) < 0)
        return -1;
    if (found_top || real_globals == top->globals)
        return 0;
    /* a message with the module, where WarningMessage has one */
    r = -1;
    if ((real_mod = warning_module(real_globals)) != NULL &&
        (top_mod = warning_module(top->globals)) != NULL &&
        (same = message_attr_is(msg, msg_module, real_mod)) >= 0)
        r = same == 1 ? PyObject_SetAttr(msg, msg_module, top_mod) : 0;
    Py_XDECREF(real_mod);
    Py_XDECREF(top_mod);
    return r;
}

static PyObject *
le_showwarnmsg(PyObject *self, PyObject *msg)
{
    /* the thread's own innermost frame, as C inside CPython may read it */
    le_frame_t *f = le_frame_current(PyThreadState_GET());
    PyFrameObject *real = NULL;
    struct warning_frame_s top = {NULL, NULL, NULL, NULL};
    (void)self;
    while (f != NULL && !le_frame_complete(f)) f = le_frame_back(f);
    if (f != NULL && (real = le_frame_object(f)) == NULL)
        return PyErr_Occurred() ? NULL : PyErr_NoMemory();
    if (real != NULL)
        pvisitframes(visit_warning_frame, &top);
    if (top.code != NULL) {
        int err = top.line == NULL ? -1 : correct_place(msg, real, &top);
        Py_XDECREF(top.line);
        /* a message without the attributes (a WarningMessage without the
           module, before 3.15) is shown as it is */
        if (err < 0 && !PyErr_ExceptionMatches(PyExc_AttributeError))
            return NULL;
        PyErr_Clear();
    }
    return PyObject_CallFunctionObjArgs(original_showwarnmsg, msg, NULL);
}

static PyMethodDef le_showwarnmsg_def = {"_showwarnmsg", le_showwarnmsg,
                                         METH_O, NULL};

/* locals, vars, dir, eval and exec read their caller's frame through
   PyEval_GetFrame, which is tstate->frame. Called from compiled code
   (through a meta of each: code naming them is left to the interpreter,
   mergepoints.c, but they are reached by other names), they run as warn
   does, with the calling frame emulated, its locals in, and current. A
   real frame LeCode_Run runs stands in as a copy with its locals now. */
#define N_FRAME_BUILTINS 5
static const char *const frame_builtin_names[N_FRAME_BUILTINS] = {
    "locals", "vars", "dir", "eval", "exec"};
static PyMethodDef *frame_builtins[N_FRAME_BUILTINS];

static PyObject *
frame_get_back(PyFrameObject *frame, void *closure);

static PyFrameObject *
root_copy(PyFrameObject *f)
{
    FrameRuntime *fr = psyco_frame_start_get(le_frame_of(f));
    struct root_lasti_s r = {f, -1};
    PyFrameObject *c;
    bool found;
    if (fr->psy_code != le_pyframe_code(f))
        return f;
    c = psyco_build_pyframe(
        (PyObject *)le_pyframe_code(f), le_frame_globals(le_frame_of(f)),
        le_frame_function(le_frame_of(f)), PyThreadState_GET());
    if (!slot_locals(*fr->psy_frames_start, le_pyframe_code(f),
                     le_frame_args(le_frame_of(c)), 0)) {
        Py_DECREF(c);
        return f;
    }
    le_frame_words_own(le_frame_of(c), n_locals_plus(le_pyframe_code(f)));
    found = pvisitframes(visit_root_lasti, &r) != NULL;
    le_frame_set_lasti(le_frame_of(c), found ? le_lasti_of(r.lasti) : -1);
    c->f_back = (PyFrameObject *)frame_get_back(f, NULL);
    if (c->f_back == NULL) {
        PyErr_Clear();
    }
    else if ((PyObject *)c->f_back == Py_None) {
        Py_DECREF(Py_None);
        c->f_back = NULL;
    }
    Py_DECREF(f);
    return c;
}

static PyObject *
cimpl_with_frame(long which, PyObject *self, PyObject *args)
{
    PyMethodDef *ml = frame_builtins[which];
    int flags = ml->ml_flags & ~(METH_CLASS | METH_STATIC | METH_COEXIST);
    Py_ssize_t n = args == NULL ? 0 : PyTuple_GET_SIZE(args);
    linked_t l = {NULL, NULL, NULL};
    bool reads = which == 0 || n < (which >= 3 ? 2 : 1);
    PyObject *result;
    /* locals always reads the frame; vars and dir with no argument, eval
       and exec with no globals */
    if (reads)
        frames_linked(&l, LINK_ALL | LINK_COPY_ROOT);
    switch (flags) {
        case METH_NOARGS:
            result = ml->ml_meth(self, NULL);
            break;
        case METH_VARARGS:
            result = ml->ml_meth(self, args);
            break;
        case METH_FASTCALL:
            result = ((_PyCFunctionFast)(void (*)(void))ml->ml_meth)(
                self, &PyTuple_GET_ITEM(args, 0), n);
            break;
        case METH_FASTCALL | METH_KEYWORDS:
            result =
                ((_PyCFunctionFastWithKeywords)(void (*)(void))ml->ml_meth)(
                    self, &PyTuple_GET_ITEM(args, 0), n, NULL);
            break;
        default:
            PyErr_SetString(PyExc_SystemError,
                            "LeJIT: unexpected builtin form");
            result = NULL;
    }
    if (reads)
        frames_unlinked(&l);
    Py_XDECREF(l.top);
    return result;
}

/* exec can change a mutable local (a virtual list, say) through the
   locals mapping, so each virtual local is made a real object first */
static bool
locals_made_real(PsycoObject *po)
{
    int i, n = n_locals_plus(po->pr.co);
    for (i = 0; i < n; i++) {
        vinfo_t *v = LOC_LOCALS_PLUS[i];
        if (v != NULL && is_virtualtime(v->source) && !compute_vinfo(v, po))
            return false;
    }
    return true;
}

#define FRAME_META(i)                                                       \
    static vinfo_t *meta_with_frame##i(PsycoObject *po, vinfo_t *vself,     \
                                       vinfo_t *vargs)                      \
    {                                                                       \
        if (!po->pr.is_inlining && !locals_made_real(po))                   \
            return NULL;                                                    \
        if (vargs == NULL)                                                  \
            return psyco_generic_call(po, cimpl_with_frame,                 \
                                      CfCommonNewRefPyObject, "lvl",        \
                                      (long)(i), vself, 0L);                \
        return psyco_generic_call(po, cimpl_with_frame,                     \
                                  CfCommonNewRefPyObject, "lvv", (long)(i), \
                                  vself, vargs);                            \
    }
FRAME_META(0)
FRAME_META(1)
FRAME_META(2)
FRAME_META(3)
FRAME_META(4)
#undef FRAME_META
static void *const frame_metas[N_FRAME_BUILTINS] = {
    meta_with_frame0, meta_with_frame1, meta_with_frame2, meta_with_frame3,
    meta_with_frame4};

static void
frame_builtins_init(void)
{
    PyObject *b = PyEval_GetBuiltins();
    int i;
    for (i = 0; i < N_FRAME_BUILTINS; i++) {
        PyObject *fn =
            b == NULL ? NULL : PyDict_GetItemString(b, frame_builtin_names[i]);
        if (fn == NULL || !PyCFunction_Check(fn))
            continue;
        frame_builtins[i] = ((PyCFunctionObject *)fn)->m_ml;
        Psyco_DefineMeta(frame_builtins[i]->ml_meth, frame_metas[i]);
    }
}

/* a replacement of a module's C function takes the original's docstring,
   so its text signature, and is bound to the module as the original is */
static PyObject *
replacement_new(PyMethodDef *def, PyObject *mod, PyObject *original)
{
    PyObject *name;
    PyObject *fn;
    if (original == NULL)
        return NULL;
    if (PyCFunction_Check(original))
        def->ml_doc = ((PyCFunctionObject *)original)->m_ml->ml_doc;
    name = PyModule_GetNameObject(mod);
    if (name == NULL)
        return NULL;
    fn = PyCFunction_NewEx(def, mod, name);
    Py_DECREF(name);
    return fn;
}

static PyMethodDef le_warn_def = {"warn", (PyCFunction)(void (*)(void))le_warn,
                                  METH_VARARGS | METH_KEYWORDS, NULL};

/* replaces warn in _warnings and, if imported, warnings, and warnings'
   _showwarnmsg */
static void
warn_init(void)
{
    PyObject *mod = PyImport_ImportModule("_warnings"), *fn, *w;
    if (mod == NULL) {
        PyErr_Clear(); /* no C warnings: warnings.py walks sys._getframe */
        return;
    }
    original_warn = PyObject_GetAttrString(mod, "warn");
    /* a _warnings imported again later has the same C function, which
       compiled code calls with the frames too (pmethodobject.c) */
    if (original_warn != NULL && PyCFunction_Check(original_warn))
        le_c_warn = PyCFunction_GET_FUNCTION(original_warn);
    fn = replacement_new(&le_warn_def, mod, original_warn);
    if (original_warn == NULL || fn == NULL ||
        PyObject_SetAttrString(mod, "warn", fn) < 0)
        PyErr_Print();
    w = PyDict_GetItemString(PyImport_GetModuleDict(), "warnings");
    if (w != NULL && fn != NULL) {
        PyObject *cur = PyObject_GetAttrString(w, "warn");
        if (cur == original_warn && PyObject_SetAttrString(w, "warn", fn) < 0)
            PyErr_Print();
        Py_XDECREF(cur);
        PyErr_Clear();
    }
    Py_XDECREF(fn);
    Py_DECREF(mod);
    /* C shows its warnings through warnings._showwarnmsg, which it looks
       up in sys.modules['warnings'] each time */
    if (w != NULL && (msg_line = PyUnicode_InternFromString("line")) != NULL &&
        (msg_filename = PyUnicode_InternFromString("filename")) != NULL &&
        (msg_lineno = PyUnicode_InternFromString("lineno")) != NULL &&
        (msg_module = PyUnicode_InternFromString("module")) != NULL &&
        (original_showwarnmsg = PyObject_GetAttrString(w, "_showwarnmsg")) !=
            NULL &&
        (fn = replacement_new(&le_showwarnmsg_def, w, original_showwarnmsg)) !=
            NULL) {
        if (PyObject_SetAttrString(w, "_showwarnmsg", fn) < 0)
            PyErr_Print();
        Py_DECREF(fn);
    }
    PyErr_Clear();
}

/* A running coroutine of a body started by its await (psyco_coros_realise)
   holds the exception its body is handling, which compiled code keeps in
   the thread's: each one in 'args' is lent it while 'on', for
   gc.get_referents to visit, if its body is in a handler */
static void
coros_exc_lend(PyObject *args, int on)
{
    Py_ssize_t i, n = PyTuple_GET_SIZE(args);
    for (i = 0; i < n; i++) {
        PyGenObject *gen = (PyGenObject *)PyTuple_GET_ITEM(args, i);
        _PyErr_StackItem *e = &gen->gi_exc_state;
        PyObject *lasti, *type, *value, *tb;
        PyCodeObject *co;
        size_t k;
        long pos;
        if (!PyCoro_CheckExact(gen) || acts == NULL)
            continue;
        /* running compiled: suspended, ended or gone on in ceval, the
           table no longer holds it */
        for (k = 0; k <= acts_mask; k++)
            if (acts[k].own != NULL && acts[k].coro == (PyObject *)gen)
                break;
        if (k > acts_mask)
            continue;
        if (!on) {
#if LE_HAS_EXC_INFO_VALUE_ONLY
            Py_CLEAR(e->exc_value);
#else
            Py_CLEAR(e->exc_type);
            Py_CLEAR(e->exc_value);
            Py_CLEAR(e->exc_traceback);
#endif
            continue;
        }
        lasti = frame_get_lasti(acts[k].frame, NULL);
        pos = lasti == NULL ? -1 : PyLong_AsLong(lasti);
        Py_XDECREF(lasti);
        PyErr_Clear();
        co = le_pyframe_code(acts[k].frame);
#if LE_HAS_EXCEPTION_TABLE
        if (pos < 0 || psyco_handled_saved(co, (int)pos) < 0)
            continue;
#else
        if (pos < 0 || !psyco_in_handler(co, (int)pos))
            continue;
#endif
        le_exc_info_topmost(PyThreadState_GET(), &type, &value, &tb);
        if (value == NULL || value == Py_None)
            continue;
        Py_INCREF(value);
#if LE_HAS_EXC_INFO_VALUE_ONLY
        e->exc_value = value;
#else
        Py_XINCREF(type);
        Py_XINCREF(tb);
        e->exc_type = type;
        e->exc_value = value;
        e->exc_traceback = tb;
#endif
    }
}

/* A generator or coroutine that compiled code runs has its locals where
   the compiled code keeps them. Where the gc visits a running one's frame
   (not LE_HAS_RUNNING_STACK_HIDDEN), they are lent to its frame for a gc
   call that visits it (le_get_referents, le_get_referrers), and what the
   frame held goes back after: a real frame LeCode_Run runs, and a frame
   compiled code runs a body on, or realised as a coroutine's. */
typedef struct {
    le_frame_t *frame;
    int n;
    PyObject **held; /* NOT_LENT where the word stays the frame's */
} lent_t;
static char not_lent;
#define NOT_LENT ((PyObject *)&not_lent)

#if !LE_HAS_RUNNING_STACK_HIDDEN
static void
lend_locals(lent_t **lent, int *nlent, int *cap, le_frame_t *frame,
            stack_frame_info_t **slot, int own)
{
    PyCodeObject *co = le_frame_code(frame);
    int i, n = le_code_nlocals(co);
    PyObject **values;
    if (n == 0)
        return;
    if (*nlent == *cap) {
        int c = *cap == 0 ? 8 : 2 * *cap;
        lent_t *t = PyMem_Realloc(*lent, c * sizeof(lent_t));
        if (t == NULL)
            return;
        *lent = t;
        *cap = c;
    }
    values = PyMem_Malloc(n_locals_plus(co) * sizeof(PyObject *) + 1);
    if (values == NULL)
        return;
    if (!slot_locals(slot, co, values, own)) {
        PyMem_Free(values);
        return;
    }
    for (i = n; i < n_locals_plus(co); i++) Py_XDECREF(values[i]);
    /* the cells are the frame's own; an unknown local stays as it is */
    for (i = 0; i < n; i++) {
        PyObject *v = values[i];
        if (v == NULL) {
            values[i] = NOT_LENT;
            continue;
        }
        values[i] = le_frame_word_take(frame, i);
        le_frame_word_put(frame, i, v);
    }
    (*lent)[*nlent].frame = frame;
    (*lent)[*nlent].n = n;
    (*lent)[*nlent].held = values;
    (*nlent)++;
}
#endif

static lent_t *
running_locals_lend(int *nlent)
{
    lent_t *lent = NULL;
    *nlent = 0;
#if !LE_HAS_RUNNING_STACK_HIDDEN
    int cap = 0;
    PyThreadState *ts;
    size_t k;
    if (runs_key != NULL)
        for (ts = PyInterpreterState_ThreadHead(PyInterpreterState_Main());
             ts != NULL; ts = PyThreadState_Next(ts)) {
            PyObject *c =
                ts->dict == NULL ? NULL : PyDict_GetItem(ts->dict, runs_key);
            FrameRuntime *fr;
            if (c == NULL)
                continue;
            for (fr = *(FrameRuntime **)PyCapsule_GetPointer(c, NULL);
                 fr != NULL; fr = fr->next)
                if (!fr->resumed && le_frame_is_gen(fr->frame) &&
                    fr->psy_code == le_frame_code(fr->frame))
                    lend_locals(&lent, nlent, &cap, fr->frame,
                                *fr->psy_frames_start, 0);
        }
    if (acts != NULL)
        for (k = 0; k <= acts_mask; k++) {
            activation_t *a = &acts[k];
            if (a->own != NULL && !a->resumed && !a->suspended &&
                (a->gen || a->coro != NULL))
                lend_locals(&lent, nlent, &cap, le_frame_of(a->frame),
                            a->inlined ? a->slot : a->own, a->inlined);
        }
#endif
    return lent;
}

static void
running_locals_return(lent_t *lent, int nlent)
{
    int k, i;
    /* last lent first, should a frame have been lent twice */
    for (k = nlent - 1; k >= 0; k--) {
        for (i = 0; i < lent[k].n; i++) {
            PyObject *v = lent[k].held[i];
            if (v == NOT_LENT)
                continue;
            v = le_frame_word_take(lent[k].frame, i);
            le_frame_word_put(lent[k].frame, i, lent[k].held[i]);
            Py_XDECREF(v);
        }
        PyMem_Free(lent[k].held);
    }
    PyMem_Free(lent);
}

/* gc.get_referents visits a frame's f_localsplus in C, where CPython
   visits them for the frame (a running one's too, if its frame object is
   its frame): a frame given to it gets its locals now first, and is
   marked running for the call if it stands for a call compiled code runs
   (stands_for_running) */
static PyObject *original_get_referents = NULL;

static PyObject *
le_get_referents(PyObject *self, PyObject *args)
{
    Py_ssize_t i, n = PyTuple_GET_SIZE(args);
    PyObject *result;
    lent_t *lent;
    int nlent;
    int *old = PyMem_Malloc(n * sizeof(int) + 1);
    if (old == NULL)
        return PyErr_NoMemory();
    for (i = 0; i < n; i++) {
        PyFrameObject *f = (PyFrameObject *)PyTuple_GET_ITEM(args, i);
        old[i] = INT_MIN; /* not marked */
        if (!PyFrame_Check(f))
            continue;
        view_sync(f, VIEW_LOCALS);
        if (stands_for_running(f))
            old[i] = le_pyframe_mark_running(f);
    }
    lent = running_locals_lend(&nlent);
    coros_exc_lend(args, 1);
    result = PyObject_Call(original_get_referents, args, NULL);
    coros_exc_lend(args, 0);
    running_locals_return(lent, nlent);
    for (i = 0; i < n; i++)
        if (old[i] != INT_MIN)
            le_pyframe_unmark_running(
                (PyFrameObject *)PyTuple_GET_ITEM(args, i), old[i]);
    PyMem_Free(old);
    return result;
}

static PyMethodDef le_get_referents_def = {"get_referents", le_get_referents,
                                           METH_VARARGS, NULL};

/* gc.get_objects and gc.get_referrers find a running coroutine, and a
   function's __name__ and __qualname__ are copied into its coroutine when
   it is made: each realises the coroutines of the bodies started by
   their awaits first (psyco_coros_realise) */
static PyObject *original_get_objects = NULL, *original_get_referrers = NULL;

static PyObject *
le_get_objects(PyObject *self, PyObject *args, PyObject *kwds)
{
    psyco_coros_realise();
    return PyObject_Call(original_get_objects, args, kwds);
}

/* get_referrers leaves out the tuple of arguments it was called with, but
   would find another: each calls the original as it was called */
static PyObject *
le_get_referrers(PyObject *self, PyObject *args)
{
    int nlent;
    lent_t *lent;
    PyObject *result;
    psyco_coros_realise();
    lent = running_locals_lend(&nlent);
    result = PyObject_Call(original_get_referrers, args, NULL);
    running_locals_return(lent, nlent);
    return result;
}

static PyObject *
le_get_referrers_fast(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    int nlent;
    lent_t *lent;
    PyObject *result;
    psyco_coros_realise();
    lent = running_locals_lend(&nlent);
#if LE_HAS_VECTORCALL
    result = PyObject_Vectorcall(original_get_referrers, args, nargs, NULL);
#else
    result =
        _PyObject_FastCall(original_get_referrers, (PyObject **)args, nargs);
#endif
    running_locals_return(lent, nlent);
    return result;
}

static PyMethodDef le_get_objects_def = {
    "get_objects", (PyCFunction)(void (*)(void))le_get_objects,
    METH_VARARGS | METH_KEYWORDS, NULL};
static PyMethodDef le_get_referrers_def = {"get_referrers", le_get_referrers,
                                           METH_VARARGS, NULL};
static PyMethodDef le_get_referrers_fast_def = {
    "get_referrers", (PyCFunction)(void (*)(void))le_get_referrers_fast,
    METH_FASTCALL, NULL};

static PyGetSetDef *func_name_cpython[2];

static int
le_func_set_name(PyObject *op, PyObject *value, void *closure)
{
    psyco_coros_realise();
    return func_name_cpython[0]->set(op, value, closure);
}

static int
le_func_set_qualname(PyObject *op, PyObject *value, void *closure)
{
    psyco_coros_realise();
    return func_name_cpython[1]->set(op, value, closure);
}

static PyGetSetDef func_name_getset[2];

static void
coros_realised_init(PyObject *gcmod)
{
    static const char *names[2] = {"__name__", "__qualname__"};
    static const setter sets[2] = {le_func_set_name, le_func_set_qualname};
    PyObject *fn;
    int i;
    original_get_objects = PyObject_GetAttrString(gcmod, "get_objects");
    fn = original_get_objects == NULL
             ? NULL
             : replacement_new(&le_get_objects_def, gcmod,
                               original_get_objects);
    if (fn == NULL || PyObject_SetAttrString(gcmod, "get_objects", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    original_get_referrers = PyObject_GetAttrString(gcmod, "get_referrers");
    fn = original_get_referrers == NULL
             ? NULL
             : replacement_new(
                   PyCFunction_Check(original_get_referrers) &&
                           (PyCFunction_GET_FLAGS(original_get_referrers) &
                            METH_FASTCALL)
                       ? &le_get_referrers_fast_def
                       : &le_get_referrers_def,
                   gcmod, original_get_referrers);
    if (fn == NULL || PyObject_SetAttrString(gcmod, "get_referrers", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    for (i = 0; i < 2; i++) {
        func_name_getset[i].set = sets[i];
        func_name_cpython[i] =
            getset_replace(&PyFunction_Type, names[i], &func_name_getset[i]);
    }
}

static void
get_referents_init(void)
{
    PyObject *mod = PyImport_ImportModule("gc"), *fn;
    if (mod == NULL) {
        PyErr_Print();
        return;
    }
    original_get_referents = PyObject_GetAttrString(mod, "get_referents");
    fn = replacement_new(&le_get_referents_def, mod, original_get_referents);
    if (fn == NULL || PyObject_SetAttrString(mod, "get_referents", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    coros_realised_init(mod);
    Py_DECREF(mod);
}

#ifdef PyFrameLocalsProxy_Check
/* Every way into PEP 667's frame-locals proxy, its slots and its methods,
   exposes its frame first (expose): the proxy itself reads
   and writes the frame object's locals. Its methods' functions are read
   from their PyMethodDef at each call, so they are replaced there. */
static void
expose_proxy(PyObject *o, int write)
{
    if (PyFrameLocalsProxy_Check(o))
        expose(((PyFrameLocalsProxyObject *)o)->frame, write);
}

static struct {
    reprfunc tp_repr;
    richcmpfunc tp_richcompare;
    getiterfunc tp_iter;
    binaryfunc nb_or, nb_inplace_or;
    objobjproc sq_contains;
    lenfunc mp_length;
    binaryfunc mp_subscript;
    objobjargproc mp_ass_subscript;
} proxy_slots;

static PyObject *
proxy_repr(PyObject *self)
{
    expose_proxy(self, 0);
    return proxy_slots.tp_repr(self);
}
static PyObject *
proxy_richcompare(PyObject *self, PyObject *other, int op)
{
    expose_proxy(self, 0);
    expose_proxy(other, 0);
    return proxy_slots.tp_richcompare(self, other, op);
}
static PyObject *
proxy_iter(PyObject *self)
{
    expose_proxy(self, 0);
    return proxy_slots.tp_iter(self);
}
static PyObject *
proxy_or(PyObject *a, PyObject *b)
{
    expose_proxy(a, 0);
    expose_proxy(b, 0);
    return proxy_slots.nb_or(a, b);
}
static PyObject *
proxy_inplace_or(PyObject *a, PyObject *b)
{
    expose_proxy(a, 1);
    expose_proxy(b, 0);
    return proxy_slots.nb_inplace_or(a, b);
}
static int
proxy_contains(PyObject *self, PyObject *key)
{
    expose_proxy(self, 0);
    return proxy_slots.sq_contains(self, key);
}
static Py_ssize_t
proxy_length(PyObject *self)
{
    expose_proxy(self, 0);
    return proxy_slots.mp_length(self);
}
static PyObject *
proxy_subscript(PyObject *self, PyObject *key)
{
    expose_proxy(self, 0);
    return proxy_slots.mp_subscript(self, key);
}
static int
proxy_ass_subscript(PyObject *self, PyObject *key, PyObject *v)
{
    expose_proxy(self, 1);
    return proxy_slots.mp_ass_subscript(self, key, v);
}

/* the methods: one wrapper per entry, for METH_O, METH_NOARGS and plain
   METH_FASTCALL ones; those that may store are dict's (update, pop,
   setdefault) */
#define PROXY_METHODS 16
static PyCFunction proxy_methods[PROXY_METHODS];
static int proxy_writes[PROXY_METHODS];
#define PROXY_METHOD(i)                                                       \
    static PyObject *proxy_meth_##i(PyObject *self, PyObject *arg)            \
    {                                                                         \
        expose_proxy(self, proxy_writes[i]);                                  \
        return proxy_methods[i](self, arg);                                   \
    }                                                                         \
    static PyObject *proxy_fast_##i(PyObject *self, PyObject *const *args,    \
                                    Py_ssize_t n)                             \
    {                                                                         \
        expose_proxy(self, proxy_writes[i]);                                  \
        return ((_PyCFunctionFast)(void (*)(void))proxy_methods[i])(self,     \
                                                                    args, n); \
    }
/* clang-format off */
PROXY_METHOD(0)  PROXY_METHOD(1)  PROXY_METHOD(2)  PROXY_METHOD(3)
PROXY_METHOD(4)  PROXY_METHOD(5)  PROXY_METHOD(6)  PROXY_METHOD(7)
PROXY_METHOD(8)  PROXY_METHOD(9)  PROXY_METHOD(10) PROXY_METHOD(11)
PROXY_METHOD(12) PROXY_METHOD(13) PROXY_METHOD(14) PROXY_METHOD(15)
#define PROXY_ENTRY(i) \
    {proxy_meth_##i, (PyCFunction)(void (*)(void))proxy_fast_##i}
static const PyCFunction proxy_wrappers[PROXY_METHODS][2] = {
    PROXY_ENTRY(0),  PROXY_ENTRY(1),  PROXY_ENTRY(2),  PROXY_ENTRY(3),
    PROXY_ENTRY(4),  PROXY_ENTRY(5),  PROXY_ENTRY(6),  PROXY_ENTRY(7),
    PROXY_ENTRY(8),  PROXY_ENTRY(9),  PROXY_ENTRY(10), PROXY_ENTRY(11),
    PROXY_ENTRY(12), PROXY_ENTRY(13), PROXY_ENTRY(14), PROXY_ENTRY(15)};
/* clang-format on */

static void
frame_locals_proxy_init(void)
{
    PyTypeObject *t = &PyFrameLocalsProxy_Type;
    PyMethodDef *m;
    int i;
    if (proxy_slots.tp_repr != NULL)
        return; /* done before */
#define WRAP_SLOT(where, field, ours)                    \
    do {                                                 \
        if ((where) != NULL && (where)->field != NULL) { \
            proxy_slots.field = (where)->field;          \
            (where)->field = ours;                       \
        }                                                \
    } while (0)
    psyco_assert(t->tp_repr != NULL);
    WRAP_SLOT(t, tp_repr, proxy_repr);
    WRAP_SLOT(t, tp_richcompare, proxy_richcompare);
    WRAP_SLOT(t, tp_iter, proxy_iter);
    WRAP_SLOT(t->tp_as_number, nb_or, proxy_or);
    WRAP_SLOT(t->tp_as_number, nb_inplace_or, proxy_inplace_or);
    WRAP_SLOT(t->tp_as_sequence, sq_contains, proxy_contains);
    WRAP_SLOT(t->tp_as_mapping, mp_length, proxy_length);
    WRAP_SLOT(t->tp_as_mapping, mp_subscript, proxy_subscript);
    WRAP_SLOT(t->tp_as_mapping, mp_ass_subscript, proxy_ass_subscript);
#undef WRAP_SLOT
    /* the slots' own methods (__setitem__, __len__, ...) call the
       functions they were made with */
    {
        PyObject *key, *v;
        Py_ssize_t pos = 0;
        while (PyDict_Next(le_type_dict(t), &pos, &key, &v)) {
            PyWrapperDescrObject *d = (PyWrapperDescrObject *)v;
            if (Py_TYPE(v) != &PyWrapperDescr_Type)
                continue;
#define WRAPPED(field, ours)                           \
    do {                                               \
        if (proxy_slots.field != NULL &&               \
            d->d_wrapped == (void *)proxy_slots.field) \
            d->d_wrapped = (void *)ours;               \
    } while (0)
            WRAPPED(tp_repr, proxy_repr);
            WRAPPED(tp_richcompare, proxy_richcompare);
            WRAPPED(tp_iter, proxy_iter);
            WRAPPED(nb_or, proxy_or);
            WRAPPED(nb_inplace_or, proxy_inplace_or);
            WRAPPED(sq_contains, proxy_contains);
            WRAPPED(mp_length, proxy_length);
            WRAPPED(mp_subscript, proxy_subscript);
            WRAPPED(mp_ass_subscript, proxy_ass_subscript);
#undef WRAPPED
        }
    }
    for (i = 0, m = t->tp_methods; m != NULL && m->ml_name != NULL; m++) {
        int kind = m->ml_flags & ~METH_COEXIST;
        int fast = kind == METH_FASTCALL;
        if (kind != METH_O && kind != METH_NOARGS && !fast)
            continue;
        psyco_assert(i < PROXY_METHODS);
        proxy_methods[i] = m->ml_meth;
        proxy_writes[i] = strcmp(m->ml_name, "update") == 0 ||
                          strcmp(m->ml_name, "pop") == 0 ||
                          strcmp(m->ml_name, "setdefault") == 0;
        m->ml_meth = proxy_wrappers[i][fast];
        i++;
    }
}
#endif

static PyMethodDef le_getframe_def = {
    "_getframe", (PyCFunction)(void (*)(void))le_getframe,
    METH_FASTCALL | METH_KEYWORDS,
    "_getframe([depth]) -> frameobject\n\n"
    "Return a frame object from the call stack, counting frames of code\n"
    "compiled by LeJIT (emulated)."};

#if LE_HAS_INTERPRETER_FRAME && !LE_HAS_FRAME_INSTR_PTR
/* CPython's _PyGen_yf, behind a generator's or coroutine's close(),
   throw(), gi_yieldfrom and cr_await, takes the word after the frame's
   prev_instr for a RESUME after a yield from even while the frame runs,
   where it is the current call's inline cache: an eval hook makes the call
   deoptimise each time, and its counter can then read as such a RESUME,
   so a stack word is taken as the awaited object. Each of those runs here
   with a running frame's place moved to before its first instruction,
   which is never such a RESUME, and put back after. */
static PyCFunction gen_close_original[2];
static _PyCFunctionFast gen_throw_original[2];
static getter gen_getter_original[2];

/* the running frame of 'gen', moved to before its first instruction; NULL
   if it isn't running */
static _Py_CODEUNIT *
gen_place_hide(PyObject *gen)
{
    PyGenObject *g = (PyGenObject *)gen;
    _PyInterpreterFrame *f = (_PyInterpreterFrame *)g->gi_iframe;
    _Py_CODEUNIT *place;
    if (g->gi_frame_state != FRAME_EXECUTING)
        return NULL;
    place = f->prev_instr;
    f->prev_instr = _PyCode_CODE(LE_FRAME_CODE(f)) - 1;
    return place;
}

static void
gen_place_restore(PyObject *gen, _Py_CODEUNIT *place)
{
    if (place != NULL)
        ((_PyInterpreterFrame *)((PyGenObject *)gen)->gi_iframe)->prev_instr =
            place;
}

#define GEN_GUARDED(name, decl, call)              \
    static PyObject *name decl                     \
    {                                              \
        _Py_CODEUNIT *place = gen_place_hide(gen); \
        PyObject *result = call;                   \
        gen_place_restore(gen, place);             \
        return result;                             \
    }
GEN_GUARDED(gen_close_guarded, (PyObject * gen, PyObject *arg),
            gen_close_original[0](gen, arg))
GEN_GUARDED(coro_close_guarded, (PyObject * gen, PyObject *arg),
            gen_close_original[1](gen, arg))
GEN_GUARDED(gen_throw_guarded,
            (PyObject * gen, PyObject *const *args, Py_ssize_t n),
            gen_throw_original[0](gen, args, n))
GEN_GUARDED(coro_throw_guarded,
            (PyObject * gen, PyObject *const *args, Py_ssize_t n),
            gen_throw_original[1](gen, args, n))
GEN_GUARDED(gen_yf_guarded, (PyObject * gen, void *closure),
            gen_getter_original[0](gen, closure))
GEN_GUARDED(coro_yf_guarded, (PyObject * gen, void *closure),
            gen_getter_original[1](gen, closure))

static PyMethodDef gen_guarded_defs[2][2];
static PyGetSetDef gen_guarded_getsets[2];

/* each replaced by one that guards it */
static void
gen_guard_init(void)
{
    static PyTypeObject *types[2] = {&PyGen_Type, &PyCoro_Type};
    static const char *yf[2] = {"gi_yieldfrom", "cr_await"};
    static const PyCFunction closes[2] = {gen_close_guarded,
                                          coro_close_guarded};
    static const _PyCFunctionFast throws[2] = {gen_throw_guarded,
                                               coro_throw_guarded};
    static const getter yf_wraps[2] = {gen_yf_guarded, coro_yf_guarded};
    PyMethodDef *close, *throw;
    PyGetSetDef *yf_cpython;
    int t;
    for (t = 0; t < 2; t++) {
        close = method_replace(types[t], "close", METH_NOARGS, closes[t],
                               &gen_guarded_defs[t][0]);
        if (close != NULL)
            gen_close_original[t] = close->ml_meth;
        throw = method_replace(types[t], "throw", METH_FASTCALL,
                               (PyCFunction)(void (*)(void))throws[t],
                               &gen_guarded_defs[t][1]);
        if (throw != NULL)
            gen_throw_original[t] =
                (_PyCFunctionFast)(void (*)(void)) throw->ml_meth;
        gen_guarded_getsets[t].get = yf_wraps[t];
        yf_cpython = getset_replace(types[t], yf[t], &gen_guarded_getsets[t]);
        if (yf_cpython != NULL)
            gen_getter_original[t] = yf_cpython->get;
    }
}
#endif

/***************************************************************/
/* CPython's C API functions that read the thread's innermost frame or a
   frame's locals, replaced at their entries (x64/ipatch.c) for every
   caller, extensions and CPython itself: the frame they find is the one
   Python sees (sys._getframe), not the nearest real one, and a frame's
   locals are its compiled frame's now (view_sync). Each then runs
   CPython's own function, with that frame current if need be, as
   le_warn_with_frames does for _warnings.warn. A function the build
   doesn't export, or whose entry can't be replaced, stays CPython's; so
   does a caller that has it inlined (a build with link-time optimisation
   inlines CPython's own uses of them). A frame's line, writes to its
   locals (PyFrame_LocalsToFast) and another thread's frame
   (PyThreadState_GetFrame) are CPython's. */

DEFINEVAR PyFrameObject *(*le_cpython_eval_getframe)(void) = PyEval_GetFrame;
static PyFrameObject *(*threadstate_getframe_cpython)(PyThreadState *);
static PyObject *(*eval_getglobals_cpython)(void);
static PyObject *(*eval_getbuiltins_cpython)(void);
static PyObject *(*eval_getlocals_cpython)(void);
static PyObject *(*eval_getframelocals_cpython)(void);
static PyObject *(*eval_getframeglobals_cpython)(void);
static PyObject *(*eval_getframebuiltins_cpython)(void);
static PyFrameObject *(*frame_getback_cpython)(PyFrameObject *);
static PyObject *(*frame_getlocals_cpython)(PyFrameObject *);
static PyObject *(*frame_getvar_cpython)(PyFrameObject *, PyObject *);
static PyObject *(*frame_getvarstring_cpython)(PyFrameObject *, const char *);
static int (*frame_fasttolocalswitherror_cpython)(PyFrameObject *);
static void (*frame_fasttolocals_cpython)(PyFrameObject *);

/* the frame last given borrowed, if nothing else held it (an emulated
   frame not kept), held until the next is */
static PyFrameObject *capi_held = NULL;

/* tracemalloc's settings, where libpython exports them: an enum, then
   whether it traces. While it traces, it takes a trace's frames with
   PyThreadState_GetFrame and PyFrame_GetBack holding the lock that making
   any object takes too (_PyTraceMalloc_NewReference), so those two are
   CPython's then, which make none. */
static const int *tracemalloc_config = NULL;

static bool
capi_may_make_objects(void)
{
    return tracemalloc_config == NULL || !tracemalloc_config[1];
}

/* 'f' (a reference it takes) stays alive at least until the next call */
static void
capi_hold(PyFrameObject *f)
{
    if (Py_REFCNT(f) == 1)
        Py_XSETREF(capi_held, f);
    else
        Py_DECREF(f);
}

/* compiled code runs the thread's innermost frame, so the innermost frame
   Python sees may be another than the thread's */
static bool
capi_compiled_innermost(PyThreadState *ts)
{
    le_frame_t *f;
    FrameRuntime *fr;
    if (capi_finding || ts == NULL || frame_runs == NULL)
        return false;
    f = le_frame_current(ts);
    while (f != NULL && !le_frame_complete(f)) f = le_frame_back(f);
    if (f == NULL)
        return false;
    fr = psyco_frame_start_get(f);
    return fr != NULL && !fr->resumed;
}

static PyObject *
visit_innermost(const visited_frame_t *v, void *unused)
{
    (void)unused;
    return (PyObject *)emulate_frame(v);
}

/* the innermost frame Python sees, a new reference or NULL, linked by
   f_back to the frames out as sys._getframe's is if 'linked', with its
   locals now if 'locals'. Any exception set stays. */
static PyFrameObject *
capi_innermost(int linked, int locals)
{
    PyObject *t, *v, *tb;
    PyFrameObject *f;
    PyErr_Fetch(&t, &v, &tb);
    capi_finding = 1;
    f = linked ? linked_frame(0, NULL)
               : (PyFrameObject *)pvisitframes(visit_innermost, NULL);
    if (f != NULL)
        view_sync(f, VIEW_PLACE | (locals ? VIEW_LOCALS : 0));
    capi_finding = 0;
    PyErr_Restore(t, v, tb);
    return f;
}

/* brings the frame's locals up to date. Any exception set stays. */
static void
capi_sync(PyFrameObject *f)
{
    PyObject *t, *v, *tb;
    if (capi_finding || f == NULL || frame_runs == NULL)
        return;
    PyErr_Fetch(&t, &v, &tb);
    capi_finding = 1;
    view_sync(f, VIEW_PLACE | VIEW_LOCALS);
    capi_finding = 0;
    PyErr_Restore(t, v, tb);
}

/* CPython's 'fn' run with the innermost frame Python sees current, where
   compiled code runs the thread's innermost frame */
static PyObject *
capi_call_current(PyObject *(*fn)(void), int locals)
{
    linked_t l = {NULL, NULL, NULL};
    bool compiled = capi_compiled_innermost(PyThreadState_GET());
    PyObject *r;
    if (compiled)
        frames_linked(&l, locals ? LINK_LOCALS : 0);
    r = fn();
    if (compiled)
        frames_unlinked(&l);
    if (l.top != NULL)
        capi_hold(l.top); /* what it gave borrowed may be the frame's */
    return r;
}

#if LE_HAS_FRAME_FUNCOBJ
/* types whose tp_new reads the current frame itself, for the module of
   the function that makes the object (typevarobject.c:caller,
   sentinelobject.c:caller), each with CPython's tp_new */
static struct {
    PyTypeObject *type;
    newfunc cpython;
} frame_reading_types[5];
static int n_frame_reading_types = 0;

/* their tp_new, run with the innermost frame Python sees current, linked
   to the frames out for the Python it may call */
static PyObject *
capi_new_with_frame(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    newfunc cpython = NULL;
    linked_t l = {NULL, NULL, NULL};
    bool compiled = capi_compiled_innermost(PyThreadState_GET());
    PyObject *r;
    int i;
    for (i = 0; i < n_frame_reading_types && cpython == NULL; i++)
        if (PyType_IsSubtype(type, frame_reading_types[i].type))
            cpython = frame_reading_types[i].cpython;
    if (cpython == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (compiled)
        frames_linked(&l, LINK_ALL);
    r = cpython(type, args, kwds);
    if (compiled)
        frames_unlinked(&l);
    Py_XDECREF(l.top);
    return r;
}

static void
capi_replace_new(const char *module, const char *name)
{
    PyObject *mod = PyImport_ImportModule(module);
    PyObject *o = mod == NULL ? NULL : PyObject_GetAttrString(mod, name);
    PyTypeObject *tp = (PyTypeObject *)o;
    Py_XDECREF(mod);
    if (o == NULL) {
        PyErr_Clear();
        return;
    }
    if (!PyType_Check(o) || tp->tp_new == NULL) {
        Py_DECREF(o);
        return;
    }
    /* the reference is kept */
    frame_reading_types[n_frame_reading_types].type = tp;
    frame_reading_types[n_frame_reading_types++].cpython = tp->tp_new;
    tp->tp_new = capi_new_with_frame;
    PyType_Modified(tp);
}
#endif

static PyFrameObject *
capi_eval_getframe(void)
{
    PyFrameObject *top;
    if (!capi_compiled_innermost(PyThreadState_GET()) ||
        (top = capi_innermost(1, 0)) == NULL)
        return le_cpython_eval_getframe();
    capi_hold(top);
    return top;
}

static PyFrameObject *
capi_threadstate_getframe(PyThreadState *ts)
{
    PyFrameObject *top;
    if (ts != PyThreadState_GET() || !capi_may_make_objects() ||
        !capi_compiled_innermost(ts) || (top = capi_innermost(1, 0)) == NULL)
        return threadstate_getframe_cpython(ts);
    return top;
}

static PyObject *
capi_eval_getglobals(void)
{
    return capi_call_current(eval_getglobals_cpython, 0);
}

static PyObject *
capi_eval_getbuiltins(void)
{
    return capi_call_current(eval_getbuiltins_cpython, 0);
}

static PyObject *
capi_eval_getlocals(void)
{
    return capi_call_current(eval_getlocals_cpython, 1);
}

static PyObject *
capi_eval_getframelocals(void)
{
    return capi_call_current(eval_getframelocals_cpython, 1);
}

static PyObject *
capi_eval_getframeglobals(void)
{
    return capi_call_current(eval_getframeglobals_cpython, 0);
}

static PyObject *
capi_eval_getframebuiltins(void)
{
    return capi_call_current(eval_getframebuiltins_cpython, 0);
}

/* frame.f_back's (frame_get_back), as a new reference or NULL */
static PyFrameObject *
capi_frame_getback(PyFrameObject *f)
{
    PyObject *t, *v, *tb, *back;
    if (capi_finding || frame_runs == NULL || !capi_may_make_objects())
        return frame_getback_cpython(f);
    PyErr_Fetch(&t, &v, &tb);
    capi_finding = 1;
    back = frame_get_back(f, NULL);
    capi_finding = 0;
    PyErr_Restore(t, v, tb);
    if (back == NULL)
        return frame_getback_cpython(f);
    if (back == Py_None) {
        Py_DECREF(back);
        return NULL;
    }
    return (PyFrameObject *)back;
}

static PyObject *
capi_frame_getlocals(PyFrameObject *f)
{
    capi_sync(f);
    return frame_getlocals_cpython(f);
}

static PyObject *
capi_frame_getvar(PyFrameObject *f, PyObject *name)
{
    capi_sync(f);
    return frame_getvar_cpython(f, name);
}

static PyObject *
capi_frame_getvarstring(PyFrameObject *f, const char *name)
{
    capi_sync(f);
    return frame_getvarstring_cpython(f, name);
}

static int
capi_frame_fasttolocalswitherror(PyFrameObject *f)
{
    capi_sync(f);
    return frame_fasttolocalswitherror_cpython(f);
}

static void
capi_frame_fasttolocals(PyFrameObject *f)
{
    capi_sync(f);
    frame_fasttolocals_cpython(f);
}

static void
capi_replace(const char *name, void *ours, void *cpython)
{
    void *fn = dlsym(RTLD_DEFAULT, name), *original;
    if (fn == NULL)
        return;
    *(void **)cpython = fn;
    if ((original = le_patch_entry(fn, ours)) != NULL)
        *(void **)cpython = original;
}

static void
capi_init(void)
{
    tracemalloc_config = dlsym(RTLD_DEFAULT, "_Py_tracemalloc_config");
#define REPLACE(name, ours, cpython) \
    capi_replace(name, (void *)(ours), (void *)&(cpython))
    REPLACE("PyEval_GetFrame", capi_eval_getframe, le_cpython_eval_getframe);
    REPLACE("PyThreadState_GetFrame", capi_threadstate_getframe,
            threadstate_getframe_cpython);
    REPLACE("PyEval_GetGlobals", capi_eval_getglobals,
            eval_getglobals_cpython);
    REPLACE("PyEval_GetBuiltins", capi_eval_getbuiltins,
            eval_getbuiltins_cpython);
    REPLACE("PyEval_GetLocals", capi_eval_getlocals, eval_getlocals_cpython);
    REPLACE("PyEval_GetFrameLocals", capi_eval_getframelocals,
            eval_getframelocals_cpython);
    REPLACE("PyEval_GetFrameGlobals", capi_eval_getframeglobals,
            eval_getframeglobals_cpython);
    REPLACE("PyEval_GetFrameBuiltins", capi_eval_getframebuiltins,
            eval_getframebuiltins_cpython);
    REPLACE("PyFrame_GetBack", capi_frame_getback, frame_getback_cpython);
    REPLACE("PyFrame_GetLocals", capi_frame_getlocals,
            frame_getlocals_cpython);
    REPLACE("PyFrame_GetVar", capi_frame_getvar, frame_getvar_cpython);
    REPLACE("PyFrame_GetVarString", capi_frame_getvarstring,
            frame_getvarstring_cpython);
    REPLACE("PyFrame_FastToLocalsWithError", capi_frame_fasttolocalswitherror,
            frame_fasttolocalswitherror_cpython);
    REPLACE("PyFrame_FastToLocals", capi_frame_fasttolocals,
            frame_fasttolocals_cpython);
#undef REPLACE
#if LE_HAS_FRAME_FUNCOBJ
    capi_replace_new("_typing", "TypeVar");
    capi_replace_new("_typing", "ParamSpec");
    capi_replace_new("_typing", "TypeVarTuple");
    capi_replace_new("_typing", "TypeAliasType");
    capi_replace_new("builtins", "sentinel");
#endif
}

/* libunwind opens a pipe the first time it unwinds, to test the addresses
   it reads, and keeps it open. Opening it now takes its two file
   descriptors as LeJIT loads, not during a later collection while the
   program has its own files open, where it would shift their numbers. */
static void
unwind_pipe_init(void)
{
    unw_context_t uc;
    unw_cursor_t c;
    if (unw_getcontext(&uc) == 0 && unw_init_local(&c, &uc) == 0)
        unw_step(&c);
}

/* sys._current_frames, replaced too: this thread's frame is the innermost
   one Python sees, linked by f_back as sys._getframe's is. Other threads'
   are CPython's. */
static PyObject *original_current_frames = NULL;

static PyObject *
le_current_frames(PyObject *self, PyObject *unused)
{
    PyObject *d = PyObject_CallObject(original_current_frames, NULL);
    PyObject *id;
    PyFrameObject *f;
    int err;
    if (d == NULL || frame_runs == NULL)
        return d;
    f = linked_frame(0, NULL);
    if (f == NULL) {
        if (!PyErr_Occurred())
            return d;
        Py_DECREF(d);
        return NULL;
    }
    id = PyLong_FromUnsignedLong(PyThreadState_GET()->thread_id);
    err = id == NULL ? -1 : PyDict_SetItem(d, id, (PyObject *)f);
    Py_XDECREF(id);
    Py_DECREF(f);
    if (err < 0)
        Py_CLEAR(d);
    return d;
}

static PyMethodDef le_current_frames_def = {
    "_current_frames", le_current_frames, METH_NOARGS,
    "Return a dict mapping each thread's thread id to its current stack "
    "frame.\n\nThis function should be used for specialized purposes "
    "only."};

static void
current_frames_init(PyObject *sysmod, PyObject *name)
{
    PyObject *fn = PyCFunction_NewEx(&le_current_frames_def, NULL, name);
    original_current_frames =
        PyObject_GetAttrString(sysmod, "_current_frames");
    if (fn == NULL || original_current_frames == NULL ||
        PyObject_SetAttrString(sysmod, "_current_frames", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
}

INITIALIZATIONFN
void
psyco_frames_init(void)
{
    PyObject *sysmod = PyImport_ImportModule("sys");
    /* named as sys's, for the messages that name the function */
    PyObject *name = PyUnicode_FromString("sys");
    PyObject *fn =
        name == NULL ? NULL : PyCFunction_NewEx(&le_getframe_def, NULL, name);
    if (sysmod != NULL)
        original_getframe = PyObject_GetAttrString(sysmod, "_getframe");
    if (sysmod == NULL || fn == NULL || original_getframe == NULL ||
        PyObject_SetAttrString(sysmod, "_getframe", fn) < 0)
        PyErr_Print();
    if (sysmod != NULL && name != NULL)
        current_frames_init(sysmod, name);
#if LE_HAS_FRAME_FUNCOBJ
    if (sysmod != NULL)
        getframemodulename_init(sysmod);
#endif
    Py_XDECREF(fn);
    Py_XDECREF(name);
    Py_XDECREF(sysmod);
    warn_init();
    get_referents_init();
    unwind_pipe_init();
    psyco_coro_suspended =
        PyObject_CallObject((PyObject *)&PyBaseObject_Type, NULL);
    if (psyco_coro_suspended == NULL)
        PyErr_Print();
#if LE_HAS_INTERPRETER_FRAME && !LE_HAS_FRAME_INSTR_PTR
    gen_guard_init();
#endif
#ifdef PyFrameLocalsProxy_Check
    frame_locals_proxy_init();
#endif
    frame_builtins_init();
    getset_replace(&PyFrame_Type, "f_back", &back_getset);
    generator_cpython =
        getset_replace(&PyFrame_Type, "f_generator", &generator_getset);
    lineno_cpython = getset_replace(&PyFrame_Type, "f_lineno", &lineno_getset);
    locals_cpython = getset_replace(&PyFrame_Type, "f_locals", &locals_getset);
    lasti_cpython = getset_replace(&PyFrame_Type, "f_lasti", &lasti_getset);
    clear_cpython =
        method_replace(&PyFrame_Type, "clear", METH_NOARGS,
                       (PyCFunction)(void (*)(void))frame_clear, &clear_def);
    capi_init();
}

/***************************************************************/
/* A compiled frame going on interpreted (pycompiler.c:resume_if_tracing):
   its locals, value stack and blocks are copied into a frame object, the
   real one LeCode_Run runs, its kept frame, the traceback frame its
   catcher word holds, or a new one, and ceval's own loop resumes it after
   f_lasti, as it resumes a generator. The word is emptied: ceval keeps
   the frame's locals from then on, and the compiled frame's, written at
   its end, would be stale. */

struct resume_s {
    PyFrameObject *f;
    /* the frame the catcher word held, if not 'f': given the locals now */
    PyFrameObject *catcher;
    stack_frame_info_t **slot; /* the compiled frame's record, or NULL */
    /* the key of the inlined frame's kept frame 'f' is, let go at its end */
    stack_frame_info_t **inline_key;
    int nvalues;
    /* 'f' keeps its locals, not the compiled frame's: they were exposed
       (expose) */
    int keep_locals;
    /* 'f' is the generator's frame the call runs its body on */
    int gen;
#if !LE_HAS_EXCEPTION_TABLE
    int iblock;
    PyTryBlock blocks[CO_MAXBLOCKS];
#endif
    PyObject *values[1];
};

/* one more call in the run's walk, false if there is no memory for it */
static bool
walked_push(FrameRuntime *fr, stack_frame_info_t **own)
{
    struct walked_calls_s *w = fr->walked == &walked_once ? NULL : fr->walked;
    if (w == NULL || w->n == w->size) {
        bool first = w == NULL;
        size_t size = first ? 16 : 2 * w->size;
        w = PyMem_Realloc(w, sizeof(struct walked_calls_s) +
                                 (size - 1) * sizeof(walked_call_t));
        if (w == NULL)
            return false;
        if (first)
            w->n = 0;
        w->size = size;
        fr->walked = w;
    }
    w->calls[w->n].own = own;
    w->calls[w->n].ret = NULL;
    w->calls[w->n].seen = false;
    w->n++;
    return true;
}

/* the slot of the record whose first entry is the innermost compiled
   frame, NULL when that frame is the real one 'real' is set to (its
   interpreter frame: its frame object is made only for a caller that needs
   it): walked
   on from the innermost call the last walk saw that is still running */
static stack_frame_info_t **
innermost_slot(le_frame_t **real)
{
    le_frame_t *f = le_frame_current(PyThreadState_GET());
    FrameRuntime *fstart = f == NULL ? NULL : psyco_frame_start_get(f);
    stack_frame_info_t **root, **f1, **last = NULL;
    struct walked_calls_s *w;
    size_t from = 0;
    bool kept;
    *real = NULL;
    if (fstart == NULL)
        return NULL;
    *real = fstart->frame;
    root = *(fstart->psy_frames_start);
    w = fstart->walked;
    if (w == NULL) {
        fstart->walked = &walked_once;
        kept = false;
    }
    else if (w != &walked_once && w->calls[0].own == root) {
        from = w->n - 1;
        while (!w->calls[from].seen) from--;
        w->n = from + 1;
        if (from > 0)
            last = w->calls[from - 1].own;
        kept = true;
    }
    else {
        /* the root slot is set as the code starts */
        if (w != NULL)
            walked_forget(w);
        kept = walked_push(fstart, root);
        if (kept)
            fstart->walked->calls[0].seen = true;
    }
    for (f1 = kept ? fstart->walked->calls[from].own : root;
         (((long)(*f1)) & 1) == 0;) {
        last = f1;
        if ((*f1)->link_stack_depth == 0)
            break;
        f1 = psyco_next_stack_frame(f1);
        if (kept && !walked_push(fstart, f1))
            kept = false;
    }
    if (!kept && fstart->walked != &walked_once)
        fstart->walked->n = from + 1;
    else if (kept && fstart->walked->n >= 3)
        walked_watch(fstart->walked, fstart->walked->n - 2);
    return last;
}

DEFINEFN
struct resume_s *
psyco_resume_begin(PyCodeObject *co, PyObject *globals, PyObject *builtins,
                   long nvalues, PyObject **word, long frameless)
{
    le_frame_t *real;
    PyFrameObject *c = word != NULL ? (PyFrameObject *)*word : NULL;
    PyFrameObject *g = NULL;
    activation_t *a;
    struct resume_s *st =
        PyMem_Malloc(sizeof(struct resume_s) + nvalues * sizeof(PyObject *));
    if (st == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    if (c != NULL) {
        /* its caller while it can still be found */
        if (c->f_back == NULL && has_catcher_note(c))
            c->f_back = catcher_caller(c);
        clear_catcher_note(c);
        *word = NULL; /* its reference is st->catcher's */
    }
    st->catcher = c;
    st->keep_locals = 0;
    st->gen = 0;
    st->inline_key = NULL;
    st->slot = innermost_slot(&real);
    if ((a = inline_kept_at(st->slot, real, co)) != NULL) {
        /* the inlined frame, asked for while it ran */
        st->inline_key = a->own;
        a->resumed = 1;
#ifdef PyFrameLocalsProxy_Check
        if (a->exposed) {
            a->exposed = 0;
            psyco_frames_exposed--;
            st->keep_locals = 1;
        }
#endif
        st->f = a->frame;
        Py_INCREF(st->f);
        if (co->co_stacksize >= 2)
            le_pyframe_stack(st->f)[0] = le_pyframe_stack(st->f)[1] = NULL;
        le_frame_set_builtins(le_frame_of(st->f), builtins);
    }
    else if (st->slot != NULL && gen_frame_at(st->slot) != NULL &&
             (*st->slot)->co == co &&
             (g = gen_kept_frame(st->slot, -1)) != NULL) {
        /* the generator's own frame, which ceval goes on with as it
           resumes it: it is linked and has its builtins already. A
           record's is made a generator's first. */
#if LE_RESUMES_GENERATORS_DIRECTLY
        if (le_frame_is_record(le_frame_of(g)) &&
            le_gen_record_escape_running(le_frame_of(g)) == NULL) {
            Py_DECREF(g);
            PyMem_Free(st);
            return NULL;
        }
#endif
        a = act_find(psyco_next_stack_frame(st->slot));
        a->resumed = 1;
#ifdef PyFrameLocalsProxy_Check
        if (a->exposed) {
            a->exposed = 0;
            psyco_frames_exposed--;
            st->keep_locals = 1;
        }
#endif
        st->f = g;
        st->gen = 1;
    }
    else if (st->slot == NULL && real != NULL && le_frame_code(real) == co &&
             (st->f = le_frame_object(real)) != NULL) {
        Py_INCREF(st->f);
#ifdef PyFrameLocalsProxy_Check
        {
            FrameRuntime *fr = psyco_frame_start_get(real);
            if (fr != NULL && fr->exposed) {
                fr->exposed = 0;
                psyco_frames_exposed--;
                st->keep_locals = 1;
            }
        }
#endif
    }
    else if (st->slot != NULL && (a = act_at(st->slot, co)) != NULL &&
             !a->resumed) {
        /* the frame asked for while compiled code ran the call */
        a->resumed = 1;
#ifdef PyFrameLocalsProxy_Check
        if (a->exposed) {
            a->exposed = 0;
            psyco_frames_exposed--;
            st->keep_locals = 1;
        }
#endif
        st->f = a->frame;
        Py_INCREF(st->f);
        if (co->co_stacksize >= 2)
            le_pyframe_stack(st->f)[0] = le_pyframe_stack(st->f)[1] = NULL;
        le_frame_set_builtins(le_frame_of(st->f), builtins);
    }
    else if (c != NULL && !le_frame_is_executing(c) &&
             le_pyframe_code(c) == co) {
        /* the catching callee's traceback frame, as CPython's is its frame */
        st->f = c;
        Py_INCREF(c);
        le_frame_set_builtins(le_frame_of(st->f), builtins);
    }
    else if (frameless)
        st->f = NULL; /* the caller makes the frame it needs */
    else {
        st->f = le_pyframe_new(PyThreadState_GET(), co, globals,
                               st->slot != NULL && (*st->slot)->co == co
                                   ? (*st->slot)->func
                                   : NULL);
        if (st->f == NULL) {
            Py_XDECREF(c);
            PyMem_Free(st);
            return NULL;
        }
        le_frame_set_builtins(le_frame_of(st->f), builtins);
    }
    st->nvalues = 0;
#if !LE_HAS_EXCEPTION_TABLE
    st->iblock = 0;
#endif
    return st;
}

DEFINEFN
void
psyco_resume_value(struct resume_s *st, long i, PyObject *v)
{
    bool object = v != NULL;
#if LE_HAS_TAGGED_LASTI
    /* a handler's lasti, as the frame holds it (pycompiler.c:offset_int) */
    object = object && ((uintptr_t)v & Py_INT_TAG) != Py_INT_TAG;
#endif
#if LE_RESUMES_GENERATORS_DIRECTLY
    /* a record yield from steps: ceval steps its real generator */
    if (object && le_gen_record_check(v)) {
        v = le_gen_record_real(v);
        if (v == NULL)
            OUT_OF_MEMORY();
        object = false;
    }
#endif
    if (object)
        Py_INCREF(v);
    st->values[i] = v;
    st->nvalues = i + 1;
}

#if !LE_HAS_EXCEPTION_TABLE
DEFINEFN
void
psyco_resume_block(struct resume_s *st, long type, long handler, long level)
{
    PyTryBlock *b = &st->blocks[st->iblock++];
    b->b_type = type;
    b->b_handler = le_lasti_of((int)handler);
    b->b_level = level;
}
#endif

/* Stand-ins for the trace and profile functions while the resumed frame
   starts: ceval would give it a 'call' event, which CPython never gives a
   frame already running when tracing starts. Each puts its own function
   back on its first event, before any of the frame's code runs. Where the
   'call' event is RESUME's, a frame resumed past it gets none, and the
   first event may be a callee's 'call': no stand-in then. */
typedef struct {
    Py_tracefunc func;
    PyObject *obj;
} hook_t;

static int
skip_call_trace(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg)
{
    hook_t *h = (hook_t *)obj;
    PyThreadState *ts = PyThreadState_GET();
    ts->c_tracefunc = h->func;
    ts->c_traceobj = h->obj;
    if (what == PyTrace_CALL)
        return 0;
    return h->func(h->obj, frame, what, arg);
}

static int
skip_call_profile(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg)
{
    hook_t *h = (hook_t *)obj;
    PyThreadState *ts = PyThreadState_GET();
    ts->c_profilefunc = h->func;
    ts->c_profileobj = h->obj;
    if (what == PyTrace_CALL)
        return 0;
    return h->func(h->obj, frame, what, arg);
}

/* The compiled frame's locals go into the frame, each old one left in
   st->values for the caller to release once all the new ones are in: one
   may hold the last reference to another's value. A frame that keeps its
   locals (keep_locals) takes only those it has none for, which its
   frame-locals proxy could not read (a virtual value) and cannot have
   deleted. */
static void
resume_locals(struct resume_s *st, le_frame_t *frame, int nlocals)
{
    int i;
    for (i = 0; i < nlocals; i++) {
        PyObject *old;
        if (st->keep_locals && LE_FRAME_WORD(frame, i) != NULL)
            continue;
        old = le_frame_word_take(frame, i);
        le_frame_word_put(frame, i, st->values[i]);
        st->values[i] = old;
    }
}

/* another frame object stood for the call: it keeps the locals as they
   are now in 'frame' */
static void
catcher_write_locals(PyFrameObject *catcher, le_frame_t *frame, int nlocals)
{
#if LE_HAS_FRAME_STACKREFS
    PyObject *locals[nlocals > 0 ? nlocals : 1];
    int i;
    for (i = 0; i < nlocals; i++) locals[i] = LE_FRAME_WORD(frame, i);
#else
    PyObject **locals = le_frame_args(frame);
#endif
    psyco_frame_write_locals(catcher, 0, locals, nlocals);
}

DEFINEFN
PyObject *
psyco_resume_run(struct resume_s *st, long lasti, long nstack)
{
    PyFrameObject *f = st->f, *cur;
    le_frame_t *frame = le_frame_of(f);
    PyThreadState *ts = PyThreadState_GET();
    int nlocals = st->nvalues - nstack;
    int i, marked;
    PyObject *result;
    PyFrameObject *kept_back;
    hook_t trace = {ts->c_tracefunc, ts->c_traceobj};
    hook_t profile = {ts->c_profilefunc, ts->c_profileobj};
    struct hidden_s hidden = {st->slot, hidden_frames};
    FrameRuntime *fr =
        st->slot == NULL ? psyco_frame_start_get(le_frame_of(f)) : NULL;
    extra_assert(nlocals == le_code_nlocalsplus(le_pyframe_code(f)));
    extra_assert(nstack <= le_pyframe_code(f)->co_stacksize);

    resume_locals(st, frame, nlocals);
    for (i = 0; i < nstack; i++) {
        PyObject *v = st->values[nlocals + i];
#if LE_HAS_TAGGED_LASTI
        if (((uintptr_t)v & Py_INT_TAG) == Py_INT_TAG)
            frame->localsplus[nlocals + i].bits = (uintptr_t)v;
        else
#endif
            le_frame_word_put(frame, nlocals + i, v);
    }
    le_frame_set_stack_depth(f, (int)nstack);
    le_frame_set_lasti(le_frame_of(f), le_lasti_of((int)lasti));
#if LE_HAS_FRAME_INSTR_PTR
    /* ceval goes on from the instruction at the place, not after it */
    LE_FRAME_PLACE(le_frame_of(f))++;
#endif
#if !LE_HAS_EXCEPTION_TABLE
    f->f_iblock = st->iblock;
    memcpy(f->f_blockstack, st->blocks, st->iblock * sizeof(PyTryBlock));
#endif
    for (i = 0; i < nlocals; i++) Py_XDECREF(st->values[i]);
    if (st->catcher != NULL) {
        if (st->catcher != f)
            catcher_write_locals(st->catcher, frame, nlocals);
        Py_DECREF(st->catcher);
    }

    if (st->slot != NULL)
        hidden_frames = &hidden;
    if (fr != NULL)
        fr->resumed = 1;
    if (!LE_HAS_OPCODE_RESUME && !ts->tracing) {
        if (trace.func != NULL) {
            ts->c_tracefunc = skip_call_trace;
            ts->c_traceobj = (PyObject *)&trace;
        }
        if (profile.func != NULL) {
            ts->c_profilefunc = skip_call_profile;
            ts->c_profileobj = (PyObject *)&profile;
        }
    }
    /* ceval puts tstate->frame back to f_back: a kept frame's, an emulated
       frame, is given back after; frame_get_back finds it meanwhile */
    kept_back = f->f_back;
    cur = le_frame_current(ts) == NULL ? NULL
                                       : le_frame_object(le_frame_current(ts));
    if (st->slot != NULL && !st->gen && kept_back != cur) {
        Py_XINCREF(cur);
        f->f_back = cur;
    }
    else
        kept_back = NULL;
#if LE_HAS_EVAL_FRAME_POPS
    (void)marked;
    /* the events of a frame that runs past RESUME, if the code gives
       none yet; without them it still runs */
    if (le_code_instrument(le_frame_of(f)) < 0)
        PyErr_Clear();
    result = le_pyframe_run_to_end(ts, f);
    /* the real frame is gone: LeCode_Run returns the result as it is */
    if (fr != NULL) {
        run_unhash(fr);
        fr->frame = NULL;
    }
#else
    marked = le_pyframe_eval_begin(ts, f);
    result = le_eval_frame_default(le_frame_of(f));
    le_pyframe_eval_end(f, marked);
#endif
    if (kept_back != NULL) {
        PyFrameObject *b = f->f_back;
        f->f_back = kept_back;
        Py_XDECREF(b);
    }
    if (fr != NULL)
        fr->resumed = 0;
    /* not put back if the frame never started (RecursionError) */
    if (ts->c_tracefunc == skip_call_trace) {
        ts->c_tracefunc = trace.func;
        ts->c_traceobj = trace.obj;
    }
    if (ts->c_profilefunc == skip_call_profile) {
        ts->c_profilefunc = profile.func;
        ts->c_profileobj = profile.obj;
    }
    if (st->slot != NULL) {
        struct hidden_s **h = &hidden_frames;
        while (*h != &hidden) h = &(*h)->next;
        *h = hidden.next;
    }
    if (st->inline_key != NULL) {
        activation_t *a = act_find(st->inline_key);
        if (a != NULL)
            drop_activation(a);
    }
    Py_DECREF(f);
    PyMem_Free(st);
    return result;
}

/* A coroutine whose body runs as a call from its await
   (psyfunc.c:psyco_coro_start) and suspends: the body returns
   psyco_coro_suspended, and the coroutine made for it and the value it
   yielded wait here for that await (psyco_coro_taken). Only compiled code
   runs between the two. */
DEFINEVAR PyObject *psyco_coro_suspended = NULL;
static PyObject *suspended_coro = NULL, *suspended_value = NULL;

/* A body that goes on in its own handler: compiled code made the exception
   it handles current where its caller's handled exception is ('theirs'),
   keeping that one at stack position 'level', where the outermost handler
   saved it (pycompiler.c:handled_level; -1 if none runs), as ceval keeps
   the coroutine's own. The exception moves to the coroutine's state 'mine',
   and the caller's goes back. */
static void
coro_exc_take(struct resume_s *st, long level, long nstack,
              _PyErr_StackItem *mine, _PyErr_StackItem *theirs)
{
    PyObject **saved;
    if (level < 0)
        return;
    saved = &st->values[st->nvalues - nstack + level];
#if LE_HAS_EXC_INFO_VALUE_ONLY
    Py_XSETREF(mine->exc_value, theirs->exc_value);
    /* none is NULL, as compiled code's POP_EXCEPT leaves it
       (pycompiler.c:cimpl_pop_exc_value) */
    theirs->exc_value = saved[0];
    if (saved[0] == Py_None) {
        theirs->exc_value = NULL;
        Py_DECREF(Py_None);
    }
    saved[0] = Py_NewRef(Py_None);
#else
    int i;
    Py_XDECREF(mine->exc_type);
    Py_XDECREF(mine->exc_value);
    Py_XDECREF(mine->exc_traceback);
    mine->exc_type = theirs->exc_type;
    mine->exc_value = theirs->exc_value;
    mine->exc_traceback = theirs->exc_traceback;
    theirs->exc_traceback = saved[0];
    theirs->exc_value = saved[1];
    theirs->exc_type = saved[2];
    for (i = 0; i < 3; i++) {
        Py_INCREF(Py_None);
        saved[i] = Py_None;
    }
#endif
}

/* the coroutine's state 'to' takes what 'from' holds (coro_exc_take's,
   while the coroutine was being made) */
static void
coro_exc_give(_PyErr_StackItem *to, _PyErr_StackItem *from)
{
#if LE_HAS_EXC_INFO_VALUE_ONLY
    Py_XSETREF(to->exc_value, from->exc_value);
#else
    Py_XSETREF(to->exc_type, from->exc_type);
    Py_XSETREF(to->exc_value, from->exc_value);
    Py_XSETREF(to->exc_traceback, from->exc_traceback);
#endif
}

/* As psyco_resume_run, but the frame, with what psyco_resume_begin and
   psyco_resume_value gave it, is paused at 'at' as ceval leaves it at the
   await's yield, and becomes the coroutine 'func' made, as CPython made it at
   the call: its origin is not recorded (psyco_coro_start), whatever the
   depth is now. 'value' is what it yielded. The marker the body returns,
   or NULL with an exception. */
DEFINEFN PyObject *
psyco_resume_suspend(struct resume_s *st, long at, long nstack, long level,
                     PyFunctionObject *func, PyObject *value)
{
    PyFrameObject *f = st->f;
    le_frame_t *frame;
    int nlocals = st->nvalues - (int)nstack;
    int i;
    PyObject *coro = NULL;
    activation_t *a;
    _PyErr_StackItem held = {0};
#if LE_HAS_INTERPRETER_FRAME
    if (f == NULL) {
        /* nothing stands for the call's frame: the coroutine is made with
           its frame inside it, as RETURN_GENERATOR makes it, and no frame
           object */
        coro = le_gen_make(func);
        if (coro == NULL) {
            for (i = 0; i < st->nvalues; i++) Py_XDECREF(st->values[i]);
            Py_XDECREF(st->catcher);
            PyMem_Free(st);
            return NULL;
        }
        frame =
            (le_frame_t *)((char *)coro + offsetof(PyGenObject, gi_iframe));
        le_frame_init_gen(frame, func, 0, FRAME_OWNED_BY_GENERATOR);
    }
    else
#endif
        frame = le_frame_of(f);
    /* suspending in its own handler */
    coro_exc_take(st, level, nstack, &held, PyThreadState_GET()->exc_info);
    resume_locals(st, frame, nlocals);
    for (i = 0; i < nstack; i++)
        le_frame_word_put(frame, nlocals + i, st->values[nlocals + i]);
    for (i = 0; i < nlocals; i++) Py_XDECREF(st->values[i]);
    if (st->catcher != NULL) {
        if (st->catcher != f)
            catcher_write_locals(st->catcher, frame, nlocals);
        Py_DECREF(st->catcher);
    }
    if (f != NULL) {
        if (st->slot != NULL &&
            (a = act_at(st->slot, le_pyframe_code(f))) != NULL &&
            a->frame == f) {
            a->suspended = 1;
            /* realised already: that coroutine suspends */
            coro = a->coro;
            a->coro = NULL;
        }
        Py_CLEAR(f->f_back);
        /* the words to the stack's top go into the coroutine */
        le_frame_set_stack_depth(f, (int)nstack);
        if (coro != NULL) {
            coro_set_running(coro, 0);
            Py_DECREF(f);
        }
        else if ((coro = le_gen_of_frame(f, func)) == NULL) {
            _PyErr_StackItem none = {0};
            coro_exc_give(&held, &none); /* it goes with the body */
            PyMem_Free(st);
            return NULL;
        }
        /* the coroutine holds the frame now */
        frame = le_frame_of(f);
    }
    coro_exc_give(&((PyGenObject *)coro)->gi_exc_state, &held);
    /* paused once the coroutine exists, which holds the state where the
       frame is inside it */
#if LE_HAS_EXCEPTION_TABLE
    le_gen_frame_pause(frame, le_lasti_of((int)at), (int)nstack);
#else
    le_gen_frame_pause(f, le_lasti_of((int)at), (int)nstack, st->iblock,
                       st->blocks);
#endif
    le_gen_frame_end(frame, value);
    if (f == NULL)
        PyObject_GC_Track(coro); /* made whole */
    PyMem_Free(st);
#if LE_RESUMES_COROUTINES
    if (psyco_trace_yield(f, value) < 0) {
        Py_DECREF(coro);
        return NULL;
    }
#endif
    Py_INCREF(value);
    suspended_coro = coro;
    suspended_value = value;
    Py_INCREF(psyco_coro_suspended);
    return psyco_coro_suspended;
}

/* As psyco_resume_run, for a coroutine's body started by its await
   (GENEXPR_DIRECT): ceval goes on with it as the coroutine 'func' made
   (realised now, if it isn't yet), as gen_send_ex runs it. What the body
   returns: the marker, if it suspended, else the result. */
DEFINEFN PyObject *
psyco_resume_run_coro(struct resume_s *st, long lasti, long nstack, long level,
                      PyFunctionObject *func)
{
    PyFrameObject *f = st->f;
    stack_frame_info_t **slot = st->slot;
    PyThreadState *ts = PyThreadState_GET();
    PyObject *coro = NULL, *result;
    PyGenObject *gen;
    activation_t *a;
    int suspended;
    struct coro_run_s run, **r;
    if (slot != NULL && (a = act_at(slot, le_pyframe_code(f))) != NULL &&
        a->frame == f) {
        coro = a->coro;
        a->coro = NULL;
    }
    if (coro == NULL) {
        Py_INCREF(f);
        coro = le_gen_of_frame(f, func);
        if (coro == NULL) {
            if (st->catcher != NULL)
                Py_DECREF(st->catcher);
            Py_DECREF(f);
            PyMem_Free(st);
            return NULL;
        }
    }
    gen = (PyGenObject *)coro;
    Py_INCREF(f); /* for after the run, which may close the coroutine */
    coro_set_running(coro, 1);
    coro_exc_take(st, level, nstack, &gen->gi_exc_state, ts->exc_info);
    gen->gi_exc_state.previous_item = ts->exc_info;
    ts->exc_info = &gen->gi_exc_state;
    run.slot = slot;
    run.coro = coro;
    run.next = coro_runs;
    coro_runs = &run;
    result = psyco_resume_run(st, lasti, nstack); /* frees st */
    /* other threads' runs may have begun and ended meanwhile */
    for (r = &coro_runs; *r != &run; r = &(*r)->next);
    *r = run.next;
    /* where ceval unlinks a generator's exception state and frame itself
       as it yields or ends, it has */
    if (gen->gi_exc_state.previous_item != NULL) {
        ts->exc_info = gen->gi_exc_state.previous_item;
        gen->gi_exc_state.previous_item = NULL;
    }
#if LE_HAS_INTERPRETER_FRAME
    ((le_frame_t *)((char *)gen + offsetof(PyGenObject, gi_iframe)))
        ->previous = NULL;
#endif
    coro_set_running(coro, 0);
#if LE_HAS_INTERPRETER_FRAME
    suspended = result != NULL && LE_GEN_PAUSED(gen->gi_frame_state);
#elif LE_HAS_FRAME_STATE
    suspended = result != NULL && f->f_state == FRAME_SUSPENDED;
#else
    suspended = result != NULL && f->f_stacktop != NULL;
#endif
    if (slot != NULL && (a = act_at(slot, le_pyframe_code(f))) != NULL &&
        a->frame == f)
        a->suspended = suspended;
    if (!suspended) {
        coro_finish(coro);
        Py_DECREF(f);
        return result;
    }
    Py_CLEAR(f->f_back);
    Py_DECREF(f);
    suspended_coro = coro;
    suspended_value = result;
    Py_INCREF(psyco_coro_suspended);
    return psyco_coro_suspended;
}

/* what the suspended body left, for the await that started it: out[0]
   the coroutine, out[1] the value */
DEFINEFN void
psyco_coro_taken(PyObject *out[])
{
    out[0] = suspended_coro;
    out[1] = suspended_value;
    suspended_coro = suspended_value = NULL;
}
