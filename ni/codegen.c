/*
 * The bridge between translated code and the backend.
 *  - psyco_generic_call is the residual call. Argument tags (v/l/d/r/a/A) say
 *    how each argument is passed, and Cf* flags (cf_flags.h) give the return
 *    type and error convention. With CfPure and every argument compile-time,
 *    the C function is called now (via libffi) and its result is a constant;
 *    a fresh object, which CPython would make on every run, is thrown away
 *    and the call emitted instead (vcompiler.h:psyco_shared_constant).
 *  - Condition codes are virtual values (cc_functions_table) until something
 *    needs them in a register.
 *  - The integer_* functions fold constants and otherwise call the x86-64 ALU
 *    emitters, so this file is part core and part backend.
 */
#include "codegen.h"
#include <iencoding.h>
#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "codemanager.h"
#include "pycodegen.h"

/*** The processor-independent part of the code generator. ***/

DEFINEFN
void
psyco_emit_header(PsycoObject *po, int nframelocal)
{
    int j = nframelocal;
    vinfo_array_t *array;
    extra_assert(LOC_CONTINUATION->array->count == 0);

    BEGIN_CODE
    INITIALIZE_FRAME_LOCALS(nframelocal);
    po->stack_depth += sizeof(long) * nframelocal;
    END_CODE

    array = LOC_CONTINUATION->array = array_new(nframelocal);
    while (j--)
        array->items[j] = vinfo_new(RunTime_NewStack(
            po->stack_depth - sizeof(long) * j, false, false));
}

#define INITIAL_STACK_DEPTH_INUSE \
    (INITIAL_STACK_DEPTH + (!NEED_STACK_FRAME_HACK) * sizeof(long))

DEFINEFN
code_t *
psyco_finish_return(PsycoObject *po, Source retval)
{
    code_t *code;
    int nframelocal = LOC_CONTINUATION->array->count;
    int retpos = getstack(LOC_CONTINUATION->source);
    extra_assert(retpos != RunTime_StackNone);
    function_return(po, retval, nframelocal, retpos);
    code = po->code;
    PsycoObject_Delete(po);
    return code;
}

/***************************************************************/
/*** Condition Codes (a.k.a. the processor 'flags' register) ***/

#if HAVE_CCREG
static source_virtual_t cc_functions_table[CC_TOTAL];

DEFINEFN
condition_code_t
cc_from_vsource(Source source)
{
    source_virtual_t *sv = VirtualTime_Get(source);
    return (condition_code_t)(sv - cc_functions_table);
}

static bool
generic_computed_cc(PsycoObject *po, vinfo_t *v)
{
    /* also upon forking, because the condition codes cannot be
       sent as arguments (function calls typically require adds
       and subs to adjust the stack). */
    /* 'v' should be one of the po->ccregs[_] here */
    BEGIN_CODE
    code = psyco_compute_cc(po, code
#if REG_TOTAL > 0
                            ,
                            REG_NONE
#endif
    );
    END_CODE
    return true;
}

DEFINEFN
vinfo_t *
psyco_vinfo_condition(PsycoObject *po, condition_code_t cc)
{
    vinfo_t *result;
    if ((int)cc < CC_TOTAL) {
        int index = INDEX_CC(cc);
        if (po->ccregs[index] != NULL) {
            /* there is already a value in the processor flags register */
            condition_code_t prevcc =
                psyco_vsource_cc(po->ccregs[index]->source);
            extra_assert(prevcc != CC_ALWAYS_FALSE);

            if (prevcc == cc) {
                /* it is the same condition, so reuse it */
                result = po->ccregs[index];
                vinfo_incref(result);
                return result;
            }
            else {
                /* it is not the same condition at all, save the old one and
                   make a new one (should never occur with the ivm backend) */
                BEGIN_CODE
                NEED_CC();
                END_CODE
            }
        }
        extra_assert(po->ccregs[index] == NULL);
        result = vinfo_new(VirtualTime_New(cc_functions_table + (int)cc));
        po->ccregs[index] = result;
    }
    else
        result = vinfo_new(CompileTime_New(cc == CC_ALWAYS_TRUE));
    return result;
}

DEFINEFN
VirtualTimeSource
psyco_source_condition(condition_code_t cc)
{
    return VirtualTime_New(cc_functions_table + (int)cc);
}

DEFINEFN
condition_code_t
psyco_vsource_cc(Source source)
{
    if (is_virtualtime(source)) {
        source_virtual_t *sv = VirtualTime_Get(source);
        if (cc_functions_table <= sv && sv < cc_functions_table + CC_TOTAL) {
            /* 'sv' points within the cc_functions_table */
            return (condition_code_t)(sv - cc_functions_table);
        }
    }
    return CC_ALWAYS_FALSE;
}

DEFINEFN
void
psyco_resolved_cc(PsycoObject *po, condition_code_t cc_known_true)
{
    if ((int)cc_known_true < CC_TOTAL) {
        vinfo_t *v;
        int index;

        index = INDEX_CC(cc_known_true);
        v = po->ccregs[index];
        if (v != NULL && psyco_vsource_cc(v->source) == cc_known_true) {
            sk_incref(&psyco_skOne);
            v->source = CompileTime_NewSk(&psyco_skOne);
            po->ccregs[index] = NULL;
        }

        index = INDEX_CC(INVERT_CC(cc_known_true));
        v = po->ccregs[index];
        if (v != NULL &&
            psyco_vsource_cc(v->source) == INVERT_CC(cc_known_true)) {
            sk_incref(&psyco_skZero);
            v->source = CompileTime_NewSk(&psyco_skZero);
            po->ccregs[index] = NULL;
        }
    }
}

#else /* if !HAVE_CCREG */

