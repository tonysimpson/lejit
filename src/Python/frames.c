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
#include "pycompiler.h"
#include "../bytecode.h"
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
    if (inlining) {
        (p + 1)->co = caller->pr.co;
        (p + 1)->nargs = -1;
        (p + 1)->back_lasti = inline_parent_lasti(caller);
        sglobals = caller->vlocals.items[INDEX_LOC_GLOBALS]->source;
        if (is_compiletime(sglobals))
            (p + 1)->globals = (PyObject *)CompileTime_Get(sglobals)->value;
        else
            (p + 1)->globals = NULL; /* uncommon */
    }
    return p;
}

/* the records of the real frames LeCode_Run runs on this thread,
   innermost first */
static _Thread_local FrameRuntime *frame_runs;

/* Every thread's frame_runs, for the owner scan (psyco_frames_running):
   its address is in a capsule in the thread state's dict, which goes when
   the thread state is cleared, before the thread ends. */
static _Thread_local char runs_listed;
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
    FrameRuntime **p = &frame_runs;
    int i;
    /* fr is the innermost unless the C stack was switched under it */
    while (*p != fr) p = &(*p)->next;
    *p = fr->next;
    for (i = 0; i < fr->n_entry_locals; i++) Py_XDECREF(fr->entry_locals[i]);
    PyMem_Free(fr->entry_locals);
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
    PyObject **locals = le_frame_args(le_frame_of(f));
    int n = le_code_nlocalsplus(co);
    int i;
    /* an emulated frame isn't executing, and the first write is from 0 */
    FrameRuntime *fr = start == 0 && le_frame_is_executing(f)
                           ? psyco_frame_start_get(le_frame_of(f))
                           : NULL;
    if (fr != NULL && fr->entry_locals == NULL) {
        PyObject **keep = PyMem_Malloc(n * sizeof(PyObject *) + 1);
        if (keep == NULL)
            return; /* the locals stay as they were */
        for (i = 0; i < n; i++) {
            keep[i] = locals[i];
            Py_XINCREF(keep[i]);
        }
        fr->entry_locals = keep;
        fr->n_entry_locals = n;
    }
    for (i = 0; i < count && start + i < n; i++) {
        PyObject *old = locals[start + i];
        if (values[i] == LE_LOCAL_UNKNOWN)
            continue;
        /* NULL: unbound, or deleted since the last write */
        Py_XINCREF(values[i]);
        locals[start + i] = values[i];
        Py_XDECREF(old);
    }
}

#if LE_RESUMES_GENERATORS
/* What Python wrote in the locals while the body ran
   (psyco_frame_write_locals) is released as the run ends
   (psyco_frame_start_unregister), not here, where it could run code: the
   slots take the compiled code's values after this. */
