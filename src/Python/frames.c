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
 * first, as they are then (frame_sync_locals). A traceback's frame gets
 * the locals when the exception leaves the frame (psyco_frame_write_locals);
 * the frame that catches it is held by its catcher word until its call
 * ends and reads them like an emulated frame (CATCHER_NOTE), and gets them
 * before an inlined call and at the end while something else holds it
 * (pycompiler.c:catcher_word); a virtual list becomes real as it becomes
 * one of its locals (catcher_local_readable). Known gaps: virtual lists in
 * other frames, inlined frames' locals, a read from another thread or
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
 * with every frame linked and placed (le_warn), gc.get_referents with its
 * frames' locals written (le_get_referents); other such C code
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
/* after Python.h, for _GNU_SOURCE: ucontext_t's REG_* */
#define UNW_LOCAL_ONLY
#include <dlfcn.h>
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

/* the entry after a record's first for the inlined frame 'caller' runs
   in, which made the call */
static void
finfo_inlined_caller(stack_frame_info_t *p, PsycoObject *caller)
{
    Source sglobals = caller->vlocals.items[INDEX_LOC_GLOBALS]->source;
    p->co = caller->pr.co;
    p->nargs = -1;
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
    p->co = callee->pr.co;
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
    p->co = co;
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

DEFINEFN int
psyco_frames_running(void (*mark)(PyCodeObject *co, void *arg), void *arg)
{
    PyThreadState *ts;
    if (runs_lost)
        return -1;
    if (runs_key == NULL)
        return 0;
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
            mark((PyCodeObject *)fr->psy_code, arg);
            if (fr->resumed)
                continue;
            for (f1 = *fr->psy_frames_start; (((long)*f1) & 1) == 0;
                 f1 = psyco_next_stack_frame(f1)) {
                stack_frame_info_t *fd = *f1, *last = finfo_last(fd);
                for (; fd <= last; fd++) mark(fd->co, arg);
                if ((*f1)->link_stack_depth == 0)
                    break; /* stack top is an inline frame */
            }
        }
    }
    return 0;
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
    fr->next = frame_runs;
    frame_runs = fr;
    if (fr->next == NULL && !runs_listed)
        runs_list();
}