#error "codegen.c needs HAVE_CCREG, which the x64 backend defines"

#endif /* HAVE_CCREG */

/*****************************************************************/
/***   run-time switches                                       ***/

static bool
computed_promotion(PsycoObject *po, vinfo_t *v)
{
    /* uncomputable, but still use the address of computed_promotion() as a
       tag to figure out if a virtual source is a c_promotion_s structure. */
    return psyco_vsource_not_important.compute_fn(po, v);
}

DEFINEVAR c_promotion_t psyco_nonfixed_promotion;
DEFINEVAR c_promotion_t psyco_nonfixed_pyobj_promotion;
/*DEFINEVAR c_promotion_t psyco_nonfixed_promotion_mega;*/
DEFINEVAR c_promotion_t psyco_nonfixed_pyobj_promotion_mega;
DEFINEVAR c_promotion_t psyco_static_type_promotion;
DEFINEVAR c_promotion_t psyco_interp_pyobj_promotion;

DEFINEFN
bool
psyco_vsource_is_promotion(VirtualTimeSource source)
{
    return VirtualTime_Get(source)->compute_fn == &computed_promotion;
}

/*****************************************************************/
/***   Calling C functions                                     ***/

/* implemented in iencoding.c */
EXTERNFN vinfo_t *
compile_time_call(PsycoObject *po, void *c_function, int flags, int arg_count,
                  char argtags[], long args[]);
EXTERNFN vinfo_t *
run_time_call(PsycoObject *po, void *c_function, int flags, int arg_count,
              char argtags[], long args[], int total_output_array_size);

#define MAX_ARGUMENTS_COUNT 8 /* as the psyco_generic_call macro (codegen.h) */

DEFINEFN
vinfo_t *
(psyco_generic_call)(PsycoObject *po, void *c_function, int flags,
                   const char *arguments, ...)
{
    char argtags[MAX_ARGUMENTS_COUNT];
    long raw_args[MAX_ARGUMENTS_COUNT], args[MAX_ARGUMENTS_COUNT];
    int count, i, j, totalstackspace = 0;
    vinfo_t *vresult;
    bool has_refs = false;

    va_list vargs;

#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, arguments);
#else
    va_start(vargs);
#endif
    extra_assert(c_function != NULL);

    for (count = 0; arguments[count]; count++) {
        long arg;
        char tag;
        vinfo_t *vi;

        extra_assert(count < MAX_ARGUMENTS_COUNT);
        raw_args[count] = arg = va_arg(vargs, long);
        tag = arguments[count];

        switch (tag) {
            case 'l':
                break;
            case 'd':
                vi = (vinfo_t *)arg;
                if (!compute_vinfo(vi, po))
                    return NULL;
                if (!is_compiletime(vi->source)) {
                    flags &= ~CfPure;
                }
                else {
                    /* compile-time: get the value */
                    arg = CompileTime_Get(vi->source)->value;
                    tag = 'D';
                }
                break;
            case 'v':
                /* Compute all values first */
                vi = (vinfo_t *)arg;
                if (!compute_vinfo(vi, po))
                    return NULL;
                if (!is_compiletime(vi->source)) {
                    flags &= ~CfPure;
                }
                else {
                    /* compile-time: get the value */
                    arg = CompileTime_Get(vi->source)->value;
                    tag = 'l';
                }
                break;
            case 'r':
                /* Push by-reference values in the stack now */
                vi = (vinfo_t *)arg;
                extra_assert(is_runtime(vi->source));
                if (getstack(vi->source) == RunTime_StackNone) {
                    reg_t rg = getreg(vi->source);
                    if (rg == REG_NONE) {
                        /* for undefined sources, pushing
                           just any register will be fine */
                        rg = REG_TRANSIENT_1;
                    }
                    BEGIN_CODE
                    SAVE_REG_VINFO(vi, rg);
                    END_CODE
                }
                arg = RunTime_NewStack(getstack(vi->source), false, false);
                has_refs = true;
                flags &= ~CfPure;
                break;
            case 'a':
            case 'A':
                has_refs = true;
                totalstackspace +=
                    sizeof(long) * ((vinfo_array_t *)arg)->count;
                break;
            default:
                Py_FatalError(
                    "unknown character argument in"
                    " psyco_generic_call()");
        }
        args[count] = arg;
        argtags[count] = tag;
    }
    va_end(vargs);
    vresult = VINFO_FRESH;
    if (flags & CfPure) {
        /* calling a pure function with no run-time argument now at compile
         * time */
        if (has_refs) {
            for (i = 0; i < count; i++) {
                if (argtags[i] == 'a' || argtags[i] == 'A') {
                    int cnt = ((vinfo_array_t *)args[i])->count;
                    args[i] = (long)malloc(cnt * sizeof(long));
                }
            }
        }
        vresult =
            compile_time_call(po, c_function, flags, count, argtags, args);
        if (has_refs) {
            if (vresult == NULL || vresult == VINFO_FRESH) {
                for (i = 0; i < count; i++) {
                    if (argtags[i] == 'a' || argtags[i] == 'A') {
                        free((void *)args[i]);
                        args[i] = raw_args[i];
                    }
                }
            }
            else {
                for (i = 0; i < count; i++) {
                    if (argtags[i] == 'a' || argtags[i] == 'A') {
                        vinfo_array_t *array = (vinfo_array_t *)raw_args[i];
                        long sk_flag = (argtags[i] == 'a') ? 0 : SkFlagPyObj;
                        for (j = 0; j < array->count; j++) {
                            array->items[j] = vinfo_new(CompileTime_NewSk(
                                sk_new(((long *)args[i])[j], sk_flag)));
                        }
                        free((void *)args[i]);
                    }
                }
            }
        }
        /* a fresh object is made at run time, by a call that cannot block */
        flags |= CfNoBlock;
    }
    if (vresult == VINFO_FRESH) {
        /* compile the run time call */
        if (!(flags & CfNoBlock) && !psyco_cannot_block(c_function))
            po->blocking_call = 1;
        if (!(flags & (CfPure | CfNoPython))) {
            po->pr.class_unchecked = 1; /* it may assign __class__ */
            po->pr.python_call = 1;
            psyco_note_call_site(po, po->pr.call_site
                                         ? po->pr.call_site - 1
                                         : po->pr.next_instr & ~NEXT_INSTR_POP);
        }
        vresult = run_time_call(po, c_function, flags, count, argtags, args,
                                totalstackspace);
        po->pr.pending_map = NULL;
        if (vresult == NULL && has_refs) {
            /* error - we should free the vinfo_ts we created */
            for (i = 0; i < count; i++) {
                if (argtags[i] == 'a' || argtags[i] == 'A') {
                    vinfo_array_t *array = (vinfo_array_t *)args[i];
                    int j = array->count;
                    while (j--) {
                        vinfo_t *v = array->items[j];
                        array->items[j] = NULL;
                        v->source = remove_rtref(v->source);
                        vinfo_decref(v, po);
                    }
                }
            }
        }
    }
    return vresult;
}