DEFINEFN PyFrameObject *
psyco_gen_pause(FrameRuntime *fr, const le_gen_pause_t *p)
{
    PyFrameObject *f = fr->frame;
    if (fr->entry_locals != NULL) {
        PyObject **locals = le_frame_args(f);
        int i, n = le_code_nlocals(le_frame_code(f));
        PyObject **keep = PyMem_Realloc(
            fr->entry_locals, (fr->n_entry_locals + n) * sizeof(PyObject *));
        for (i = 0; i < n; i++) {
            if (locals[i] == NULL)
                continue;
            if (keep != NULL)
                keep[fr->n_entry_locals++] = locals[i];
            else
                Py_DECREF(locals[i]); /* out of memory */
            locals[i] = NULL;
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
   longer runs (no tuple of its code and tag in LeJIT's chain), the frame
   keeps what was read last. */
static char locals_note;
#define LOCALS_NOTE ((PyObject *)&locals_note)

static PyObject *
pvisitframes(PyObject *(*callback)(PyObject *, void *), void *data);

static PyObject *
visit_tag(PyObject *o, void *f)
{
    PyFrameObject *frame = (PyFrameObject *)f;
    if (PyTuple_Check(o) && !PyFrame_Check(PyTuple_GET_ITEM(o, 2)) &&
        PyTuple_GET_ITEM(o, 0) == (PyObject *)le_pyframe_code(frame) &&
        PyLong_AsLong(PyTuple_GET_ITEM(o, 2)) ==
            (long)le_pyframe_stack(frame)[1]) {
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
    int resumed; /* ceval runs it now */
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
    if (le_pyframe_code(f)->co_stacksize >= 2) {
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

/* a kept frame the compiled code still runs, not ceval */
static activation_t *
live_activation(PyFrameObject *f)
{
    activation_t *a = activation_of(f);
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
    *ret = psyco_return_barrier;
    Py_INCREF(f);
    return f;
}

DEFINEFN
code_t *
psyco_activation_returned(long *sp)
{
    activation_t *a = act_find((stack_frame_info_t **)(sp + 1));
    PyFrameObject *f;
    PyObject *t, *v, *tb;
    code_t *ret;
    if (a == NULL)
        psyco_fatal_msg("a kept frame's call returned unknown");
    f = a->frame;
    ret = a->ret;
    /* the call is over, as its caller is about to say: the frame's
       finaliser may walk the frames */
    *a->slot = (stack_frame_info_t *)-1;
    act_remove(a);
    le_pyframe_set_executing(f, 0);
    if (le_pyframe_code(f)->co_stacksize >= 2 &&
        le_pyframe_stack(f)[0] == ACTIVATION_NOTE)
        le_pyframe_stack(f)[0] = le_pyframe_stack(f)[1] = NULL;
    PyErr_Fetch(&t, &v, &tb);
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
    PyFrameObject *real;
    stack_frame_info_t **slot;
    activation_t *a;
    if (acts_count == 0 || (slot = innermost_slot(&real)) == NULL ||
        (a = act_at(slot, co)) == NULL || a->resumed)
        return NULL;
    return a->frame;
}

DEFINEFN
PyFrameObject *
psyco_emulate_frame(PyObject *o)
{
    if (PyFrame_Check(o)) {
        /* a real Python frame */
        Py_INCREF(o);
        return (PyFrameObject *)o;
    }
    else if (PyFrame_Check(PyTuple_GET_ITEM(o, 2))) {
        /* (co, globals, f, lasti): the real frame whose code LeCode_Run is
           running */
        PyObject *f = PyTuple_GET_ITEM(o, 2);
        Py_INCREF(f);
        return (PyFrameObject *)f;
    }
    else {
        /* a Psyco frame: emulate it */
        PyObject *co = PyTuple_GetItem(o, 0);
        PyObject *globals = PyTuple_GetItem(o, 1);
        PyFrameObject *result;
        extra_assert(co != NULL);
        extra_assert(globals != NULL);
        long tag = PyLong_AsLong(PyTuple_GET_ITEM(o, 2));
        int lasti = (int)PyLong_AsLong(PyTuple_GET_ITEM(o, 3));
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
                slot_locals(psyco_next_stack_frame((stack_frame_info_t **)tag),
                            (PyCodeObject *)co,
                            le_frame_args(le_frame_of(result)));
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
pvisitframes(PyObject *(*callback)(PyObject *, void *), void *data)
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
            PyObject *o;
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
                /* a Psyco frame is represented as
                   (co, globals, address_of(*fi)) */
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
                        g = fdata->globals;
                        if (g == NULL)
                            g = le_frame_globals(le_frame_of(fo));
                        o = Py_BuildValue("OOli", fdata->co, g, tag, lasti);
                        lasti = fdata->back_lasti;
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
            next_record:
                revlist = p->next;
                PyMem_FREE(p);
            }
            if (result != NULL)
                break;

            /* there is still the real Python frame
               which is shadowed by a Psyco frame, i.e. a
               proxy function. Represented as
               (co, globals, f, lasti) */
            if (returning) {
                fo = le_frame_object_back(fo);
                continue;
            }
            o = Py_BuildValue("OOOi", fstart->psy_code, fstart->psy_globals,
                              fo, lasti);
            if (o == NULL)
                OUT_OF_MEMORY();
            result = callback(o, data);
            Py_DECREF(o);
        }
        else {
            /* a real unshadowed Python frame */
            result = callback((PyObject *)fo, data);
        }
        if (result != NULL)
            break;
        fo = le_frame_object_back(fo);
    }
    le_recursion_resume();
    return result;
}

static PyObject *
visit_get_globals(PyObject *o, void *ignored)
{
    if (PyFrame_Check(o))
        return le_frame_globals(le_frame_of((PyFrameObject *)o));
    else
        return PyTuple_GetItem(o, 1);
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
visit_link_frame(PyObject *o, void *data)
{
    struct link_s *l = (struct link_s *)data;
    PyFrameObject *f, *back;
    PyObject *t, *old;
    if (l->depth > 0) {
        l->depth--;
        return NULL;
    }
    f = psyco_emulate_frame(o);
    if (l->undo != NULL && PyTuple_Check(o) &&
        PyFrame_Check(PyTuple_GET_ITEM(o, 2)) &&
        PyLong_AsLong(PyTuple_GET_ITEM(o, 3)) >= 0 &&
        (t = Py_BuildValue("Oi", f, le_frame_lasti(le_frame_of(f)))) != NULL) {
        /* a frame LeCode_Run runs, at its current place for the call */
        if (PyList_Append(l->undo, t) == 0)
            le_frame_set_lasti(
                le_frame_of(f),
                le_lasti_of((int)PyLong_AsLong(PyTuple_GET_ITEM(o, 3))));
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
visit_back_frame(PyObject *o, void *data)
{
    struct back_s *b = (struct back_s *)data;
    PyFrameObject *f;
    if (!b->found) {
        if (b->frame == NULL)
            b->found = PyTuple_Check(o) &&
                       PyLong_Check(PyTuple_GET_ITEM(o, 2)) &&
                       PyLong_AsLong(PyTuple_GET_ITEM(o, 2)) == b->tag &&
                       PyTuple_GET_ITEM(o, 0) == (PyObject *)b->co;
        else {
            PyObject *real = PyFrame_Check(o) ? o
                             : PyFrame_Check(PyTuple_GET_ITEM(o, 2))
                                 ? PyTuple_GET_ITEM(o, 2)
                                 : NULL;
            b->found = real == (PyObject *)b->frame;
        }
        return NULL;
    }
    f = psyco_emulate_frame(o);
    if (f == NULL)
        return Py_None; /* stop */
    if (b->prev != NULL && b->prev->f_back != NULL) {
        /* a kept frame, linked when it was first asked for */
        Py_DECREF(f);
        return Py_None;
    }
    if (!PyTuple_Check(o) || PyFrame_Check(PyTuple_GET_ITEM(o, 2))) {
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

/* A frame's locals as they are now, for each way Python reads them
   (frame.f_locals; locals and vars by another name, cimpl_with_frame;
   gc.get_referents, le_get_referents): a real frame compiled code runs, a
   kept frame, an emulated frame and a catching callee's traceback frame
   read theirs from their compiled frame while it is in a call that says
   where they are (slot_locals). Else they stay as last written. */
static void
frame_sync_locals(PyFrameObject *f)
{
    activation_t *a;
    stack_frame_info_t **own, **tag;
    FrameRuntime *fr;
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

static PyObject *
frame_get_back(PyFrameObject *frame, void *closure)
{
    PyFrameObject *back = le_frame_object_back(frame);
    if (back == NULL && has_catcher_note(frame))
        back = frame->f_back = catcher_caller(frame); /* takes the reference */
    if (back == NULL)
        Py_RETURN_NONE;
    if (le_frame_is_executing(frame) && runs_compiled(back) &&
        live_activation(frame) == NULL) {
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
   f_lasti, so its place comes from LeJIT's chain, where its tuple carries
   it. The setter (a jump, under tracing) is ceval's. */
static PyObject *lineno_descr = NULL;

static PyObject *
visit_root_lasti(PyObject *o, void *frame)
{
    if (PyTuple_Check(o) && PyTuple_GET_ITEM(o, 2) == (PyObject *)frame) {
        Py_INCREF(PyTuple_GET_ITEM(o, 3));
        return PyTuple_GET_ITEM(o, 3);
    }
    return NULL;
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
    PyObject *o;
    int lasti = -1;
    if (frame->f_trace != NULL)
        return -1;
    if ((a = live_activation(frame)) != NULL) {
        lasti = kept_lasti(a);
        if (lasti >= 0)
            le_frame_set_lasti(le_frame_of(frame), le_lasti_of(lasti));
    }
    else if (runs_compiled(frame)) {
        o = pvisitframes(visit_root_lasti, frame);
        lasti = o == NULL ? -1 : (int)PyLong_AsLong(o);
        Py_XDECREF(o);
        PyErr_Clear();
    }
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
       real frame or a kept one, which are running */
    if (!le_frame_is_executing(f) && le_frame_stack_empty(f) &&
        le_pyframe_code(f)->co_stacksize >= 3) {
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
le_getframe(PyObject *self, PyObject *args, PyObject *kwds)
{
    int depth = 0;
    PyFrameObject *top;
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
    if (depth < 0)
        depth = 0;
#if LE_HAS_AUDIT_HOOKS
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
    func = le_frame_of(f)->f_funcobj;
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

static PyObject *
le_warn(PyObject *self, PyObject *args, PyObject *kwds)
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
    result = PyObject_Call(original_warn, args, kwds);
    le_frame_set_current(ts, saved);
    if (top != NULL && le_frame_of(top) != saved)
        le_pyframe_current_end(top);
    restore_links(undo);
    Py_DECREF(undo);
    Py_XDECREF(top);
    return result;
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
visit_root_lasti(PyObject *o, void *frame);
static PyObject *
frame_get_back(PyFrameObject *frame, void *closure);

static PyFrameObject *
root_copy(PyFrameObject *f)
{
    FrameRuntime *fr = psyco_frame_start_get(le_frame_of(f));
    PyFrameObject *c;
    PyObject *o;
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
    o = pvisitframes(visit_root_lasti, f);
    le_frame_set_lasti(le_frame_of(c),
                       o == NULL ? -1 : le_lasti_of((int)PyLong_AsLong(o)));
    Py_XDECREF(o);
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

static PyMethodDef le_warn_def = {
    "warn", (PyCFunction)(void (*)(void))le_warn, METH_VARARGS | METH_KEYWORDS,
    "warn(message, category=None, stacklevel=1)\n\n"
    "Issue a warning, or maybe ignore it or raise an exception, counting\n"
    "frames of code compiled by LeJIT for stacklevel."};

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
    fn = PyCFunction_New(&le_warn_def, NULL);
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
    result = PyObject_Call(original_get_referents, args, NULL);
    for (i = 0; i < n; i++)
        if (old[i] != INT_MIN)
            le_pyframe_unmark_running(
                (PyFrameObject *)PyTuple_GET_ITEM(args, i), old[i]);
    PyMem_Free(old);
    return result;
}

static PyMethodDef le_get_referents_def = {
    "get_referents", le_get_referents, METH_VARARGS,
    "get_referents(*objs) -> list\n"
    "Return the list of objects that are directly referred to by objs."};

static void
get_referents_init(void)
{
    PyObject *mod = PyImport_ImportModule("gc"), *name, *fn = NULL;
    if (mod == NULL) {
        PyErr_Print();
        return;
    }
    original_get_referents = PyObject_GetAttrString(mod, "get_referents");
    name = PyUnicode_FromString("gc");
    if (name != NULL)
        fn = PyCFunction_NewEx(&le_get_referents_def, NULL, name);
    if (original_get_referents == NULL || fn == NULL ||
        PyObject_SetAttrString(mod, "get_referents", fn) < 0)
        PyErr_Print();
    Py_XDECREF(fn);
    Py_XDECREF(name);
    Py_DECREF(mod);
}

static PyMethodDef le_getframe_def = {
    "_getframe", (PyCFunction)(void (*)(void))le_getframe,
    METH_VARARGS | METH_KEYWORDS,
    "_getframe([depth]) -> frameobject\n\n"
    "Return a frame object from the call stack, counting frames of code\n"
    "compiled by LeJIT (emulated)."};

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
    st->slot = innermost_slot(&real);
    if (st->slot == NULL && real != NULL && le_pyframe_code(real) == co) {
        st->f = real;
        Py_INCREF(real);
    }
    else if (st->slot != NULL && (a = act_at(st->slot, co)) != NULL &&
             !a->resumed) {
        /* the frame asked for while compiled code ran the call */
        a->resumed = 1;
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

DEFINEFN
PyObject *
psyco_resume_run(struct resume_s *st, long lasti, long nstack)
{
    PyFrameObject *f = st->f, *cur;
    PyObject **locals = le_frame_args(le_frame_of(f));
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

    /* the old locals go only once all the new ones are in: one may hold
       the last reference to another's value */
    for (i = 0; i < nlocals; i++) {
        PyObject *old = locals[i];
        locals[i] = st->values[i];
        st->values[i] = old;
    }
    for (i = 0; i < nstack; i++)
        le_pyframe_stack(f)[i] = st->values[nlocals + i];
    le_frame_set_stack_depth(f, (int)nstack);
    le_frame_set_lasti(le_frame_of(f), le_lasti_of((int)lasti));
#if !LE_HAS_EXCEPTION_TABLE
    f->f_iblock = st->iblock;
    memcpy(f->f_blockstack, st->blocks, st->iblock * sizeof(PyTryBlock));
#endif
    for (i = 0; i < nlocals; i++) Py_XDECREF(st->values[i]);
    if (st->catcher != NULL) {
        /* another frame object stood for the call: it keeps the locals as
           they are now */
        if (st->catcher != f)
            psyco_frame_write_locals(st->catcher, 0, locals, nlocals);
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
    if (st->slot != NULL && kept_back != cur) {
        Py_XINCREF(cur);
        f->f_back = cur;
    }
    else
        kept_back = NULL;
#if LE_PYFRAME_RUN_TO_END
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