DEFINEFN FrameRuntime *
psyco_frame_start_get(le_frame_t *f)
{
    FrameRuntime *fr;
    for (fr = frame_runs; fr != NULL; fr = fr->next)
        if (fr->frame == f)
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

#if LE_RESUMES_ON_FRAME
/* What Python wrote in the locals the run holds while the body ran
   (psyco_frame_write_locals) is released as the run ends
   (psyco_frame_start_unregister), not here, where it could run code: the
   slots take the compiled code's values after this. The frame's other
   words are its own. */
DEFINEFN void
psyco_gen_pause(le_frame_t *f, const le_gen_pause_t *p)
{
    int *nkeep = NULL;
    PyObject ***kept = p->ntaken > 0 ? run_keep_of(f, &nkeep) : NULL;
    if (kept != NULL && *kept != NULL) {
        int i;
        PyObject **keep =
            PyMem_Realloc(*kept, (*nkeep + p->ntaken) * sizeof(PyObject *));
        for (i = 0; i < p->ntaken; i++) {
            PyObject *o = le_frame_word_take(f, p->taken[i]);
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
    if (p->lasti >= 0)
        le_gen_frame_pause(f, p->lasti, p->nstack);
}

#else
/* What Python wrote in the locals while the body ran
   (psyco_frame_write_locals) is released as the run ends
   (psyco_frame_start_unregister), not here, where it could run code: the
   slots take the compiled code's values after this. */
DEFINEFN le_frame_t *
psyco_gen_pause(FrameRuntime *fr, const le_gen_pause_t *p)
{
    le_frame_t *f = fr->frame;
    if (fr->entry_locals != NULL) {
        int i, n = le_code_nlocals(le_frame_code(f));
        PyObject **keep = PyMem_Realloc(
            fr->entry_locals, (fr->n_entry_locals + n) * sizeof(PyObject *));
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
    le_gen_frame_pause(f, p->lasti, p->nstack, p->iblock, p->blocks);
    return f;
}
#endif

/* The callee-saved registers of the compiled frame whose slot is
   'slot', as it called C, which keeps them or saves them in its frame:
   found by unwinding the C frames from here down to it. LeJIT's code has no
   unwind information, so each run of compiled code nearer here (from the
   real frame LeCode_Run runs, innermost first) is stepped over from
   where its glue saved the registers of the C code that called it
   (iprocessor.c:write_glue_run_code_fn). False if that frame isn't the
   innermost of a run, or the walk doesn't come out where expected. */
#define MAX_RUNS 64
static bool
slot_registers(stack_frame_info_t **slot, long regs[REG_TOTAL],
               code_t **ret_ip)
{
    stack_frame_info_t **roots[MAX_RUNS], **inners[MAX_RUNS], **f1;
    le_frame_t *f = le_frame_current(PyThreadState_GET());
    unw_context_t uc;
    unw_cursor_t c;
    int i, n = 0, steps;
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
        if (inners[n++] == slot)
            break;
    }
    if (n == 0 || inners[n - 1] != slot)
        return false;
    if (unw_getcontext(&uc) < 0 || unw_init_local(&c, &uc) < 0)
        return false;
    for (i = 0; i < n; i++) {
        unw_word_t sp, ip, v;
        long *saved = (long *)roots[i], *ret;
        /* the first frame outside every loaded object is compiled code */
        for (steps = 0;; steps++) {
            Dl_info info;
            if (unw_get_reg(&c, UNW_REG_IP, &ip) < 0)
                return false;
            if (dladdr((void *)ip, &info) == 0)
                break;
            if (steps > 4096 || unw_step(&c) <= 0)
                return false;
        }
        if (unw_get_reg(&c, UNW_X86_64_RSP, &sp) < 0 ||
            sp >= (unw_word_t)inners[i] || (unw_word_t)inners[i] - sp > 65536)
            return false;
        if (i == n - 1) {
            static const int nums[] = {UNW_X86_64_RBX, UNW_X86_64_RBP,
                                       UNW_X86_64_R12, UNW_X86_64_R13,
                                       UNW_X86_64_R14, UNW_X86_64_R15};
            static const int regnums[] = {REG_X64_RBX, REG_X64_RBP,
                                          REG_X64_R12, REG_X64_R13,
                                          REG_X64_R14, REG_X64_R15};
            int k;
            *ret_ip = (code_t *)ip;
            for (k = 0; k < REG_TOTAL; k++) regs[k] = 0;
            for (k = 0; k < 6; k++) {
                if (unw_get_reg(&c, nums[k], &v) < 0)
                    return false;
                regs[regnums[k]] = (long)v;
            }
            return true;
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
            return false;
    }
    return false;
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
   Python, or is inlined. Read while the frame is in that call. */
static bool
slot_locals(stack_frame_info_t **slot, PyCodeObject *co, PyObject **values)
{
    long v = (long)*slot;
    struct locals_map_s *map;
    PyObject *t, *val, *tb;
    long *p, regs[REG_TOTAL], *pregs = NULL;
    char *data;
    int i, n = n_locals_plus(co);
    if (v == -1 || v == FINFO_RETURNING)
        return false;
    if ((v & 1) != 0) {
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
        map = ((stack_frame_info_t *)v)->back_locals;
        if (map == NULL || map->nvalues != n)
            return false;
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

/* f's locals now, from the compiled frame whose slot is 'slot', if it
   says (slot_locals): an unknown cell or free variable is left as it is */
static void
locals_from_slot(PyFrameObject *f, stack_frame_info_t **slot)
{
    int i, n = n_locals_plus(le_pyframe_code(f));
    PyObject **values = PyMem_Malloc(n * sizeof(PyObject *) + 1);
    if (values == NULL)
        return;
    if (slot_locals(slot, le_pyframe_code(f), values)) {
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
   they are asked for (frame_sync_locals), not made with it: most emulated
   frames are walked over for their code and line. Its empty value stack
   holds LOCALS_NOTE and its tag meanwhile. Once the compiled frame no
   longer runs (its code and tag no longer in LeJIT's chain), the frame
   keeps what was read last. */
static char locals_note;
#define LOCALS_NOTE ((PyObject *)&locals_note)

/* one frame pvisitframes visits: a real frame of its own ('code' NULL), a
   compiled frame ('frame' NULL: its code, globals, tag and the place it is
   at), or the real frame whose code LeCode_Run runs (its code, globals,
   frame and place); all borrowed */
typedef struct {
    PyObject *code, *globals;
    PyFrameObject *frame;
    long tag;
    int lasti;
} visited_frame_t;

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

static void
fill_noted_locals(PyFrameObject *f)
{
    PyObject *found;
    long tag;
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
    locals_from_slot(f, psyco_next_stack_frame((stack_frame_info_t **)tag));
}

PSY_INLINE PyFrameObject *
psyco_build_pyframe(PyObject *co, PyObject *globals, PyThreadState *tstate)
{
    le_frame_t *back;
    PyFrameObject *result;

    /* frame objects are not created in stack order
       with Psyco, so it's probably better not to
       create plain wrong chained lists */
    back = le_frame_current(tstate);
    le_frame_set_current(tstate, NULL);
    result = le_pyframe_new(tstate, (PyCodeObject *)co, globals);
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
   when asked for (frame_sync_locals, kept_lasti). Keyed by the callee's own
   slot, a stack address, unique among the calls running in every thread.
   An inlined frame has no call of its own whose end could be seen, so its
   frame objects are still new each time. */
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
       entry_locals are (psyco_frame_write_locals, psyco_gen_pause) */
    int gen;
    PyObject **keep;
    int nkeep;
} activation_t;

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
    long v = (long)*a->own;
    if (v == -1 || v == FINFO_RETURNING)
        return -1;
    if ((v & 1) != 0)
        return psyco_note_map(v)->lasti;
    return finfo_last((stack_frame_info_t *)v)->back_lasti;
}

/* Where the return address of the compiled callee whose slot is 'own' is:
   after the arguments the call pushed, unless a block the callee went on
   in keeps it elsewhere (a unification may move it, as any stack value).
   Then it is looked for between the slot and the callee's own callee, or
   the next run of compiled code nearer here (or here): NULL if it isn't
   there once, so that nothing but it is replaced. */
#define RET_SEARCH_MAX 8192
static code_t **
return_address(stack_frame_info_t **own, stack_frame_info_t *rec)
{
    code_t **p = (code_t **)(own - rec->nargs - 1), **lo, **q, **found = NULL;
    le_frame_t *t;
    if (*p == rec->ret_addr)
        return p;
    if ((((long)*own) & 1) == 0) {
        lo = (code_t **)psyco_next_stack_frame(own) + 1;
    }
    else {
        lo = (code_t **)&p;
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
    }
    if ((code_t **)own - lo > RET_SEARCH_MAX)
        lo = (code_t **)own - RET_SEARCH_MAX;
    for (q = (code_t **)own - 1; q >= lo; q--)
        if (*q == rec->ret_addr) {
            if (found != NULL)
                return NULL;
            found = q;
        }
    return found;
}

#if LE_RESUMES_GENERATORS_DIRECTLY
/* the generator's frame the call whose record is in 'slot' runs on, if
   the record says it runs on one (on_gen_frame): the word above the
   callee's own slot, which its caller pushed (psyco_call_psyco_gen) */
static le_frame_t *
gen_frame_at(stack_frame_info_t **slot)
{
    stack_frame_info_t *rec = *slot;
    if ((((long)rec) & 1) != 0 || !rec->on_gen_frame)
        return NULL;
    return *(le_frame_t **)(psyco_next_stack_frame(slot) + 1);
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
    a->ret = *ret;
    a->frame = f; /* the table's reference */
    a->resumed = 0;
    a->exposed = 0;
    a->suspended = 0;
    a->coro = NULL;
    a->gen = 1;
    a->keep = NULL;
    a->nkeep = 0;
    *ret = psyco_return_barrier;
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

static PyFrameObject *
kept_frame(long tag, PyObject *co, PyObject *globals, int lasti)
{
    stack_frame_info_t **slot = (stack_frame_info_t **)tag, **own;
    stack_frame_info_t *rec = *slot;
    activation_t *a;
    code_t **ret;
    PyFrameObject *f;
    if (rec->nargs < 0 || rec->co != (PyCodeObject *)co)
        return NULL; /* an inlined frame */
    if (gen_frame_at(slot) != NULL)
        return gen_kept_frame(slot, lasti);
    own = psyco_next_stack_frame(slot);
    a = act_find(own);
    if (a != NULL) {
        if (a->slot != slot || le_pyframe_code(a->frame) != (PyCodeObject *)co)
            return NULL;
        f = a->frame;
        Py_INCREF(f);
        if (!a->resumed)
            le_frame_set_lasti(le_frame_of(f), le_lasti_of(lasti));
        return f;
    }
    ret = return_address(own, rec);
    if (ret == NULL)
        return NULL;
    f = psyco_build_pyframe(co, globals, PyThreadState_GET());
    le_frame_set_lasti(le_frame_of(f), le_lasti_of(lasti));
    /* running, as CPython's: frame.clear() refuses it */
    le_pyframe_set_executing(f, 1);
    if (((PyCodeObject *)co)->co_stacksize >= 2) {
        le_pyframe_stack(f)[0] = ACTIVATION_NOTE;
        le_pyframe_stack(f)[1] = (PyObject *)own;
    }
    a = act_add(own);
    a->slot = slot;
    a->ret = *ret;
    a->frame = f; /* the table's reference */
    a->resumed = 0;
    a->exposed = 0;
    a->suspended = 0;
    a->coro = NULL;
    a->gen = 0;
    a->keep = NULL;
    a->nkeep = 0;
    *ret = psyco_return_barrier;
    Py_INCREF(f);
    return f;
}

#if LE_RESUMES_COROUTINES
/* a realised coroutine runs (psyco_coros_realise) or not, as gen_send_ex
   marks it: on a frame with no state its own flag says so, and its frame
   has no stack top while it runs */
static void
coro_set_running(PyObject *coro, int on)
{
#if LE_HAS_FRAME_STATE
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
    PyFrameObject *f = gen->gi_frame;
    coro_set_running(coro, 0);
    if (f != NULL) {
        f->f_gen = NULL;
        gen->gi_frame = NULL;
        Py_DECREF(f);
    }
    Py_DECREF(coro);
}

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
    coro = PyCoro_New(f, func->func_name, func->func_qualname); /* takes f */
    if (coro == NULL) {
        PyErr_Clear();
        return NULL;
    }
    /* made where the origin depth was 0 (psyco_coro_start) */
    Py_CLEAR(((PyCoroObject *)coro)->cr_origin);
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
    PyThreadState *ts;
    PyObject *t, *v, *tb;
    if (runs_key == NULL)
        return;
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
}
#endif

DEFINEFN
code_t *
psyco_activation_returned(long *sp)
{
    activation_t *a = act_find((stack_frame_info_t **)(sp + 1));
    PyFrameObject *f;
    PyObject *t, *v, *tb, **keep;
    int gen, nkeep;
#if LE_RESUMES_COROUTINES
    PyObject *coro;
#endif
    code_t *ret;
    if (a == NULL)
        psyco_fatal_msg("a kept frame's call returned unknown");
    f = a->frame;
    ret = a->ret;
    gen = a->gen;
    keep = a->keep;
    nkeep = a->nkeep;
    /* the call is over, as its caller is about to say: the frame's
       finaliser may walk the frames */
    *a->slot = (stack_frame_info_t *)-1;
#ifdef PyFrameLocalsProxy_Check
    if (a->exposed)
        psyco_frames_exposed--;
#endif
    if (!a->suspended && !gen)
        le_pyframe_set_executing(f, 0);
#if LE_RESUMES_COROUTINES
    coro = a->coro;
#endif
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
#if LE_RESUMES_COROUTINES
    if (coro != NULL)
        coro_finish(coro);
#endif
    Py_DECREF(f);
    PyErr_Restore(t, v, tb);
    return ret;
}

static stack_frame_info_t **
innermost_slot(PyFrameObject **real);

DEFINEFN
PyFrameObject *
psyco_innermost_activation(PyCodeObject *co)
{
    PyFrameObject *real, *g;
    stack_frame_info_t **slot;
    activation_t *a;
    if ((slot = innermost_slot(&real)) == NULL)
        return NULL;
    /* a generator's frame stands for its body's call from the start */
    if (gen_frame_at(slot) != NULL && (*slot)->co == co &&
        (g = gen_kept_frame(slot, -1)) != NULL)
        Py_DECREF(g); /* the table holds it */
    if (acts_count == 0 || (a = act_at(slot, co)) == NULL || a->resumed)
        return NULL;
    return a->frame;
}

/* A frame object for a frame pvisitframes visits: a real frame, or a new
   emulated frame for a compiled one. New reference. */
static PyFrameObject *
emulate_frame(const visited_frame_t *v)
{
    if (v->frame != NULL) {
        /* a real Python frame, or the one whose code LeCode_Run runs */
        Py_INCREF(v->frame);
        return v->frame;
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
        /* the tag is the slot holding the frame's record, or less one per
           inlined frame before it */
        if ((tag & (sizeof(long) - 1)) == 0 &&
            (result = kept_frame(tag, co, globals, lasti)) != NULL)
            return result;
        result = psyco_build_pyframe(co, globals, PyThreadState_GET());
        le_frame_set_lasti(le_frame_of(result), le_lasti_of(lasti));
        if ((tag & (sizeof(long) - 1)) == 0) {
            if (((PyCodeObject *)co)->co_stacksize >= 2) {
                le_pyframe_stack(result)[0] = LOCALS_NOTE;
                le_pyframe_stack(result)[1] = (PyObject *)tag;
            }
            else {
                if (slot_locals(
                        psyco_next_stack_frame((stack_frame_info_t **)tag),
                        (PyCodeObject *)co,
                        le_frame_args(le_frame_of(result))))
                    le_frame_words_own(le_frame_of(result),
                                       n_locals_plus((PyCodeObject *)co));
            }
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
    le_frame_t *f = le_frame_current(PyThreadState_Get());
    PyFrameObject *fo = NULL;
    bool linked = false; /* the previous frame was emulated, so the compiled
                            frames up to this one are already in the chain
                            (linked_frame) */
    le_recursion_suspend();
    while (f != NULL && !le_frame_complete(f)) f = le_frame_back(f);
    if (f != NULL && (fo = le_frame_object(f)) == NULL)
        OUT_OF_MEMORY();
    /* each frame's caller is as frame.f_back gives it: an emulated frame
       made current (le_warn) is linked to the next only by f_back */
    while (fo != NULL) {
        /* is this Python frame the starting point of a chained
           list of Psyco frames ? */
        fstart = linked ? NULL : psyco_frame_start_get(le_frame_of(fo));
        linked = !le_frame_is_executing(fo) || live_activation(fo) != NULL;
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
                            g = le_frame_globals(gf != NULL ? gf
                                                            : le_frame_of(fo));
                        v.code = (PyObject *)fdata->co;
                        v.globals = g;
                        v.frame = NULL;
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
                fo = le_frame_object_back(fo);
                continue;
            }
            v.code = (PyObject *)fstart->psy_code;
            v.globals = fstart->psy_globals;
            v.frame = fo;
            v.tag = 0;
            v.lasti = lasti;
            result = callback(&v, data);
        }
        else {
            /* a real unshadowed Python frame */
            visited_frame_t real = {NULL, NULL, fo, 0, -1};
            result = callback(&real, data);
        }
        if (result != NULL)
            break;
        fo = le_frame_object_back(fo);
    }
    le_recursion_resume();
    return result;
}

static PyObject *
visit_get_globals(const visited_frame_t *v, void *ignored)
{
    if (v->code == NULL)
        return le_frame_globals(le_frame_of(v->frame));
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

/* frame.f_back, replaced in PyFrame_Type's dict. A running real frame
   entered through C from compiled code has as f_back the real frame
   LeCode_Run runs, which skips any compiled frames in
   between: they are found where the frame is in LeJIT's chain and given,
   emulated and linked to the real one. The stored f_back stays, since
   the evaluators put tstate->frame back to it. */
struct back_s {
    PyFrameObject *frame, *back, *top, *prev;
    bool found;
    long tag; /* with 'frame' NULL: the compiled frame to find, by its tag */
    PyCodeObject *co;
};

static PyObject *
visit_back_frame(const visited_frame_t *v, void *data)
{
    struct back_s *b = (struct back_s *)data;
    PyFrameObject *f;
    if (!b->found) {
        if (b->frame == NULL)
            b->found = v->frame == NULL && v->tag == b->tag &&
                       v->code == (PyObject *)b->co;
        else
            b->found = v->frame != NULL
                           ? v->frame == b->frame
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
       asked, and keeps none */
    if (visited_gen_frame(v) != NULL)
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
   (frame_sync_locals) and caller (frame_get_back) then, so that it shows
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
clear_catcher_note(PyFrameObject *f)
{
    if (has_catcher_note(f))
        le_pyframe_stack(f)[0] = le_pyframe_stack(f)[1] =
            le_pyframe_stack(f)[2] = NULL;
}

/* the catching callee's own slot, if it runs on this thread and its word
   still holds 'f', with the slot of its record and its run */
static stack_frame_info_t **
catcher_slot(PyFrameObject *f, stack_frame_info_t ***tag, FrameRuntime **run)
{
    stack_frame_info_t **own = (stack_frame_info_t **)le_pyframe_stack(f)[1],
                       **f1;
    PyObject **word = (PyObject **)le_pyframe_stack(f)[2];
    FrameRuntime *fr;
    for (fr = frame_runs; fr != NULL; fr = fr->next) {
        if (fr->resumed || !le_frame_executing(fr->frame))
            continue;
        for (f1 = *fr->psy_frames_start;
             (((long)*f1) & 1) == 0 && (*f1)->link_stack_depth != 0;
             f1 = psyco_next_stack_frame(f1))
            if (psyco_next_stack_frame(f1) == own) {
                /* the word is in the callee's stack, which is live */
                if (*word != (PyObject *)f || is_hidden(f1))
                    return NULL;
                *tag = f1;
                *run = fr;
                return own;
            }
    }
    return NULL;
}

/* the catching callee's caller, emulated and linked, or NULL */
static PyFrameObject *
catcher_caller(PyFrameObject *f)
{
    stack_frame_info_t **tag;
    FrameRuntime *fr;
    struct back_s b = {NULL, NULL, NULL, NULL, false, 0, le_pyframe_code(f)};
    if (catcher_slot(f, &tag, &fr) == NULL)
        return NULL;
    b.tag = (long)tag;
    b.back = le_frame_object(fr->frame);
    pvisitframes(visit_back_frame, &b);
    PyErr_Clear();
    if (b.top == NULL && b.found) {
        Py_INCREF(b.back);
        return b.back;
    }
    return b.top;
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

/* A frame's locals as they are now, for each way Python reads them
   (frame.f_locals; locals and vars by another name, cimpl_with_frame;
   gc.get_referents, le_get_referents): a real frame compiled code runs, a
   kept frame, an emulated frame and a catching callee's traceback frame
   read theirs from their compiled frame while it is in a call that says
   where they are (slot_locals). Else they stay as last written. An
   exposed frame's are its frame object's already (expose). */
static void
frame_sync_locals(PyFrameObject *f)
{
    activation_t *a;
    stack_frame_info_t **own, **tag;
    FrameRuntime *fr;
#ifdef PyFrameLocalsProxy_Check
    int *exposed = exposed_flag(f);
    if (exposed != NULL && *exposed)
        return;
#endif
    if (runs_compiled(f)) {
        fr = psyco_frame_start_get(le_frame_of(f));
        if (fr->psy_code == le_pyframe_code(f))
            locals_from_slot(f, *fr->psy_frames_start);
    }
    else if ((a = live_activation(f)) != NULL) {
        locals_from_slot(f, a->own);
    }
    else if (has_catcher_note(f)) {
        if ((own = catcher_slot(f, &tag, &fr)) != NULL)
            locals_from_slot(f, own);
    }
    else {
        fill_noted_locals(f);
    }
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
    frame_sync_locals(f);
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
    PyFrameObject *real;
    stack_frame_info_t **slot = innermost_slot(&real);
    activation_t *a;
    FrameRuntime *fr;
    if (slot == NULL && real != NULL && le_pyframe_code(real) == co) {
        fr = psyco_frame_start_get(le_frame_of(real));
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

static PyObject *
frame_get_back(PyFrameObject *frame, void *closure)
{
    PyFrameObject *back = le_frame_object_back(frame);
    activation_t *a;
    if (back == NULL && has_catcher_note(frame))
        back = frame->f_back = catcher_caller(frame); /* takes the reference */
    if (back == NULL)
        Py_RETURN_NONE;
    /* a generator's frame its body runs on as a direct call is linked
       only to the frame the call was made from */
    if (le_frame_is_executing(frame) && runs_compiled(back) &&
        ((a = live_activation(frame)) == NULL || a->gen)) {
        struct back_s b = {frame, back, NULL, NULL, false, 0, NULL};
        pvisitframes(visit_back_frame, &b);
        PyErr_Clear();
        if (b.top != NULL)
            return (PyObject *)b.top;
    }
    Py_INCREF(back);
    return (PyObject *)back;
}

static PyGetSetDef back_getset = {"f_back", (getter)frame_get_back, NULL, NULL,
                                  NULL};

/* frame.f_lineno, replaced too: a frame LeCode_Run runs doesn't keep its
   f_lasti, so its place comes from LeJIT's chain, which visits it with
   it. The setter (a jump, under tracing) is ceval's. */
static PyObject *lineno_descr = NULL;

/* the place of the frame LeCode_Run runs, 'frame' in it, or -1 */
struct root_lasti_s {
    PyFrameObject *frame;
    int lasti;
};

static PyObject *
visit_root_lasti(const visited_frame_t *v, void *data)
{
    struct root_lasti_s *r = (struct root_lasti_s *)data;
    if (v->code != NULL && v->frame == r->frame) {
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

/* where compiled code running the frame is now (bytes), or -1: a kept
   frame is placed there too */
static int
compiled_place(PyFrameObject *frame)
{
    activation_t *a;
    int lasti = -1;
    if (frame->f_trace != NULL)
        return -1;
    if ((a = live_activation(frame)) != NULL) {
        lasti = kept_lasti(a);
        if (lasti >= 0)
            le_frame_set_lasti(le_frame_of(frame), le_lasti_of(lasti));
    }
    else if (runs_compiled(frame))
        lasti = root_lasti(frame);
    return lasti;
}

static PyObject *
frame_get_lineno(PyFrameObject *frame, void *closure)
{
    int lasti = compiled_place(frame);
    if (lasti >= 0 && runs_compiled(frame))
        return line_object(PyCode_Addr2Line(le_pyframe_code(frame), lasti));
    return line_object(PyFrame_GetLineNumber(frame));
}

static int
frame_set_lineno(PyFrameObject *frame, PyObject *v, void *c)
{
    return Py_TYPE(lineno_descr)
        ->tp_descr_set(lineno_descr, (PyObject *)frame, v);
}

static PyGetSetDef lineno_getset = {"f_lineno", (getter)frame_get_lineno,
                                    (setter)frame_set_lineno, NULL, NULL};

/* frame.f_lasti, replaced too, from the same place as f_lineno */
static PyObject *lasti_descr = NULL;

static PyObject *
frame_get_lasti(PyFrameObject *frame, void *closure)
{
    int lasti = compiled_place(frame);
    if (lasti >= 0 && runs_compiled(frame))
        return PyLong_FromLong(lasti);
    return Py_TYPE(lasti_descr)
        ->tp_descr_get(lasti_descr, (PyObject *)frame,
                       (PyObject *)&PyFrame_Type);
}

static PyGetSetDef lasti_getset = {"f_lasti", (getter)frame_get_lasti, NULL,
                                   NULL, NULL};

/* frame.f_locals, replaced in PyFrame_Type's dict: PyFrame_FastToLocals
   reads f_localsplus, which frame_sync_locals brings up to date */
static PyObject *locals_descr = NULL;

static PyObject *
frame_get_locals(PyFrameObject *frame, void *closure)
{
    frame_sync_locals(frame);
    return Py_TYPE(locals_descr)
        ->tp_descr_get(locals_descr, (PyObject *)frame,
                       (PyObject *)&PyFrame_Type);
}

static PyGetSetDef locals_getset = {"f_locals", (getter)frame_get_locals, NULL,
                                    NULL, NULL};

/* frame.clear(), replaced too: a frame object that stands for a call
   compiled code still runs, without being running itself (the catching
   callee's traceback frame, an emulated frame), is refused as CPython
   refuses its running frames, by CPython's method with the frame marked
   running for the call */
static PyObject *clear_descr = NULL;

static bool
stands_for_running(PyFrameObject *f)
{
    stack_frame_info_t **tag;
    FrameRuntime *fr;
    PyObject *found;
    if (le_frame_is_executing(f))
        return false; /* CPython's method refuses it itself */
    if (has_catcher_note(f))
        return catcher_slot(f, &tag, &fr) != NULL;
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
    int old = 0, mark = stands_for_running(frame);
    if (mark)
        old = le_pyframe_mark_running(frame);
    result = PyObject_CallFunctionObjArgs(clear_descr, frame, NULL);
    if (mark)
        le_pyframe_unmark_running(frame, old);
    return result;
}

static PyMethodDef clear_def = {
    "clear", (PyCFunction)(void (*)(void))frame_clear, METH_NOARGS,
    "F.clear(): clear most references held by the frame"};

/* A traceback entry for a compiled frame, 'f', is being added over
   'callee''s, the frame the exception came out of: that frame, if it has
   returned, gets 'f' as f_back. It is an emulated frame, whose f_back is
   none or another emulated frame of the same code made when its entry
   was, or a real frame entered through C whose f_back is the frame
   LeCode_Run runs, while the code that one runs is in a compiled callee
   or inlined frame, which 'f' is. A fused generator's frame is left
   without one, as a generator's frame has no caller once it has stopped. */
DEFINEFN
void
psyco_frame_link_callee(PyFrameObject *callee, PyFrameObject *f)
{
    PyFrameObject *back = callee->f_back;
    FrameRuntime *fr;
    if (callee == f || back == f || le_frame_is_executing(callee) ||
        (le_pyframe_code(callee)->co_flags & CO_GENERATOR))
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

DEFINEFN
void
psyco_frame_returned(le_frame_t *f)
{
    PyFrameObject *top;
    if (!psyco_frame_called_from_callee(le_frame_back(f)))
        return;
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
        PyFrameObject *real;
        stack_frame_info_t **last = innermost_slot(&real);
        extra_assert(last != NULL && psyco_next_stack_frame(last) == own);
#endif
        le_pyframe_stack(f)[0] = CATCHER_NOTE;
        le_pyframe_stack(f)[1] = (PyObject *)own;
        le_pyframe_stack(f)[2] = (PyObject *)word;
    }
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
void
psyco_frame_catcher_drop(PyObject **word)
{
    PyFrameObject *f = (PyFrameObject *)*word;
    if (f != NULL && Py_REFCNT(f) <= catcher_holders(f))
        catcher_release(word);
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

/* _warnings.warn walks tstate->frame in C: it runs with the compiled
   frames over the current real frame emulated and current. */
static PyObject *original_warn = NULL;
DEFINEVAR PyCFunction le_c_warn = NULL;

DEFINEFN
PyObject *
le_warn_with_frames(PyObject *warn, PyObject *args, PyObject *kwds)
{
    PyThreadState *ts = PyThreadState_GET();
    le_frame_t *saved = le_frame_current(ts);
    PyFrameObject *top;
    PyObject *result, *undo = PyList_New(0);
    if (undo == NULL)
        return NULL;
    /* every frame is linked for the call, and put back after it */
    top = linked_frame(0, undo);
    if (top != NULL && le_frame_of(top) != saved)
        le_pyframe_make_current(ts, top);
    result = PyObject_Call(warn, args, kwds);
    le_frame_set_current(ts, saved);
    if (top != NULL && le_frame_of(top) != saved)
        le_pyframe_current_end(top);
    restore_links(undo);
    Py_DECREF(undo);
    Py_XDECREF(top);
    return result;
}

static PyObject *
le_warn(PyObject *self, PyObject *args, PyObject *kwds)
{
    (void)self;
    return le_warn_with_frames(original_warn, args, kwds);
}

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
    c = psyco_build_pyframe((PyObject *)le_pyframe_code(f),
                            le_frame_globals(le_frame_of(f)),
                            PyThreadState_GET());
    if (!slot_locals(*fr->psy_frames_start, le_pyframe_code(f),
                     le_frame_args(le_frame_of(c)))) {
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
    PyThreadState *ts = PyThreadState_GET();
    le_frame_t *saved = le_frame_current(ts);
    PyFrameObject *top = NULL;
    PyObject *result, *undo = NULL;
    /* locals always reads the frame; vars and dir with no argument, eval
       and exec with no globals */
    if (which == 0 || n < (which >= 3 ? 2 : 1)) {
        undo = PyList_New(0);
        if (undo == NULL)
            return NULL;
        top = linked_frame(0, undo);
        if (top != NULL && runs_compiled(top))
            top = root_copy(top);
        else if (top != NULL)
            frame_sync_locals(top);
        if (top != NULL && le_frame_of(top) != saved)
            le_pyframe_make_current(ts, top);
    }
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
    le_frame_set_current(ts, saved);
    if (top != NULL && le_frame_of(top) != saved)
        le_pyframe_current_end(top);
    if (undo != NULL) {
        restore_links(undo);
        Py_DECREF(undo);
    }
    Py_XDECREF(top);
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

/* replaces warn in _warnings and, if imported, warnings */
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
}

#if LE_RESUMES_COROUTINES
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
        _PyErr_StackItem *e = &gen->gi_exc_state, *top;
        size_t k;
        PyObject *lasti;
        long pos;
        if (!PyCoro_CheckExact(gen) || gen->gi_frame == NULL ||
            !le_frame_is_executing(gen->gi_frame))
            continue;
        for (k = 0; k <= acts_mask && acts != NULL; k++)
            if (acts[k].own != NULL && acts[k].coro == (PyObject *)gen)
                break;
        if (acts == NULL || k > acts_mask)
            continue;
        if (!on) {
            Py_CLEAR(e->exc_type);
            Py_CLEAR(e->exc_value);
            Py_CLEAR(e->exc_traceback);
            continue;
        }
        lasti = PyObject_GetAttrString((PyObject *)gen->gi_frame, "f_lasti");
        pos = lasti == NULL ? -1 : PyLong_AsLong(lasti);
        Py_XDECREF(lasti);
        PyErr_Clear();
        if (pos < 0 || !psyco_in_handler(gen->gi_code == NULL
                                             ? le_pyframe_code(gen->gi_frame)
                                             : (PyCodeObject *)gen->gi_code,
                                         (int)pos))
            continue;
        /* as _PyErr_GetTopmostException */
        top = PyThreadState_GET()->exc_info;
        while (top != NULL &&
               (top->exc_type == NULL || top->exc_type == Py_None))
            top = top->previous_item;
        if (top == NULL)
            continue;
        Py_XINCREF(top->exc_type);
        Py_XINCREF(top->exc_value);
        Py_XINCREF(top->exc_traceback);
        e->exc_type = top->exc_type;
        e->exc_value = top->exc_value;
        e->exc_traceback = top->exc_traceback;
    }
}
#endif

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
    int *old = PyMem_Malloc(n * sizeof(int) + 1);
    if (old == NULL)
        return PyErr_NoMemory();
    for (i = 0; i < n; i++) {
        PyFrameObject *f = (PyFrameObject *)PyTuple_GET_ITEM(args, i);
        old[i] = INT_MIN; /* not marked */
        if (!PyFrame_Check(f))
            continue;
        frame_sync_locals(f);
        if (stands_for_running(f))
            old[i] = le_pyframe_mark_running(f);
    }
#if LE_RESUMES_COROUTINES
    coros_exc_lend(args, 1);
#endif
    result = PyObject_Call(original_get_referents, args, NULL);
#if LE_RESUMES_COROUTINES
    coros_exc_lend(args, 0);
#endif
    for (i = 0; i < n; i++)
        if (old[i] != INT_MIN)
            le_pyframe_unmark_running(
                (PyFrameObject *)PyTuple_GET_ITEM(args, i), old[i]);
    PyMem_Free(old);
    return result;
}

static PyMethodDef le_get_referents_def = {"get_referents", le_get_referents,
                                           METH_VARARGS, NULL};

#if LE_RESUMES_COROUTINES
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

static PyObject *
le_get_referrers(PyObject *self, PyObject *args)
{
    psyco_coros_realise();
    return PyObject_Call(original_get_referrers, args, NULL);
}

static PyMethodDef le_get_objects_def = {
    "get_objects", (PyCFunction)(void (*)(void))le_get_objects,
    METH_VARARGS | METH_KEYWORDS, NULL};
static PyMethodDef le_get_referrers_def = {"get_referrers", le_get_referrers,
                                           METH_VARARGS, NULL};

static setter original_func_set[2];

static int
le_func_set_name(PyObject *op, PyObject *value, void *closure)
{
    psyco_coros_realise();
    return original_func_set[0](op, value, closure);
}

static int
le_func_set_qualname(PyObject *op, PyObject *value, void *closure)
{
    psyco_coros_realise();
    return original_func_set[1](op, value, closure);
}

static PyGetSetDef func_name_getset[2];

static void
coros_realised_init(PyObject *gcmod)
{
    static const char *names[2] = {"__name__", "__qualname__"};
    static const setter sets[2] = {le_func_set_name, le_func_set_qualname};
    PyObject *fn, *d = le_type_dict(&PyFunction_Type);
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
             : replacement_new(&le_get_referrers_def, gcmod,
                               original_get_referrers);
    if (fn == NULL || PyObject_SetAttrString(gcmod, "get_referrers", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    for (i = 0; i < 2; i++) {
        PyObject *descr = PyDict_GetItemString(d, names[i]);
        PyGetSetDef *old;
        if (descr == NULL || Py_TYPE(descr) != &PyGetSetDescr_Type) {
            PyErr_Clear();
            continue;
        }
        old = ((PyGetSetDescrObject *)descr)->d_getset;
        original_func_set[i] = old->set;
        func_name_getset[i] = *old;
        func_name_getset[i].set = sets[i];
        fn = PyDescr_NewGetSet(&PyFunction_Type, &func_name_getset[i]);
        if (fn == NULL || PyDict_SetItemString(d, names[i], fn) < 0)
            PyErr_Print();
        Py_XDECREF(fn);
    }
    PyType_Modified(&PyFunction_Type);
}
#endif

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
#if LE_RESUMES_COROUTINES
    coros_realised_init(mod);
#endif
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

/* each replaced in the type's dict by one like it that guards it */
static void
gen_guard_init(void)
{
    static PyTypeObject *types[2] = {&PyGen_Type, &PyCoro_Type};
    static const char *methods[2] = {"close", "throw"};
    static const char *yf[2] = {"gi_yieldfrom", "cr_await"};
    static const PyCFunction wraps[2][2] = {
        {gen_close_guarded, (PyCFunction)(void (*)(void))gen_throw_guarded},
        {coro_close_guarded, (PyCFunction)(void (*)(void))coro_throw_guarded}};
    static const getter yf_wraps[2] = {gen_yf_guarded, coro_yf_guarded};
    int t, k;
    for (t = 0; t < 2; t++) {
        PyObject *d = le_type_dict(types[t]), *descr, *fn;
        for (k = 0; k < 2; k++) {
            descr = PyDict_GetItemString(d, methods[k]);
            if (descr == NULL || Py_TYPE(descr) != &PyMethodDescr_Type ||
                ((PyMethodDescrObject *)descr)->d_method->ml_flags !=
                    (k == 0 ? METH_NOARGS : METH_FASTCALL))
                continue;
            gen_guarded_defs[t][k] = *((PyMethodDescrObject *)descr)->d_method;
            if (k == 0)
                gen_close_original[t] = gen_guarded_defs[t][k].ml_meth;
            else
                gen_throw_original[t] =
                    (_PyCFunctionFast)(void (*)(void))gen_guarded_defs[t][k]
                        .ml_meth;
            gen_guarded_defs[t][k].ml_meth = wraps[t][k];
            fn = PyDescr_NewMethod(types[t], &gen_guarded_defs[t][k]);
            if (fn == NULL || PyDict_SetItemString(d, methods[k], fn) < 0)
                PyErr_Print();
            Py_XDECREF(fn);
        }
        descr = PyDict_GetItemString(d, yf[t]);
        if (descr != NULL && Py_TYPE(descr) == &PyGetSetDescr_Type) {
            gen_guarded_getsets[t] = *((PyGetSetDescrObject *)descr)->d_getset;
            gen_getter_original[t] = gen_guarded_getsets[t].get;
            gen_guarded_getsets[t].get = yf_wraps[t];
            fn = PyDescr_NewGetSet(types[t], &gen_guarded_getsets[t]);
            if (fn == NULL || PyDict_SetItemString(d, yf[t], fn) < 0)
                PyErr_Print();
            Py_XDECREF(fn);
        }
        PyType_Modified(types[t]);
    }
}
#endif

INITIALIZATIONFN
void
psyco_frames_init(void)
{
    /* Upstream also hooked _PyThreadState_GetFrame (in 3.7,
       _PyRuntime.gilstate.getframe, behind PyEval_GetFrame), not done yet */
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
#if LE_HAS_FRAME_FUNCOBJ
    if (sysmod != NULL)
        getframemodulename_init(sysmod);
#endif
    Py_XDECREF(fn);
    Py_XDECREF(name);
    Py_XDECREF(sysmod);
    warn_init();
    get_referents_init();
#if LE_RESUMES_COROUTINES
    psyco_coro_suspended =
        PyObject_CallObject((PyObject *)&PyBaseObject_Type, NULL);
    if (psyco_coro_suspended == NULL)
        PyErr_Print();
#endif
#if LE_HAS_INTERPRETER_FRAME && !LE_HAS_FRAME_INSTR_PTR
    gen_guard_init();
#endif
#ifdef PyFrameLocalsProxy_Check
    frame_locals_proxy_init();
#endif
    frame_builtins_init();
    fn = PyDescr_NewGetSet(&PyFrame_Type, &back_getset);
    if (fn == NULL ||
        PyDict_SetItemString(le_type_dict(&PyFrame_Type), "f_back", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    lineno_descr =
        PyDict_GetItemString(le_type_dict(&PyFrame_Type), "f_lineno");
    Py_XINCREF(lineno_descr);
    fn = lineno_descr == NULL
             ? NULL
             : PyDescr_NewGetSet(&PyFrame_Type, &lineno_getset);
    if (fn == NULL ||
        PyDict_SetItemString(le_type_dict(&PyFrame_Type), "f_lineno", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    locals_descr =
        PyDict_GetItemString(le_type_dict(&PyFrame_Type), "f_locals");
    Py_XINCREF(locals_descr);
    fn = locals_descr == NULL
             ? NULL
             : PyDescr_NewGetSet(&PyFrame_Type, &locals_getset);
    if (fn == NULL ||
        PyDict_SetItemString(le_type_dict(&PyFrame_Type), "f_locals", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    lasti_descr = PyDict_GetItemString(le_type_dict(&PyFrame_Type), "f_lasti");
    Py_XINCREF(lasti_descr);
    fn = lasti_descr == NULL ? NULL
                             : PyDescr_NewGetSet(&PyFrame_Type, &lasti_getset);
    if (fn == NULL ||
        PyDict_SetItemString(le_type_dict(&PyFrame_Type), "f_lasti", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    clear_descr = PyDict_GetItemString(le_type_dict(&PyFrame_Type), "clear");
    Py_XINCREF(clear_descr);
    fn = clear_descr == NULL ? NULL
                             : PyDescr_NewMethod(&PyFrame_Type, &clear_def);
    if (fn == NULL ||
        PyDict_SetItemString(le_type_dict(&PyFrame_Type), "clear", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    PyType_Modified(&PyFrame_Type);
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

/* the slot of the record whose first entry is the innermost compiled
   frame, NULL when that frame is the real one 'real' is set to */
static stack_frame_info_t **
innermost_slot(PyFrameObject **real)
{
    le_frame_t *f = le_frame_current(PyThreadState_GET());
    FrameRuntime *fstart = f == NULL ? NULL : psyco_frame_start_get(f);
    stack_frame_info_t **f1, **last = NULL;
    *real = NULL;
    if (fstart == NULL)
        return NULL;
    *real = le_frame_object(fstart->frame);
    for (f1 = *(fstart->psy_frames_start); (((long)(*f1)) & 1) == 0;
         f1 = psyco_next_stack_frame(f1)) {
        last = f1;
        if ((*f1)->link_stack_depth == 0)
            break;
    }
    return last;
}

DEFINEFN
struct resume_s *
psyco_resume_begin(PyCodeObject *co, PyObject *globals, PyObject *builtins,
                   long nvalues, PyObject **word)
{
    PyFrameObject *real, *c = word != NULL ? (PyFrameObject *)*word : NULL;
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
    st->slot = innermost_slot(&real);
    if (st->slot != NULL && gen_frame_at(st->slot) != NULL &&
        (*st->slot)->co == co && (g = gen_kept_frame(st->slot, -1)) != NULL) {
        /* the generator's own frame, which ceval goes on with as it
           resumes it: it is linked and has its builtins already */
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
    else if (st->slot == NULL && real != NULL && le_pyframe_code(real) == co) {
        st->f = real;
        Py_INCREF(real);
#ifdef PyFrameLocalsProxy_Check
        {
            FrameRuntime *fr = psyco_frame_start_get(le_frame_of(real));
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
    else {
        st->f = le_pyframe_new(PyThreadState_GET(), co, globals);
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
    Py_XINCREF(v);
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
    for (i = 0; i < nstack; i++)
        le_frame_word_put(frame, nlocals + i, st->values[nlocals + i]);
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
        /* another frame object stood for the call: it keeps the locals as
           they are now */
        if (st->catcher != f) {
#if LE_HAS_FRAME_STACKREFS
            PyObject *locals[nlocals > 0 ? nlocals : 1];
            for (i = 0; i < nlocals; i++) locals[i] = LE_FRAME_WORD(frame, i);
#else
            PyObject **locals = le_frame_args(frame);
#endif
            psyco_frame_write_locals(st->catcher, 0, locals, nlocals);
        }
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
    if (fr != NULL)
        fr->frame = NULL;
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
    Py_DECREF(f);
    PyMem_Free(st);
    return result;
}

#if LE_RESUMES_COROUTINES
/* A coroutine whose body runs as a call from its await
   (psyfunc.c:psyco_coro_start) and suspends: the body returns
   psyco_coro_suspended, and the coroutine made for it and the value it
   yielded wait here for that await (psyco_coro_taken). Only compiled code
   runs between the two. */
DEFINEVAR PyObject *psyco_coro_suspended = NULL;
static PyObject *suspended_coro = NULL, *suspended_value = NULL;

/* As psyco_resume_run, but the frame, with what psyco_resume_begin and
   psyco_resume_value gave it, is paused at 'at' as ceval's YIELD_FROM
   leaves it, and becomes the coroutine 'func' made, as CPython made it at
   the call: its origin is not recorded (psyco_coro_start), whatever the
   depth is now. 'value' is what it yielded. The marker the body returns,
   or NULL with an exception. */
DEFINEFN PyObject *
psyco_resume_suspend(struct resume_s *st, long at, long nstack,
                     PyFunctionObject *func, PyObject *value)
{
    PyFrameObject *f = st->f;
    le_frame_t *frame = le_frame_of(f);
    int nlocals = st->nvalues - (int)nstack;
    int i;
    PyObject *coro;
    activation_t *a;
    resume_locals(st, frame, nlocals);
    for (i = 0; i < nstack; i++)
        le_frame_word_put(frame, nlocals + i, st->values[nlocals + i]);
    le_gen_frame_pause(f, le_lasti_of((int)at), (int)nstack, st->iblock,
                       st->blocks);
    le_gen_frame_end(f, value);
    for (i = 0; i < nlocals; i++) Py_XDECREF(st->values[i]);
    if (st->catcher != NULL) {
        if (st->catcher != f)
            psyco_frame_write_locals(st->catcher, 0, le_frame_args(frame),
                                     nlocals);
        Py_DECREF(st->catcher);
    }
    coro = NULL;
    if (st->slot != NULL &&
        (a = act_at(st->slot, le_pyframe_code(f))) != NULL && a->frame == f) {
        a->suspended = 1;
        /* realised already: that coroutine suspends */
        coro = a->coro;
        a->coro = NULL;
    }
    Py_CLEAR(f->f_back);
    PyMem_Free(st);
    if (coro != NULL) {
        coro_set_running(coro, 0);
        Py_DECREF(f);
    }
    else {
        coro =
            PyCoro_New(f, func->func_name, func->func_qualname); /* takes f */
        if (coro == NULL)
            return NULL;
        Py_CLEAR(((PyCoroObject *)coro)->cr_origin);
    }
    if (psyco_trace_yield(f, value) < 0) {
        Py_DECREF(coro);
        return NULL;
    }
    Py_INCREF(value);
    suspended_coro = coro;
    suspended_value = value;
    Py_INCREF(psyco_coro_suspended);
    return psyco_coro_suspended;
}

/* A body that goes on in its own handler: compiled code made the
   exception it handles current where its caller's handled exception is
   ('theirs'), keeping that one under the outermost EXCEPT_HANDLER block,
   as ceval keeps the coroutine's own. The exception moves to the
   coroutine, and the caller's goes back. */
static void
coro_exc_take(struct resume_s *st, long nstack, PyGenObject *gen,
              _PyErr_StackItem *theirs)
{
    _PyErr_StackItem *mine = &gen->gi_exc_state;
    PyObject **saved;
    int i;
    for (i = 0; i < st->iblock; i++)
        if (st->blocks[i].b_type == EXCEPT_HANDLER)
            break;
    if (i == st->iblock)
        return;
    saved = &st->values[st->nvalues - nstack + st->blocks[i].b_level];
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
}

/* As psyco_resume_run, for a coroutine's body started by its await
   (GENEXPR_DIRECT): ceval goes on with it as the coroutine 'func' made
   (realised now, if it isn't yet), as gen_send_ex runs it. What the body
   returns: the marker, if it suspended, else the result. */
DEFINEFN PyObject *
psyco_resume_run_coro(struct resume_s *st, long lasti, long nstack,
                      PyFunctionObject *func)
{
    PyFrameObject *f = st->f;
    stack_frame_info_t **slot = st->slot;
    PyThreadState *ts = PyThreadState_GET();
    PyObject *coro = NULL, *result;
    activation_t *a;
    int suspended;
    if (slot != NULL && (a = act_at(slot, le_pyframe_code(f))) != NULL &&
        a->frame == f) {
        coro = a->coro;
        a->coro = NULL;
    }
    if (coro == NULL) {
        Py_INCREF(f);
        coro = PyCoro_New(f, func->func_name, func->func_qualname);
        if (coro == NULL) {
            Py_DECREF(f);
            if (st->catcher != NULL)
                Py_DECREF(st->catcher);
            Py_DECREF(f);
            PyMem_Free(st);
            return NULL;
        }
        Py_CLEAR(((PyCoroObject *)coro)->cr_origin);
    }
    coro_set_running(coro, 1);
    coro_exc_take(st, nstack, (PyGenObject *)coro, ts->exc_info);
    ((PyGenObject *)coro)->gi_exc_state.previous_item = ts->exc_info;
    ts->exc_info = &((PyGenObject *)coro)->gi_exc_state;
    result = psyco_resume_run(st, lasti, nstack); /* frees st */
    ts->exc_info = ((PyGenObject *)coro)->gi_exc_state.previous_item;
    ((PyGenObject *)coro)->gi_exc_state.previous_item = NULL;
    coro_set_running(coro, 0);
#if LE_HAS_FRAME_STATE
    suspended = result != NULL && f->f_state == FRAME_SUSPENDED;
#else
    suspended = result != NULL && f->f_stacktop != NULL;
#endif
    if (slot != NULL && (a = act_at(slot, le_pyframe_code(f))) != NULL &&
        a->frame == f)
        a->suspended = suspended;
    if (!suspended) {
        coro_finish(coro);
        return result;
    }
    Py_CLEAR(f->f_back);
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
#endif