DEFINEFN
void
psyco_inline_enter(PsycoObject *po)
{
    struct stack_frame_info_s *finfo = psyco_finfo(NULL, po);
    BEGIN_CODE
    ABOUT_TO_CALL_SUBFUNCTION((long)finfo);
    END_CODE
}

/* While the frame runs, its finfo slot holds an odd word: before a call
   that may go through C, (lasti << 1) | 1, for f_lineno of its emulated
   frame (frames.c:pvisitframes); a direct call to compiled code needs no
   note. An inlined frame's slot holds its record. */
/* every map a note names, by index: (index << 1) | 1 fits the 32-bit
   immediate of one store. The map belongs to the owner of the code making
   the call, which keeps the index and gives it back when it dies; only a
   frame running that code reads its notes, so no live note names a reused
   index. */
DEFINEVAR struct locals_map_s **psyco_locals_maps = NULL;
DEFINEVAR long psyco_stat_notes_made = 0, psyco_stat_notes_free = 0;
static int *free_notes = NULL;
static long free_notes_cap = 0;

static long note_index(void)
{
    long i;
    ni_owner_t *o = psyco_owner;
    if (o != NULL && o->nnotes == o->notescap) {
        int cap = o->notescap ? 2 * o->notescap : 8;
        void *p = PyMem_Realloc(o->notes, cap * sizeof(int));
        if (p == NULL)
            OUT_OF_MEMORY();
        o->notes = p;
        o->notescap = cap;
    }
    if (psyco_stat_notes_free > 0)
        i = free_notes[--psyco_stat_notes_free];
    else {
        i = psyco_stat_notes_made;
        if ((i & (i - 1)) == 0) {
            void *p = PyMem_Realloc(psyco_locals_maps,
                                    2 * (i + 1) * sizeof(*psyco_locals_maps));
            if (p == NULL || i >= (1L << 30))
                OUT_OF_MEMORY();
            psyco_locals_maps = p;
        }
        psyco_stat_notes_made++;
    }
    if (o != NULL)
        o->notes[o->nnotes++] = (int)i;
    return i;
}

DEFINEFN
void
psyco_release_notes(int *notes, int n)
{
    int k;
    if (psyco_stat_notes_free + n > free_notes_cap) {
        long cap = 2 * (psyco_stat_notes_free + n);
        void *p = PyMem_Realloc(free_notes, cap * sizeof(int));
        if (p == NULL)
            return; /* those indices are not reused */
        free_notes = p;
        free_notes_cap = cap;
    }
    for (k = 0; k < n; k++) {
        psyco_locals_maps[notes[k]] = NULL;
        free_notes[psyco_stat_notes_free++] = notes[k];
    }
}

DEFINEFN
void
psyco_note_call_site(PsycoObject *po, int lasti)
{
    struct locals_map_s *map;
    long i;
    if (po->pr.is_inlining || lasti < 0)
        return;
    map = psyco_locals_map(po, lasti, NULL);
    i = note_index();
    psyco_locals_maps[i] = map;
    po->pr.pending_map = map;
    BEGIN_CODE
    SAVE_IMMED_TO_EBP_BASE((i << 1) | 1, INITIAL_STACK_DEPTH);
    END_CODE
}

struct lm_build {
    long *items;
    int count, size;
};

static void lm_add(struct lm_build *b, long kind, long value, long n)
{
    if (b->count + 3 > b->size) {
        long *items = PyMem_Realloc(b->items, (b->size * 2 + 48) * sizeof(long));
        if (items == NULL)
            OUT_OF_MEMORY();
        b->items = items;
        b->size = b->size * 2 + 48;
    }
    b->items[b->count++] = kind;
    b->items[b->count++] = value;
    b->items[b->count++] = n;
}

/* Virtual values are kept only if they can be computed from their items
   at run time (direct_compute_fn), up to a few levels deep. */
static void lm_value(PsycoObject *po, struct lm_build *b, vinfo_t *vi,
                     const long *reg_pos, int level)
{
    if (vi == NULL) {
        lm_add(b, LM_NONE, 0, 0);
        return;
    }
    switch (gettime(vi->source)) {
    case RunTime: {
        long pos = getstack(vi->source);
        if (pos == RUNTIME_STACK_NONE) {
            reg_t rg = getreg(vi->source);
            if (rg == REG_NONE) {
                lm_add(b, LM_NONE, 0, 0);
                return;
            }
            if (reg_pos != NULL) {
                pos = reg_pos[rg];
            }
            else if (callee_saved_reg_table[rg]) {
                /* the C callee keeps it: frames.c finds where */
                lm_add(b, LM_REG, rg, 0);
                return;
            }
            else {
                /* the call will move it (RAX, from a call just made) */
                BEGIN_CODE
                SAVE_REG_VINFO(vi, rg);
                END_CODE
                pos = getstack(vi->source);
            }
        }
        lm_add(b, LM_STACK, pos, 0);
        return;
    }
    case CompileTime: {
        source_known_t *sk = CompileTime_Get(vi->source);
        if (sk->refcount1_flags & SkFlagPyObj)
            psyco_owner_pin((PyObject *)sk->value);
        lm_add(b, LM_CONST, sk->value, 0);
        return;
    }
    default: {
        source_virtual_t *sv = VirtualTime_Get(vi->source);
        int i, n = vi->array->count;
        if (sv->direct_compute_fn == NULL || level > 3) {
            lm_add(b, LM_NONE, 0, 0);
            return;
        }
        lm_add(b, LM_VIRTUAL, (long)sv, n);
        for (i = 0; i < n; i++)
            lm_value(po, b, vi->array->items[i], reg_pos, level + 1);
        return;
    }
    }
}

DEFINEFN
struct locals_map_s *
psyco_locals_map(PsycoObject *po, int lasti, const long *reg_pos)
{
    PyCodeObject *co = po->pr.co;
    int i, n = co->co_nlocals + (int)PyTuple_GET_SIZE(co->co_cellvars) +
               (int)PyTuple_GET_SIZE(co->co_freevars);
    /* kept between maps, as there is one per call site */
    static struct lm_build b = {NULL, 0, 0};
    struct locals_map_s *map;
    if (po->pr.is_inlining)
        return NULL;
    b.count = 0;
    for (i = 0; i < n; i++)
        lm_value(po, &b, LOC_LOCALS_PLUS[i], reg_pos, 0);
    map = psyco_owner_alloc(offsetof(struct locals_map_s, items) +
                            (b.count + 1) * sizeof(long));
    map->lasti = lasti;
    map->nvalues = n;
    /* after the pushes: positions are offsets down from the stack top */
    map->below = po->stack_depth - INITIAL_STACK_DEPTH;
    for (i = 0; i < b.count; i += 3) {
        map->items[i] = b.items[i];
        map->items[i + 1] = b.items[i] == LM_STACK
                                ? po->stack_depth - b.items[i + 1]
                                : b.items[i + 1];
        map->items[i + 2] = b.items[i + 2];
    }
#if NI_CODESTATS
    psyco_cs_map(map, &map->lasti, (char *)(map->items + b.count) -
                                       (char *)&map->lasti);
#endif
    return map;
}

DEFINEFN
void
psyco_inline_exit(PsycoObject *po)
{
    BEGIN_CODE
    RETURNED_FROM_SUBFUNCTION();
    END_CODE
}

/*****************************************************************/
/***   Emit common instructions                                ***/

/* forward */
static condition_code_t
int_cmp_i(PsycoObject *po, vinfo_t *rt1, long immed2, int py_op);

DEFINEFN
condition_code_t
integer_non_null(PsycoObject *po, vinfo_t *vi)
{
    condition_code_t result;

    if (is_virtualtime(vi->source)) {
        result = psyco_vsource_cc(vi->source);
        if (result != CC_ALWAYS_FALSE)
            return result;
        if (!compute_vinfo(vi, po))
            return CC_ERROR;
    }
    if (is_compiletime(vi->source)) {
        if (CompileTime_Get(vi->source)->value != 0)
            return CC_ALWAYS_TRUE;
        else
            return CC_ALWAYS_FALSE;
    }
    BEGIN_CODE
    CHECK_NONZERO_FROM_RT(vi->source, result);
    END_CODE
    return result;
}

DEFINEFN
condition_code_t
integer_NON_NULL(PsycoObject *po, vinfo_t *vi)
{
    condition_code_t result;

    if (vi == NULL)
        return CC_ERROR;

    result = integer_non_null(po, vi);

    /* 'vi' cannot be a reference to a Python object if we are
       asking ourselves if it is NULL or not. So the following
       vinfo_decref() will not emit a Py_DECREF() that would
       clobber the condition code. We check all this. */
#if ALL_CHECKS
    extra_assert(!has_rtref(vi->source));
    {
        code_t *code1 = po->code;
#endif
        vinfo_decref(vi, po);
#if ALL_CHECKS
        extra_assert(po->code == code1);
    }
#endif
    return result;
}

#define GENERIC_BINARY_HEADER                             \
    if (!compute_vinfo(v2, po) || !compute_vinfo(v1, po)) \
    return NULL

#define GENERIC_BINARY_HEADER_i \
    if (!compute_vinfo(v1, po)) \
    return NULL

#define GENERIC_BINARY_CT_CT(c_code)                                \
    if (is_compiletime(v1->source) && is_compiletime(v2->source)) { \
        long a = CompileTime_Get(v1->source)->value;                \
        long b = CompileTime_Get(v2->source)->value;                \
        long c = (c_code);                                          \
        return vinfo_new(CompileTime_New(c));                       \
    }

DEFINEFN
vinfo_t *
integer_add(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, bool ovf)
{
    GENERIC_BINARY_HEADER;
    if (is_compiletime(v1->source)) {
        long a = CompileTime_Get(v1->source)->value;
        if (a == 0) {
            /* adding zero to v2 */
            vinfo_incref(v2);
            return v2;
        }
        if (is_compiletime(v2->source)) {
            long b = CompileTime_Get(v2->source)->value;
            long c = a + b;
            if (ovf && (c ^ a) < 0 && (c ^ b) < 0)
                return NULL; /* overflow */
            return vinfo_new(CompileTime_New(c));
        }
        if (!ovf)
            return bint_add_i(po, v2, a, false);
    }
    else if (is_compiletime(v2->source)) {
        long b = CompileTime_Get(v2->source)->value;
        if (b == 0) {
            /* adding zero to v1 */
            vinfo_incref(v1);
            return v1;
        }
        if (!ovf)
            return bint_add_i(po, v1, b, false);
    }
    return BINARY_INSTR_ADD(
        ovf, ovf && is_nonneg(v1->source) && is_nonneg(v2->source));
}

DEFINEFN
vinfo_t *
integer_add_i(PsycoObject *po, vinfo_t *v1, long value2, bool unsafe)
{
    if (value2 == 0) {
        /* adding zero to v1 */
        vinfo_incref(v1);
        return v1;
    }
    else {
        GENERIC_BINARY_HEADER_i;
        if (is_compiletime(v1->source)) {
            long c = CompileTime_Get(v1->source)->value + value2;
            return vinfo_new(CompileTime_New(c));
        }
        return bint_add_i(po, v1, value2, unsafe);
    }
}

DEFINEFN
vinfo_t *
integer_sub(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, bool ovf)
{
    GENERIC_BINARY_HEADER;
    if (is_compiletime(v1->source)) {
        long a = CompileTime_Get(v1->source)->value;
        if (is_compiletime(v2->source)) {
            long b = CompileTime_Get(v2->source)->value;
            long c = a - b;
            if (ovf && (c ^ a) < 0 && (c ^ ~b) < 0)
                return NULL; /* overflow */
            return vinfo_new(CompileTime_New(c));
        }
    }
    else if (is_compiletime(v2->source)) {
        long b = CompileTime_Get(v2->source)->value;
        if (b == 0) {
            /* subtracting zero from v1 */
            vinfo_incref(v1);
            return v1;
        }
        if (!ovf)
            return bint_add_i(po, v1, -b, false);
    }
    return BINARY_INSTR_SUB(ovf, false);
}

DEFINEFN
vinfo_t *
integer_or(PsycoObject *po, vinfo_t *v1, vinfo_t *v2)
{
    GENERIC_BINARY_HEADER;
    GENERIC_BINARY_CT_CT(a | b);
    return BINARY_INSTR_OR(false,
                           is_nonneg(v1->source) && is_nonneg(v2->source));
}

DEFINEFN
vinfo_t *
integer_xor(PsycoObject *po, vinfo_t *v1, vinfo_t *v2)
{
    GENERIC_BINARY_HEADER;
    GENERIC_BINARY_CT_CT(a ^ b);
    return BINARY_INSTR_XOR(false,
                            is_nonneg(v1->source) && is_nonneg(v2->source));
}

DEFINEFN
vinfo_t *
integer_and(PsycoObject *po, vinfo_t *v1, vinfo_t *v2)
{
    GENERIC_BINARY_HEADER;
    GENERIC_BINARY_CT_CT(a & b);
    return BINARY_INSTR_AND(false,
                            is_nonneg(v1->source) || is_nonneg(v2->source));
}

static vinfo_t *
int_mul_i(PsycoObject *po, vinfo_t *v1, long value2, bool ovf)
{
    switch (value2) {
        case 0:
            return vinfo_new(CompileTime_New(0));
        case 1:
            vinfo_incref(v1);
            return v1;
    }
    if (((value2 - 1) & value2) == 0 && value2 >= 0 && !ovf) {
        /* value2 is a power of two */
        return bint_lshift_i(po, v1, intlog2(value2));
    }
    else {
        return bint_mul_i(po, v1, value2, ovf);
    }
}

DEFINEFN
vinfo_t *
integer_mul(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, bool ovf)
{
    GENERIC_BINARY_HEADER;
    if (is_compiletime(v1->source)) {
        long a = CompileTime_Get(v1->source)->value;
        if (is_compiletime(v2->source)) {
            long b = CompileTime_Get(v2->source)->value;
            /* unlike Python, we use a function written in assembly
               to perform the product overflow checking */
            if (ovf && psyco_int_mul_ovf(a, b))
                return NULL; /* overflow */
            return vinfo_new(CompileTime_New(a * b));
        }
        return int_mul_i(po, v2, a, ovf);
    }
    else if (is_compiletime(v2->source)) {
        long b = CompileTime_Get(v2->source)->value;
        return int_mul_i(po, v1, b, ovf);
    }
    return BINARY_INSTR_MUL(
        ovf, ovf && is_rtnonneg(v1->source) && is_rtnonneg(v2->source));
}

DEFINEFN
vinfo_t *
integer_mul_i(PsycoObject *po, vinfo_t *v1, long value2)
{
    GENERIC_BINARY_HEADER_i;
    if (is_compiletime(v1->source)) {
        long c = CompileTime_Get(v1->source)->value * value2;
        return vinfo_new(CompileTime_New(c));
    }
    return int_mul_i(po, v1, value2, false);
}

DEFINEFN
vinfo_t *
integer_lshift(PsycoObject *po, vinfo_t *v1, vinfo_t *v2)
{
    condition_code_t cc;
    GENERIC_BINARY_HEADER;
    if (is_compiletime(v2->source))
        return integer_lshift_i(po, v1, CompileTime_Get(v2->source)->value);

    cc = int_cmp_i(po, v2, LONG_BIT, Py_GE | COMPARE_UNSIGNED);
    if (cc == CC_ERROR)
        return NULL;

    if (runtime_condition_f(po, cc)) {
        cc = int_cmp_i(po, v2, 0, Py_LT);
        if (cc == CC_ERROR)
            return NULL;
        if (runtime_condition_f(po, cc)) {
            PycException_SetString(po, PyExc_ValueError,
                                   "negative shift count");
            return NULL;
        }
        return vinfo_new(CompileTime_New(0));
    }
    return BINARY_INSTR_LSHIFT(false);
}

DEFINEFN
vinfo_t *
integer_lshift_i(PsycoObject *po, vinfo_t *v1, long counter)
{
    GENERIC_BINARY_HEADER_i;
    if (0 < counter && counter < LONG_BIT) {
        if (is_compiletime(v1->source)) {
            long c = CompileTime_Get(v1->source)->value << counter;
            return vinfo_new(CompileTime_New(c));
        }
        else
            return bint_lshift_i(po, v1, counter);
    }
    else if (counter == 0) {
        vinfo_incref(v1);
        return v1;
    }
    else if (counter >= LONG_BIT)
        return vinfo_new(CompileTime_New(0));
    else {
        PycException_SetString(po, PyExc_ValueError, "negative shift count");
        return NULL;
    }
}

DEFINEFN /* signed */
    vinfo_t *
    integer_rshift(PsycoObject *po, vinfo_t *v1, vinfo_t *v2)
{
    condition_code_t cc;
    GENERIC_BINARY_HEADER;
    if (is_compiletime(v2->source))
        return integer_rshift_i(po, v1, CompileTime_Get(v2->source)->value);

    cc = int_cmp_i(po, v2, LONG_BIT, Py_GE | COMPARE_UNSIGNED);
    if (cc == CC_ERROR)
        return NULL;

    if (runtime_condition_f(po, cc)) {
        cc = int_cmp_i(po, v2, 0, Py_LT);
        if (cc == CC_ERROR)
            return NULL;
        if (runtime_condition_f(po, cc)) {
            PycException_SetString(po, PyExc_ValueError,
                                   "negative shift count");
            return NULL;
        }
        return integer_rshift_i(po, v1, LONG_BIT - 1);
    }
    return BINARY_INSTR_RSHIFT(is_nonneg(v1->source));
}

DEFINEFN /* signed */
    vinfo_t *
    integer_rshift_i(PsycoObject *po, vinfo_t *v1, long counter)
{
    GENERIC_BINARY_HEADER_i;
    if (counter >= LONG_BIT - 1) {
        if (is_nonneg(v1->source))
            return vinfo_new(CompileTime_New(0));
        counter = LONG_BIT - 1;
    }
    if (counter > 0) {
        if (is_compiletime(v1->source)) {
            long c = ((long)(CompileTime_Get(v1->source)->value)) >> counter;
            return vinfo_new(CompileTime_New(c));
        }
        else
            return bint_rshift_i(po, v1, counter);
    }
    else if (counter == 0) {
        vinfo_incref(v1);
        return v1;
    }
    else {
        PycException_SetString(po, PyExc_ValueError, "negative shift count");
        return NULL;
    }
}

DEFINEFN
vinfo_t *
integer_urshift_i(PsycoObject *po, vinfo_t *v1, long counter)
{
    GENERIC_BINARY_HEADER_i;
    if (0 < counter && counter < LONG_BIT) {
        if (is_compiletime(v1->source)) {
            long c = ((unsigned long)(CompileTime_Get(v1->source)->value)) >>
                     counter;
            return vinfo_new(CompileTime_New(c));
        }
        else
            return bint_urshift_i(po, v1, counter);
    }
    else if (counter == 0) {
        vinfo_incref(v1);
        return v1;
    }
    else if (counter >= LONG_BIT)
        return vinfo_new(CompileTime_New(0));
    else {
        PycException_SetString(po, PyExc_ValueError, "negative shift count");
        return NULL;
    }
}

/* DEFINEFN */
/* vinfo_t* integer_lshift(PsycoObject* po, vinfo_t* v1, vinfo_t* v2) */
/* { */
/*   NonVirtualSource v1s, v2s; */
/*   v2s = vinfo_compute(v2, po); */
/*   if (v2s == SOURCE_ERROR) return NULL; */
/*   if (is_compiletime(v2s)) */
/*     return integer_lshift_i(po, v1, CompileTime_Get(v2s)->value); */

/*   v1s = vinfo_compute(v1, po); */
/*   if (v1s == SOURCE_ERROR) return NULL; */
/*   XXX implement me */
/* } */

#define GENERIC_UNARY_HEADER    \
    if (!compute_vinfo(v1, po)) \
        return NULL;

#define GENERIC_UNARY_CT(c_code, ovf, c_ovf)         \
    if (is_compiletime(v1->source)) {                \
        long a = CompileTime_Get(v1->source)->value; \
        long c = (c_code);                           \
        if ((ovf) && (c_ovf))                        \
            return NULL;                             \
        return vinfo_new(CompileTime_New(c));        \
    }

DEFINEFN
vinfo_t *
integer_inv(PsycoObject *po, vinfo_t *v1)
{
    GENERIC_UNARY_HEADER;
    GENERIC_UNARY_CT(~a, false, false);
    return UNARY_INSTR_INV(false, false);
}

DEFINEFN
vinfo_t *
integer_neg(PsycoObject *po, vinfo_t *v1, bool ovf)
{
    GENERIC_UNARY_HEADER;
    GENERIC_UNARY_CT(-a, ovf, c == (-LONG_MAX - 1));
    return UNARY_INSTR_NEG(ovf, false);
}

DEFINEFN
vinfo_t *
integer_abs(PsycoObject *po, vinfo_t *v1, bool ovf)
{
    GENERIC_UNARY_HEADER;
    if (is_nonneg(v1->source)) {
        vinfo_incref(v1);
        return v1;
    }
    GENERIC_UNARY_CT(a < 0 ? -a : a, ovf, c == (-LONG_MAX - 1));
    return UNARY_INSTR_ABS(ovf, true); /* nb. result assumed to be positive,
                                          which is false in case of overflow */
}

static condition_code_t
immediate_compare(long a, long b, int base_py_op)
{
    switch (base_py_op) {
        case Py_LT:
            return a < b ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
        case Py_LE:
            return a <= b ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
        case Py_EQ | COMPARE_UNSIGNED:
        case Py_EQ:
            return a == b ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
        case Py_NE | COMPARE_UNSIGNED:
        case Py_NE:
            return a != b ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
        case Py_GT:
            return a > b ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
        case Py_GE:
            return a >= b ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;

        case Py_LT | COMPARE_UNSIGNED:
            return ((unsigned long)a) < ((unsigned long)b) ? CC_ALWAYS_TRUE
                                                           : CC_ALWAYS_FALSE;
        case Py_LE | COMPARE_UNSIGNED:
            return ((unsigned long)a) <= ((unsigned long)b) ? CC_ALWAYS_TRUE
                                                            : CC_ALWAYS_FALSE;
        case Py_GT | COMPARE_UNSIGNED:
            return ((unsigned long)a) > ((unsigned long)b) ? CC_ALWAYS_TRUE
                                                           : CC_ALWAYS_FALSE;
        case Py_GE | COMPARE_UNSIGNED:
            return ((unsigned long)a) >= ((unsigned long)b) ? CC_ALWAYS_TRUE
                                                            : CC_ALWAYS_FALSE;
        default:
            Py_FatalError("immediate_compare(): bad py_op");
            return CC_ERROR;
    }
}

static condition_code_t
int_cmp_i(PsycoObject *po, vinfo_t *rt1, long immed2, int py_op)
{
    int base_py_op = py_op & COMPARE_OP_MASK;
    extra_assert(is_runtime(rt1->source));
    /* detect easy cases */
    if (immed2 == 0) {
        if (is_rtnonneg(rt1->source)) /* if we know that rt1>=0 */
            switch (base_py_op) {
                case Py_LT: /* rt1 < 0 */
                    return CC_ALWAYS_FALSE;
                case Py_GE: /* rt1 >= 0 */
                    return CC_ALWAYS_TRUE;
                default:; /* pass */
            }
        switch (base_py_op) {
            case Py_LT | COMPARE_UNSIGNED: /* (unsigned) rt1 < 0 */
                return CC_ALWAYS_FALSE;
            case Py_GE | COMPARE_UNSIGNED: /* (unsigned) rt1 >= 0 */
                return CC_ALWAYS_TRUE;
            default:; /* pass */
        }
    }
    else if (immed2 < 0) {
        if (is_rtnonneg(rt1->source)) /* rt1>=0 && immed2<0 */
        {
            switch (base_py_op) {
                case Py_EQ:                    /* rt1 == immed2 */
                case Py_LT:                    /* rt1 <  immed2 */
                case Py_LE:                    /* rt1 <= immed2 */
                case Py_GT | COMPARE_UNSIGNED: /* (unsigned) rt1 >  immed2 */
                case Py_GE | COMPARE_UNSIGNED: /* (unsigned) rt1 >= immed2 */
                    return CC_ALWAYS_FALSE;
                case Py_NE:                    /* rt1 != immed2 */
                case Py_GE:                    /* rt1 >= immed2 */
                case Py_GT:                    /* rt1 >  immed2 */
                case Py_LE | COMPARE_UNSIGNED: /* (unsigned) rt1 <= immed2 */
                case Py_LT | COMPARE_UNSIGNED: /* (unsigned) rt1 <  immed2 */
                    return CC_ALWAYS_TRUE;
                default:; /* pass */
            }
        }
        if (immed2 == LONG_MIN) {
            switch (base_py_op) {
                case Py_LT: /* rt1 <  LONG_MIN */
                    return CC_ALWAYS_FALSE;
                case Py_GE: /* rt1 >= LONG_MIN */
                    return CC_ALWAYS_TRUE;
                case Py_EQ: /* rt1 == LONG_MIN */
                case Py_LE: /* rt1 <= LONG_MIN */
                    if (py_op & CHEAT_MAXINT)
                        return CC_ALWAYS_FALSE;
                    break;
                case Py_NE: /* rt1 != LONG_MIN */
                case Py_GT: /* rt1 >  LONG_MIN */
                    if (py_op & CHEAT_MAXINT)
                        return CC_ALWAYS_TRUE;
                    break;
                default:; /* pass */
            }
        }
    }
    else if (immed2 == LONG_MAX) {
        if (is_rtnonneg(rt1->source)) /* if we know that rt1>=0 */
        {
            switch (base_py_op) {
                case Py_GT | COMPARE_UNSIGNED: /* (unsigned) rt1 >  LONG_MAX */
                    return CC_ALWAYS_FALSE;
                case Py_LE | COMPARE_UNSIGNED: /* (unsigned) rt1 <= LONG_MAX */
                    return CC_ALWAYS_TRUE;
                case Py_GE | COMPARE_UNSIGNED: /* (unsigned) rt1 >= LONG_MAX */
                    if (py_op & CHEAT_MAXINT)
                        return CC_ALWAYS_FALSE;
                    break;
                case Py_LT | COMPARE_UNSIGNED: /* (unsigned) rt1 <  LONG_MAX */
                    if (py_op & CHEAT_MAXINT)
                        return CC_ALWAYS_TRUE;
                    break;
                default:; /* pass */
            }
        }
        switch (base_py_op) {
            case Py_LE: /* rt1 <= LONG_MAX */
                return CC_ALWAYS_TRUE;
            case Py_GT: /* rt1 >  LONG_MAX */
                return CC_ALWAYS_FALSE;
            case Py_EQ: /* rt1 == LONG_MAX */
            case Py_GE: /* rt1 >= LONG_MAX */
                if (py_op & CHEAT_MAXINT)
                    return CC_ALWAYS_FALSE;
                break;
            case Py_NE: /* rt1 != LONG_MAX */
            case Py_LT: /* rt1 <  LONG_MAX */
                if (py_op & CHEAT_MAXINT)
                    return CC_ALWAYS_TRUE;
                break;
            default:; /* pass */
        }
    }
    /* end of easy cases */
    return bint_cmp_i(po, base_py_op, rt1, immed2);
}

static const int inverted_py_op[8] = {
    /* Py_LT: */ Py_GT,
    /* Py_LE: */ Py_GE,
    /* Py_EQ: */ Py_EQ,
    /* Py_NE: */ Py_NE,
    /* Py_GT: */ Py_LT,
    /* Py_GE: */ Py_LE,
    /* (6)    */ -1,
    /* (7)    */ -1};

DEFINEFN
condition_code_t
integer_cmp(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, int py_op)
{
    int base_py_op = py_op & COMPARE_OP_MASK;

    if (vinfo_known_equal(v1, v2))
        goto same_source;

    if (!compute_vinfo(v1, po) || !compute_vinfo(v2, po))
        return CC_ERROR;

    if (vinfo_known_equal(v1, v2)) {
    same_source:
        /* comparing equal sources, signed or unsigned */
        switch (py_op & COMPARE_BASE_MASK) {
            case Py_LE:
            case Py_EQ:
            case Py_GE:
                return CC_ALWAYS_TRUE;
            default:
                return CC_ALWAYS_FALSE;
        }
    }
    if (is_compiletime(v1->source))
        if (is_compiletime(v2->source)) {
            long a = CompileTime_Get(v1->source)->value;
            long b = CompileTime_Get(v2->source)->value;
            return immediate_compare(a, b, base_py_op);
        }
        else {
            /* invert the two operands because (assumedly) the processor has
               only CMP xxx,immed and not CMP immed,xxx */
            py_op = inverted_py_op[py_op & COMPARE_BASE_MASK] |
                    (py_op & ~COMPARE_BASE_MASK);
            return int_cmp_i(po, v2, CompileTime_Get(v1->source)->value,
                             py_op);
        }
    else if (is_compiletime(v2->source)) {
        return int_cmp_i(po, v1, CompileTime_Get(v2->source)->value, py_op);
    }
    else {
        return BINARY_INSTR_CMP(base_py_op);
    }
}

DEFINEFN
condition_code_t
integer_cmp_i(PsycoObject *po, vinfo_t *v1, long value2, int py_op)
{
    /* a condition still in the flags is 0 or 1: compared with 0 it is the
       condition itself, as in integer_non_null(). Without this, testing a
       bool made by a comparison (plong_bool) wrote the flags into a
       register with SETcc and compared that again. */
    if (value2 == 0 && (py_op == Py_NE || py_op == Py_EQ)) {
        condition_code_t cc = psyco_vsource_cc(v1->source);
        if (cc != CC_ALWAYS_FALSE)
            return py_op == Py_NE ? cc : INVERT_CC(cc);
    }
    if (!compute_vinfo(v1, po))
        return CC_ERROR;

    if (is_compiletime(v1->source)) {
        long a = CompileTime_Get(v1->source)->value;
        return immediate_compare(a, value2, py_op);
    }
    else
        return int_cmp_i(po, v1, value2, py_op);
}

DEFINEFN
vinfo_t *
integer_conditional(PsycoObject *po, condition_code_t cc, long immed_true,
                    long immed_false)
{
    switch ((int)cc) {
        case (int)CC_ALWAYS_FALSE:
            return vinfo_new(CompileTime_New(immed_false));

        case (int)CC_ALWAYS_TRUE:
            return vinfo_new(CompileTime_New(immed_true));

        default:
            return BINARY_INSTR_COND(cc, immed_true, immed_false);
    }
}

INITIALIZATIONFN
void
psyco_codegen_init(void)
{
#if HAVE_CCREG
    int i;
    for (i = 0; i < CC_TOTAL; i++) {
        /* the condition codes cannot be passed across function calls */
        INIT_SVIRTUAL_NOCALL(cc_functions_table[i], generic_computed_cc, 0);
    }
#endif

    psyco_nonfixed_promotion.header.compute_fn = &computed_promotion;
    psyco_nonfixed_promotion.pflags = 0;

    psyco_nonfixed_pyobj_promotion.header.compute_fn = &computed_promotion;
    psyco_nonfixed_pyobj_promotion.pflags = PFlagPyObj;

    /*psyco_nonfixed_promotion_mega.header.compute_fn = &computed_promotion;
      psyco_nonfixed_promotion_mega.pflags = PFlagMegamorphic;*/

    psyco_nonfixed_pyobj_promotion_mega.header.compute_fn =
        &computed_promotion;
    psyco_nonfixed_pyobj_promotion_mega.pflags = PFlagPyObj | PFlagMegamorphic;

    psyco_static_type_promotion.header.compute_fn = &computed_promotion;
    psyco_static_type_promotion.pflags =
        PFlagPyObj | PFlagMegamorphic | PFlagStaticType;

    psyco_interp_pyobj_promotion.header.compute_fn = &computed_promotion;
    psyco_interp_pyobj_promotion.pflags =
        PFlagPyObj | PFlagMegamorphic | PFlagInterpret;
}
