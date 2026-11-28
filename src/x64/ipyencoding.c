#include "ipyencoding.h"
#include "../pycodegen.h"
#include "../codemanager.h"
/* LE_GC_UNTRACKED_TEST */
#include "../mechanisms/store_gc.h"
#if LE_HAS_FRAME_STACKREFS
/* Py_TAG_REFCNT, PyStackRef_NULL */
#include "../mechanisms/frames.h"
#endif
/* PyLazyImport_Type */
#include "../Python/pycinternal.h"

/* Emitted code's view of the shape of dict keys (pobject.h:le_dict_shape):
   a compare of the keys at 'rk' with an immediate, which ends the
   instruction (shape_imm_t), or a load */
#if LE_HAS_DICT_KEYS_KIND
#define KEYS_SHAPE_CMP(shape, rk) \
    CMP_I16_WO(shape, rk, offsetof(le_dictkeys_t, dk_log2_index_bytes))
#define KEYS_SHAPE_LOAD(r, rk) \
    MOVZX_DR_WO(r, rk, offsetof(le_dictkeys_t, dk_log2_index_bytes))
#else
#define KEYS_SHAPE_CMP(shape, rk) \
    CMP_I32_O(shape, rk, offsetof(le_dictkeys_t, dk_size))
#define KEYS_SHAPE_LOAD(r, rk) MOV_R_O(r, rk, offsetof(le_dictkeys_t, dk_size))
#endif

DEFINEFN void
function_return(PsycoObject *po, Source retval, int nframelocal, long retpos)
{
    int post_return_stack_depth = 0;
    int pre_return_stack_correction = 0;
    int return_depth = sizeof(long) + INITIAL_STACK_DEPTH;
    BEGIN_CODE
    /* load the return value into EAX for regular functions, EBX for functions
     * with a prologue */
    if (retval != SOURCE_DUMMY) {
        reg_t rg =
            nframelocal > 0 ? REG_ANY_CALLEE_SAVED : REG_FUNCTIONS_RETURN;
        LOAD_REG_FROM(retval, rg);
    }

    if (nframelocal > 0) {
        /* psyco_emit_header() was used; first clear the stack only up to and
         * not including the frame-local data */
        int framelocpos = getstack(LOC_CONTINUATION->array->items[0]->source);
        STACK_CORRECTION(framelocpos - po->stack_depth);
        po->stack_depth = framelocpos;
        MOV_R_R(REG_FUNCTIONS_RETURN, REG_ANY_CALLEE_SAVED);
    }

    pre_return_stack_correction = retpos - po->stack_depth;
    STACK_CORRECTION(pre_return_stack_correction);
    po->stack_depth += pre_return_stack_correction;
    post_return_stack_depth = po->stack_depth - return_depth;
    if (post_return_stack_depth >= 0x8000) {
        POP_R(REG_TRANSIENT_1);
        STACK_CORRECTION(-post_return_stack_depth);
        PUSH_R(REG_TRANSIENT_1);
        po->stack_depth = return_depth;
        RET();
    }
    else {
        RET_N(post_return_stack_depth);
    }
    po->stack_depth = 0;

    END_CODE
}

#if LE_HAS_DEALLOC_HOOKS
/* Python/pyruntime.c */
extern void *
le_ref_tracer_word(void);
#endif

/* the live XMM registers stored below RSP and loaded back, around a call
   on a path that must leave the state as it was; 'area' is the bytes used */
static int
xmm_live_count(PsycoObject *po)
{
    reg_t rg;
    int n = 0;
    for (rg = REG_X64_XMM8; rg <= REG_X64_XMM14; rg++)
        n += REG_NUMBER(po, rg) != NULL;
    return n;
}
static code_t *
xmm_save_live(PsycoObject *po, code_t *code, int *area)
{
    reg_t rg;
    int n = 0;
    *area = 8 * xmm_live_count(po);
    SUB_R_I8(REG_X64_RSP, *area);
    po->stack_depth += *area;
    n = 0;
    for (rg = REG_X64_XMM8; rg <= REG_X64_XMM14; rg++)
        if (REG_NUMBER(po, rg) != NULL) {
            MOVSD_O_XMM(REG_X64_RSP, 8 * n, XMM_HW(rg));
            n++;
        }
    return code;
}
static code_t *
xmm_restore_live(PsycoObject *po, code_t *code, int area)
{
    reg_t rg;
    int n = 0;
    extra_assert(8 * xmm_live_count(po) == area);
#if ALL_CHECKS
    code = psyco_clobber_xmm(code); /* as the C code may */
#endif
    for (rg = REG_X64_XMM8; rg <= REG_X64_XMM14; rg++)
        if (REG_NUMBER(po, rg) != NULL) {
            MOVSD_XMM_O(XMM_HW(rg), REG_X64_RSP, 8 * n);
            n++;
        }
    ADD_R_I8(REG_X64_RSP, area);
    po->stack_depth -= area;
    return code;
}

#if LE_HAS_DEALLOC_DEFERS_TO_TRASHCAN
/* Python/pyruntime.c */
extern bool
le_trashcan_words(long *tstate_at, long *soft_at, long *chain_at, long *room);

_Static_assert(Py_TPFLAGS_HAVE_GC >> 8 < 256 &&
                   (Py_TPFLAGS_HAVE_GC & 255) == 0,
               "the GC flag is tested in tp_flags' second byte");

/* a forward 8-bit jump, on 'cc' or always (CC_ALWAYS_TRUE), whose end is
   put in '*end' for short_jump_land */
static code_t *
short_jump(code_t *code, condition_code_t cc, code_t **end)
{
    code[0] = cc == CC_ALWAYS_TRUE ? 0xEB : 0x70 | cc;
    *end = code + 2;
    return code + 2;
}
static void
short_jump_land(code_t *code, code_t *end)
{
    long amount = (long)code - (long)end;
    extra_assert(0 <= amount && amount < 128);
    if (end[-2] == 0xEB)
        le_trace_jump(end - 2, code);
    else
        le_trace_jump_cond(end - 2, end, code);
    end[-1] = (code_t)amount;
}

/* Frees the object in RDI, at a stack aligned for a call, as _Py_Dealloc
   would, given the ref tracer's word: 'fn' is the tp_dealloc of its type,
   one the GC tracks, or NULL for a type known only at run time. With no
   tracer set, a type the GC doesn't track is freed with tp_dealloc; one it
   tracks, where _Py_Dealloc's trashcan test is known to be the one made
   here (Python/pyruntime.c:le_trashcan_words), is freed with tp_dealloc
   while the margin is 4 or more, and a delete_later chain left is then
   emptied, as _Py_Dealloc does. The margin is measured at the call, a few
   bytes above where _Py_Dealloc measures it. Everything else calls
   _Py_Dealloc. Changes R10, R11 and what a C call changes. */
DEFINEFN
code_t *
dealloc_testing_margin(code_t *code, PsycoObject *po, destructor fn,
                       void *tracer)
{
    long tstate_at, soft_at, chain_at, room;
    bool margin = le_trashcan_words(&tstate_at, &soft_at, &chain_at, &room);
    reg_t tp = REG_TRANSIENT_1, ts = REG_TRANSIENT_2;
    code_t *traced, *by_flag = NULL, *low = NULL, *no_chain = NULL,
                    *chained = NULL, *untracked = NULL;
    extra_assert(fn == NULL || margin);
    MOV_R_I(ts, (long)tracer);
    CMP_I8_A(0, ts);
    code = short_jump(code, CC_NE, &traced);
    if (fn == NULL) {
        /* without the test here, a type the GC tracks is _Py_Dealloc's */
        MOV_R_O8(tp, REG_X64_RDI, CPY_OFFSET(PyObject, ob_type));
        TEST_I8_BO(tp, CPY_OFFSET(PyTypeObject, tp_flags) + 1,
                   Py_TPFLAGS_HAVE_GC >> 8);
        code = short_jump(code, margin ? CC_E : CC_NE, &by_flag);
    }
    if (margin) {
        /* below a margin of 4: the C stack pointer under the soft limit
           plus 'room' */
        MOV_R_FS(ts, tstate_at);
        MOV_R_O(ts, ts, soft_at);
        ADD_R_I32(ts, room);
        CMP_R_R(ts, REG_X64_RSP);
        code = short_jump(code, CC_B, &low);
        if (fn == NULL) {
            MOV_R_O8(tp, tp, CPY_OFFSET(PyTypeObject, tp_dealloc));
            CALL_R(tp);
        }
        else
            CALL_I(fn);
        MOV_R_FS(ts, tstate_at);
        CMP_I8_O(0, ts, chain_at);
        code = short_jump(code, CC_E, &no_chain);
        MOV_R_R(REG_X64_RDI, ts);
        CALL_I(_PyTrash_thread_destroy_chain);
        code = short_jump(code, CC_ALWAYS_TRUE, &chained);
    }
    if (fn == NULL) {
        if (margin)
            short_jump_land(code, by_flag);
        MOV_R_O8(tp, tp, CPY_OFFSET(PyTypeObject, tp_dealloc));
        CALL_R(tp);
        code = short_jump(code, CC_ALWAYS_TRUE, &untracked);
    }
    short_jump_land(code, traced);
    if (by_flag != NULL && !margin)
        short_jump_land(code, by_flag);
    if (low != NULL)
        short_jump_land(code, low);
    CALL_I(_Py_Dealloc);
    if (no_chain != NULL) {
        short_jump_land(code, no_chain);
        short_jump_land(code, chained);
    }
    if (untracked != NULL)
        short_jump_land(code, untracked);
    return code;
}
#endif

/* What a decref of an object of 'type' (NULL where it is known only at run
   time) calls at 0: '*fn', or NULL for its type's, read at run time; with a
   ref tracer that _Py_Dealloc reports to, '*tracer' is its word, which is
   compared with 0 before each call, and _Py_Dealloc is called instead
   while one is set. Returns whether _Py_Dealloc may defer the free to the
   trashcan, which dealloc_testing_margin tests. */
static bool
dealloc_choice(PyTypeObject *type, destructor *fn, void **tracer)
{
    bool trash = false;
    *fn = type != NULL ? type->tp_dealloc : NULL;
    *tracer = NULL;
#if defined(Py_TRACE_REFS)
    *fn = _Py_Dealloc;
#elif LE_HAS_DEALLOC_DEFERS_TO_TRASHCAN
    if (type == NULL || (type->tp_flags & Py_TPFLAGS_HAVE_GC)) {
        long unused;
        trash = type == NULL ||
                le_trashcan_words(&unused, &unused, &unused, &unused);
        if (!trash)
            *fn = _Py_Dealloc;
    }
#endif
#if LE_HAS_DEALLOC_HOOKS
    if (*fn != _Py_Dealloc && (*tracer = le_ref_tracer_word()) == NULL) {
        *fn = _Py_Dealloc;
        trash = false;
    }
#endif
    return trash;
}

DEFINEFN
bool
decref_dealloc_long(PsycoObject *po, PyTypeObject *type)
{
    destructor fn;
    void *tracer;
    return xmm_live_count(po) > 0 || dealloc_choice(type, &fn, &tracer);
}

DEFINEFN
code_t *
decref_dealloc_calling(code_t *code, PsycoObject *po, reg_t rg,
                       PyTypeObject *type)
{
    destructor fn;
    void *tracer;
    bool trash = dealloc_choice(type, &fn, &tracer);
    char xmm_call = po->xmm_call;
    int xmm_live = xmm_live_count(po), xmm_area = 0;
    extra_assert(CPY_OFFSET(PyObject, ob_type) < 128);
    extra_assert(CPY_OFFSET(PyTypeObject, tp_dealloc) < 128);
#if LE_HAS_IMMORTAL_OBJECTS
    /* as Py_DECREF, an immortal (its count's low 32 bits negative) is left
       alone */
    NEED_CC_REG(rg);
    CMP_I8_DA(0, rg);
    BEGIN_COND_JUMP_OF(1, CC_S, xmm_live > 0 || trash);
    LE_SUB_OB_REFCNT(rg);
#else
    DEC_OB_REFCNT_NZ(rg);
#endif
    /* NE is the not zero flag; saving XMM registers or the trashcan's test
       makes the path too long for an 8-bit jump */
    BEGIN_COND_JUMP_OF(0, CC_NE, xmm_live > 0 || trash);
    if (xmm_live > 0) {
        code = xmm_save_live(po, code, &xmm_area);
        po->xmm_call = XMM_CALL_KEPT;
    }
#if LE_HAS_DEALLOC_DEFERS_TO_TRASHCAN
    if (trash) {
        BEGIN_CALL(1);
        CALL_SET_ARG_FROM_REG(rg, 0);
        code = dealloc_testing_margin(code, po, fn, tracer);
        END_CALLS();
    }
    else
#endif
#if LE_HAS_DEALLOC_HOOKS
        if (tracer != NULL) {
        BEGIN_CALL(1);
        CALL_SET_ARG_FROM_REG(rg, 0);
        MOV_R_I(REG_TRANSIENT_1, (long)tracer);
        CMP_I8_A(0, REG_TRANSIENT_1);
        MOV_R_I(REG_TRANSIENT_1, (long)_Py_Dealloc); /* keeps the flags */
        BEGIN_SHORT_COND_JUMP(2, CC_NE);
        if (fn == NULL) {
            MOV_R_O8(REG_TRANSIENT_1, rg, CPY_OFFSET(PyObject, ob_type));
            MOV_R_O8(REG_TRANSIENT_1, REG_TRANSIENT_1,
                     CPY_OFFSET(PyTypeObject, tp_dealloc));
        }
        else
            MOV_R_I(REG_TRANSIENT_1, (long)fn);
        END_SHORT_JUMP(2);
        END_CALL_R(REG_TRANSIENT_1);
    }
    else
#endif
        if (fn == NULL) {
        BEGIN_CALL(1);
        CALL_SET_ARG_FROM_REG(rg, 0);
        MOV_R_O8(REG_TRANSIENT_1, rg, CPY_OFFSET(PyObject, ob_type));
        MOV_R_O8(REG_TRANSIENT_1, REG_TRANSIENT_1,
                 CPY_OFFSET(PyTypeObject, tp_dealloc));
        END_CALL_R(REG_TRANSIENT_1);
    }
    else {
        BEGIN_CALL(1);
        CALL_SET_ARG_FROM_REG(rg, 0);
        END_CALL_I(fn);
    }
    if (xmm_area > 0)
        code = xmm_restore_live(po, code, xmm_area);
    po->xmm_call = xmm_call;
    END_JUMP_OF(0);
#if LE_HAS_IMMORTAL_OBJECTS
    END_JUMP_OF(1);
#endif
    return code;
}

DEFINEFN
void
decref_create_new_ref(PsycoObject *po, vinfo_t *w)
{
    /* we must Py_INCREF() the object */
    BEGIN_CODE
    if (is_compiletime(w->source))
        INC_KNOWN_OB_REFCNT((PyObject *)CompileTime_Get(w->source)->value);
    else {
        /* 'w' is in a register because of write_array_item() */
        extra_assert(!RUNTIME_REG_IS_NONE(w));
        INC_OB_REFCNT(RUNTIME_REG(w));
    }
    END_CODE
}

DEFINEFN
bool
decref_create_new_lastref(PsycoObject *po, vinfo_t *w)
{
    bool could_eat = eat_reference(w);
    if (!could_eat) {
        /* in this case we must Py_INCREF() the object */
        BEGIN_CODE
        if (is_compiletime(w->source))
            INC_KNOWN_OB_REFCNT((PyObject *)CompileTime_Get(w->source)->value);
        else {
            /* 'w' is in a register because of write_array_item() */
            extra_assert(!RUNTIME_REG_IS_NONE(w));
            INC_OB_REFCNT(RUNTIME_REG(w));
        }
        END_CODE
    }
    return could_eat;
}

/* whether an inline load or store is into a managed dict's values */
#define IN_VALUES(s) (LE_MANAGED_VALUES && (s)->keys != NULL)

/* Inline attribute store (ipyencoding.h).
   Registers: RDI the object, RSI the value, RDX the cache, RCX
   the dict, R8 its values, R9 its keys then the index, RAX scratch then
   the old value, R10 and R11 scratch. A watched dict is a miss
   (le_dict_watched). A managed dict's values (s->keys) have no
   dict and no version tag: R8 is read from the object (less one, so the
   values are at R8 + 1, where LE_HAS_TAGGED_DICT_OR_VALUES), or is the
   object itself where the values are in it (LE_HAS_INLINE_VALUES), R9 is the
   class keys, and RCX is scratch. Laid out as

       checks ............ jcc MISS (each failure)
       old = values[ix]; test; jz FILL
     STORE:  values[ix] = value; incref value; new version tag, where
             dicts have one
             dec old (not an immortal); jnz DONE; call dealloc_glue;
             jmp DONE
     FILL:   ix == ma_used, else MISS; ma_used++; old = value; jmp STORE
       or into a managed dict's values: append ix to their insertion order
             (_PyDictValues_AddToInsertionOrder); old = value; jmp STORE
     MISS:   call miss_glue; test eax, eax
             (the caller's error check: jne <respawn>)
     DONE:

   FILL makes the old value the new one, increfed once more, so STORE's
   decref can't reach zero. FILL is only where filling an empty slot is
   likely (s->fill: stores to self in __init__); elsewhere an empty slot is
   a miss, which the helper stores as CPython does. A name the keys don't
   hold yet is always a miss, so the keys and their version are only ever
   changed by CPython. The checks
   come before any write, and nothing before the old value's decref runs Python
   code. The jumps to MISS are short where they reach: the body is emitted
   twice, first with every one near, which gives an upper bound on each
   distance. */
_Static_assert(Py_TPFLAGS_HAVE_GC == 1UL << 14,
               "the value's GC flag is tested in tp_flags' second byte");
_Static_assert(offsetof(store_cache_t, shape) == 0,
               "the shape check compares [cache] directly");
#define STORE_MAX_MISS_JUMPS 8
#if LE_HAS_IMMORTAL_OBJECTS
#define INC_STORED(rg) INC_OB_REFCNT_internal(rg)
#else
#define INC_STORED(rg) INC_A(rg)
#endif

/* Code emitted twice from 'start' (the attribute store and load, whose
   short jumps the first pass measures) decides each encoding from 'start'
   with a margin (LE_RIP_NEAR), and finds its constants' slots before the
   first pass, so that both passes emit the same lengths. */
#define SITE_CONSTS_MAX 6
typedef struct {
    long value[SITE_CONSTS_MAX];
    code_t *slot[SITE_CONSTS_MAX];
    int n;
} site_consts_t;

static void
site_const(site_consts_t *k, code_t *start, long value)
{
    extra_assert(k->n < SITE_CONSTS_MAX);
    k->value[k->n] = value;
    k->slot[k->n++] = FITS_IN_32BITS(value) || LE_RIP_NEAR(start, value)
                          ? NULL
                          : psyco_const_slot(start, value);
}

static void
site_consts_operand(site_consts_t *k, code_t *start, Source src)
{
    if (is_compiletime(src))
        site_const(k, start, KSOURCE_SOURCE(src)->value);
}

static code_t *
site_slot(const site_consts_t *k, long value)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (k->value[i] == value)
            return k->slot[i];
    return NULL;
}
/* the constant 'addr' in a register: 5 bytes below 4 GB, else 7
   (RIP-relative, or from its slot in 'consts') or 10 */
#define LOAD_ADDRESS(rg, addr)                                            \
    do {                                                                  \
        long _la = (long)(addr);                                          \
        code_t *_ls = site_slot(consts, _la);                             \
        if ((unsigned long)_la > 0xFFFFFFFFUL && LE_RIP_NEAR(start, _la)) \
            LEA_R_RIP(rg, _la - ((long)code + 7));                        \
        else if ((unsigned long)_la > 0xFFFFFFFFUL && _ls != NULL)        \
            MOV_R_RIP(rg, (long)_ls - ((long)code + 7));                  \
        else                                                              \
            MOV_R_IABS(rg, _la);                                          \
    } while (0)
/* a call to the glue 'fn' from code that starts at 'start'; with XMM
   values live, to its copy that keeps them */
#define CALL_GLUE(fn, start)                             \
    do {                                                 \
        code_t *_glue = (fn);                            \
        char _xmm_call = po->xmm_call;                   \
        if (psyco_xmm_live(po)) {                        \
            _glue = psyco_glue_keeping_xmm(_glue);       \
            po->xmm_call = XMM_CALL_KEPT;                \
        }                                                \
        if (LE_RIP_NEAR(start, _glue)) {                 \
            le_trace_call((code_t *)code, _glue);        \
            WRITE_1(0xE8);                               \
            WRITE_32BIT((long)_glue - ((long)code + 4)); \
            le_reloc_note(code - 4, LE_RELOC_JUMP, 4);   \
        }                                                \
        else {                                           \
            MOV_R_IABS(REG_TRANSIENT_1, (long)_glue);    \
            CALL_R(REG_TRANSIENT_1);                     \
        }                                                \
        po->xmm_call = _xmm_call;                        \
    } while (0)
static void
load_operand(PsycoObject *po, code_t **pcode, Source src, reg_t rg,
             code_t *start, const site_consts_t *consts)
{
    code_t *code = *pcode;
    if (is_compiletime(src)) {
        LOAD_ADDRESS(rg, KSOURCE_SOURCE(src)->value);
    }
    else {
        LOAD_REG_FROM(src, rg);
    }
    *pcode = code;
}

/* the dict in RCX gets a new version tag (store_cache_t.version, through
   the cache in RDX), where dicts have one */
#if !LE_HAS_DICT_VERSION_TAG
#define DICT_NEW_VERSION() ((void)0)
#elif LE_HAS_DICT_WATCHER_BITS
#define DICT_NEW_VERSION()                                             \
    do {                                                               \
        MOV_R_O(REG_TRANSIENT_1, REG_X64_RDX,                          \
                offsetof(store_cache_t, version));                     \
        MOV_R_A(REG_TRANSIENT_2, REG_TRANSIENT_1);                     \
        ADD_R_I32(REG_TRANSIENT_2, LE_DICT_VERSION_STEP);              \
        MOV_A_R(REG_TRANSIENT_1, REG_TRANSIENT_2);                     \
        MOV_O_R(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag), \
                REG_TRANSIENT_2);                                      \
    } while (0)
#else
#define DICT_NEW_VERSION()                                             \
    do {                                                               \
        MOV_R_O(REG_TRANSIENT_1, REG_X64_RDX,                          \
                offsetof(store_cache_t, version));                     \
        INC_A(REG_TRANSIENT_1);                                        \
        MOV_R_A(REG_TRANSIENT_1, REG_TRANSIENT_1);                     \
        MOV_O_R(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag), \
                REG_TRANSIENT_1);                                      \
    } while (0)
#endif
#if LE_HAS_DICT_KEYS_KIND
/* the mask of keys of 2**n slots, for n up to 7 */
static signed char le_probe_masks[8] = {0, 1, 3, 7, 15, 31, 63, 127};
#define STORE_MAX_STUB_JUMPS 10
#endif

static code_t *
inline_store_body(PsycoObject *po, code_t *code, inline_store_t *s,
                  const site_consts_t *consts, const bool *near, code_t **jat,
                  int *pnj, code_t **pmiss)
{
    code_t *start = code;
    int nj = 0;
    code_t *fill_jz = NULL, *gc_ok = NULL, *store, *decref;
    long values_at = 0; /* the values' array, from R8 */
#if LE_HAS_DICT_KEYS_KIND
    code_t *jprobe[2], *jstub[STORE_MAX_STUB_JUMPS];
    int nprobe = 0, nstub = 0, i;
#endif
#define JCC_MISS(cc)                    \
    do {                                \
        jat[nj] = code;                 \
        if (near[nj]) {                 \
            WRITE_2(0x0F, 0x80 | (cc)); \
            code += 4;                  \
        }                               \
        else {                          \
            WRITE_2(0x70 | (cc), 0);    \
        }                               \
        nj++;                           \
    } while (0)
#define JCC_PROBE(cc)               \
    do {                            \
        jprobe[nprobe++] = code;    \
        WRITE_2(0x0F, 0x80 | (cc)); \
        code += 4;                  \
    } while (0)
#define JCC_STUB(cc)                                \
    do {                                            \
        extra_assert(nstub < STORE_MAX_STUB_JUMPS); \
        jstub[nstub++] = code;                      \
        WRITE_2(0x0F, 0x80 | (cc));                 \
        code += 4;                                  \
    } while (0)
    load_operand(po, &code, s->obj->source, REG_X64_RDI, start, consts);
    load_operand(po, &code, s->value->source, REG_X64_RSI, start, consts);
    LOAD_ADDRESS(REG_X64_RDX, s->cache);
    if (IN_VALUES(s)) {
#if LE_HAS_INLINE_VALUES
        TEST_I8_BO(REG_X64_RDI, s->dictoffset + LE_VALUES_VALID, 1);
        JCC_PROBE(CC_E); /* no longer its attributes */
        MOV_R_O(REG_X64_RAX, REG_X64_RDI, LE_MANAGED_DICT_OFFSET);
        TEST_R_R(REG_X64_RAX, REG_X64_RAX);
        JCC_MISS(CC_NE); /* a dict was made, which may be watched */
        LEA_R_O(REG_X64_R8, REG_X64_RDI, s->dictoffset);
        values_at = LE_VALUES_ARRAY;
#elif LE_HAS_TAGGED_DICT_OR_VALUES
        TEST_I8_BO(REG_X64_RDI, s->dictoffset, 1);
        JCC_PROBE(CC_E); /* the dict, or none */
        MOV_R_O(REG_X64_R8, REG_X64_RDI, s->dictoffset);
        values_at = 1;
#else
        MOV_R_O(REG_X64_R8, REG_X64_RDI, s->dictoffset);
        TEST_R_R(REG_X64_R8, REG_X64_R8);
#if LE_HAS_DICT_KEYS_KIND
        JCC_PROBE(CC_E); /* the dict was made */
#else
        JCC_MISS(CC_E); /* the dict was made */
#endif
#endif
        LOAD_ADDRESS(REG_X64_R9, s->keys);
    }
    else {
        MOV_R_O(REG_X64_RCX, REG_X64_RDI, s->dictoffset);
        TEST_R_R(REG_X64_RCX, REG_X64_RCX);
        JCC_MISS(CC_E); /* no dict yet */
        MOV_R_O(REG_X64_R8, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_values));
        TEST_R_R(REG_X64_R8, REG_X64_R8);
#if LE_HAS_DICT_KEYS_KIND
        JCC_PROBE(CC_E); /* combined */
#else
        JCC_MISS(CC_E); /* combined */
#endif
#if LE_HAS_DICT_WATCHER_BITS
        TEST_I8_BO(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag),
                   (signed char)LE_DICT_WATCHER_MASK);
        JCC_MISS(CC_NE); /* watched (le_dict_watched) */
#endif
        MOV_R_O(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_keys));
        KEYS_SHAPE_LOAD(REG_X64_RAX, REG_X64_R9);
        CMP_R_A(REG_X64_RAX, REG_X64_RDX);
        JCC_MISS(CC_NE); /* keys of another shape */
    }
    MOV_R_O(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, kword));
    MOV_R_X8(REG_X64_RAX, REG_X64_R9, 0, REG_X64_RAX, 8);
    CMP_R_O8(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, name));
    JCC_MISS(CC_NE); /* the name isn't at the cached entry */
    MOV_R_O(REG_X64_R9, REG_X64_RDX, offsetof(store_cache_t, ix));
    if (s->gc != STORE_GC_NONE) {          /* MAINTAIN_TRACKING */
        LE_GC_UNTRACKED_TEST(REG_X64_RCX); /* mechanisms/store_gc.h */
        if (s->gc == STORE_GC_DICT) {
            JCC_MISS(CC_E);
        }
        else {
            gc_ok = code;
            WRITE_2(0x70 | CC_NE, 0); /* the dict is tracked */
            MOV_R_O(REG_TRANSIENT_1, REG_X64_RSI,
                    CPY_OFFSET(PyObject, ob_type));
            TEST_I8_BO(REG_TRANSIENT_1, CPY_OFFSET(PyTypeObject, tp_flags) + 1,
                       Py_TPFLAGS_HAVE_GC >> 8);
            JCC_MISS(CC_NE);
            gc_ok[1] = (code_t)(code - (gc_ok + 2));
        }
    }
    MOV_R_X8(REG_X64_RAX, REG_X64_R8, values_at, REG_X64_R9, 8);
    TEST_R_R(REG_X64_RAX, REG_X64_RAX);
    if (s->fill) {
        fill_jz = code;
        WRITE_2(0x70 | CC_E, 0);
    }
    else {
        JCC_MISS(CC_E); /* an empty slot */
    }
    store = code;
    MOV_X8_R(REG_X64_R8, values_at, REG_X64_R9, 8, REG_X64_RSI);
    INC_STORED(REG_X64_RSI);
    if (!IN_VALUES(s)) /* a dict's new version tag */
        DICT_NEW_VERSION();
    decref = code;
#if LE_HAS_IMMORTAL_OBJECTS
    CMP_I8_DA(0, REG_X64_RAX); /* an immortal is left alone */
    s->done[2] = code;
    WRITE_2(0x0F, 0x80 | CC_S);
    code += 4;
#endif
    DEC_A(REG_X64_RAX); /* last: may run any code */
    s->done[0] = code;
    WRITE_2(0x0F, 0x80 | CC_NE);
    code += 4;
    CALL_GLUE(psyco_dealloc_rax_glue, start);
    s->done[1] = code;
    WRITE_1(0xE9);
    code += 4;
    if (!s->fill)
        goto probe;
    extra_assert(code - (fill_jz + 2) < 128);
    fill_jz[1] = (code_t)(code - (fill_jz + 2));
    le_trace_jump_cond(fill_jz, fill_jz + 2, code);
    if (IN_VALUES(s)) {
#if LE_HAS_INLINE_VALUES
        /* the order's entries follow the values' 'capacity' words, and
           'size' is its length; every index the keys hold is below the
           capacity */
        MOVZX_DR_BO(REG_X64_RCX, REG_X64_R8,
                    values_at - LE_VALUES_ARRAY + LE_VALUES_CAPACITY);
        LEA_R_X8(REG_X64_RCX, REG_X64_R8, values_at, REG_X64_RCX, 8);
        MOVZX_DR_BO(REG_X64_RAX, REG_X64_R8,
                    values_at - LE_VALUES_ARRAY + LE_VALUES_SIZE);
        MOV_BX8_R(REG_X64_RCX, 0, REG_X64_RAX, 1, REG_X64_R9);
        ADD_R_I8(REG_X64_RAX, 1);
        MOV_BO_R(REG_X64_R8, values_at - LE_VALUES_ARRAY + LE_VALUES_SIZE,
                 REG_X64_RAX);
#else
        /* the order's length is the byte below the values, its entries the
           bytes below that, the first nearest; the values have room for
           every entry of the keys */
        MOVZX_DR_BO(REG_X64_RCX, REG_X64_R8, values_at - 2);
        ADD_R_I8(REG_X64_RCX, 1);
        MOV_BO_R(REG_X64_R8, values_at - 2, REG_X64_RCX);
        UNARY_INSTR_ON_REG(3, REG_X64_RCX); /* neg */
        MOV_BX8_R(REG_X64_R8, values_at - 2, REG_X64_RCX, 1, REG_X64_R9);
#endif
    }
    else {
        CMP_R_O8(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_used));
        JCC_MISS(CC_NE); /* out of order: CPython converts the dict */
        INC_O8(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_used));
    }
    MOV_R_R(REG_X64_RAX, REG_X64_RSI);
    INC_STORED(REG_X64_RSI);
    extra_assert(store - (code + 2) >= -128);
    WRITE_2(0xEB, (code_t)(store - (code + 2)));
    le_trace_jump(code - 2, store);
probe:
    (void)decref;
#if LE_HAS_DICT_KEYS_KIND
    if (nprobe > 0) {
        /* the probe of a combined dict, in RCX (lookdict_unicode's), which
           replaces the value of a name it finds; the miss arguments are
           loaded again for every failure */
        code_t *body, *jnext, *jfound;
        long ind = offsetof(le_dictkeys_t, dk_indices);
        Py_hash_t hash = PyObject_Hash(s->cache->name);
        for (i = 0; i < nprobe; i++) {
            *(dword_t *)(jprobe[i] + 2) = REL32(jprobe[i] + 6, code);
            le_trace_jump_cond(jprobe[i], jprobe[i] + 6, code);
        }
        if (IN_VALUES(s)) {
            MOV_R_O(REG_X64_RCX, REG_X64_RDI, LE_MANAGED_DICT_OFFSET);
            TEST_R_R(REG_X64_RCX, REG_X64_RCX);
            JCC_STUB(CC_E);
            MOV_R_O(REG_X64_R8, REG_X64_RCX,
                    CPY_OFFSET(PyDictObject, ma_values));
            TEST_R_R(REG_X64_R8, REG_X64_R8);
            JCC_STUB(CC_NE); /* split */
        }
#if LE_HAS_DICT_WATCHER_BITS
        TEST_I8_BO(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag),
                   (signed char)LE_DICT_WATCHER_MASK);
        JCC_STUB(CC_NE); /* watched (le_dict_watched) */
#endif
        MOV_R_O(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_keys));
        MOVZX_DR_BO(REG_X64_RAX, REG_X64_R9, offsetof(le_dictkeys_t, dk_kind));
        CMP_I8_R(DICT_KEYS_UNICODE, REG_X64_RAX);
        JCC_STUB(CC_NE); /* keys that hold hashes */
        MOVZX_DR_BO(REG_X64_R8, REG_X64_R9,
                    offsetof(le_dictkeys_t, dk_log2_size));
        CMP_I8_R(7, REG_X64_R8);
        JCC_STUB(CC_uG); /* wider indices */
        LOAD_ADDRESS(REG_X64_RDX, le_probe_masks);
        MOVSX_R_BX8(REG_X64_R8, REG_X64_RDX, 0, REG_X64_R8, 1);
        MOV_R_IABS(REG_TRANSIENT_1, hash);
        MOV_R_R(REG_X64_RSI, REG_TRANSIENT_1);
        LOAD_ADDRESS(REG_TRANSIENT_2, s->cache->name);
        MOV_R_IABS(REG_X64_RDI, hash & 127);
        AND_R_R(REG_X64_RDI, REG_X64_R8);
        MOVSX_R_BX8(REG_X64_RAX, REG_X64_R9, ind, REG_X64_RDI, 1);
        body = code;
        CMP_I8_R(-1, REG_X64_RAX);
        JCC_STUB(CC_E); /* absent: a new entry */
        TEST_R_R(REG_X64_RAX, REG_X64_RAX);
        jnext = code;
        WRITE_2(0x70 | CC_S, 0); /* a dummy */
        /* the entry's key is at keys + ind + size + 16 * ix */
        SHIFT_LEFT_BY(REG_X64_RAX, 4);
        ADD_R_R(REG_X64_RAX, REG_X64_R8);
        MOV_R_X8(REG_X64_RDX, REG_X64_R9, ind + 1, REG_X64_RAX, 1);
        CMP_R_R(REG_TRANSIENT_2, REG_X64_RDX);
        jfound = code;
        WRITE_2(0x70 | CC_E, 0);
        CMP_R_O8(REG_TRANSIENT_1, REG_X64_RDX,
                 CPY_OFFSET(PyASCIIObject, hash));
        JCC_STUB(CC_E); /* the same hash */
        jnext[1] = (code_t)(code - (jnext + 2));
        le_trace_jump_cond(jnext, jnext + 2, code);
        SHIFT_RIGHT_BY(REG_X64_RSI, 5);
        LEA_R_X8(REG_X64_RDI, REG_X64_RDI, 0, REG_X64_RDI, 4);
        LEA_R_X8(REG_X64_RDI, REG_X64_RDI, 1, REG_X64_RSI, 1);
        AND_R_R(REG_X64_RDI, REG_X64_R8);
        MOVSX_R_BX8(REG_X64_RAX, REG_X64_R9, ind, REG_X64_RDI, 1);
        extra_assert(body - (code + 2) >= -128);
        WRITE_2(0xEB, (code_t)(body - (code + 2)));
        le_trace_jump(code - 2, body);
        jfound[1] = (code_t)(code - (jfound + 2));
        le_trace_jump_cond(jfound, jfound + 2, code);
        /* R9: the entry's value */
        LEA_R_X8(REG_X64_R9, REG_X64_R9, ind + 9, REG_X64_RAX, 1);
        load_operand(po, &code, s->value->source, REG_X64_RSI, start, consts);
        if (s->dict_gc != STORE_GC_NONE) {     /* MAINTAIN_TRACKING */
            LE_GC_UNTRACKED_TEST(REG_X64_RCX); /* mechanisms/store_gc.h */
            if (s->dict_gc == STORE_GC_DICT) {
                JCC_STUB(CC_E);
            }
            else {
                code_t *tracked = code;
                WRITE_2(0x70 | CC_NE, 0);
                MOV_R_O(REG_TRANSIENT_1, REG_X64_RSI,
                        CPY_OFFSET(PyObject, ob_type));
                TEST_I8_BO(REG_TRANSIENT_1,
                           CPY_OFFSET(PyTypeObject, tp_flags) + 1,
                           Py_TPFLAGS_HAVE_GC >> 8);
                JCC_STUB(CC_NE);
                tracked[1] = (code_t)(code - (tracked + 2));
                le_trace_jump_cond(tracked, tracked + 2, code);
            }
        }
        MOV_R_A(REG_X64_RAX, REG_X64_R9);
        MOV_A_R(REG_X64_R9, REG_X64_RSI);
        INC_STORED(REG_X64_RSI);
        LOAD_ADDRESS(REG_X64_RDX, s->cache);
        DICT_NEW_VERSION();
        WRITE_1(0xE9);
        UPDATABLE_WRITE_32BIT(REL32(code + 4, decref));
        le_trace_jump(code - 5, decref);
        for (i = 0; i < nstub; i++) {
            *(dword_t *)(jstub[i] + 2) = REL32(jstub[i] + 6, code);
            le_trace_jump_cond(jstub[i], jstub[i] + 6, code);
        }
        load_operand(po, &code, s->obj->source, REG_X64_RDI, start, consts);
        load_operand(po, &code, s->value->source, REG_X64_RSI, start, consts);
        LOAD_ADDRESS(REG_X64_RDX, s->cache);
    }
#endif
    *pmiss = code;
    CALL_GLUE(s->miss_glue, start);
    TEST_DR_DR(REG_X64_RAX, REG_X64_RAX);
    extra_assert(nj <= STORE_MAX_MISS_JUMPS);
    *pnj = nj;
    return code;
#undef JCC_MISS
#undef JCC_PROBE
#undef JCC_STUB
}

DEFINEFN
void
psyco_inline_store_begin(PsycoObject *po, inline_store_t *s)
{
    bool near[STORE_MAX_MISS_JUMPS];
    site_consts_t consts;
    code_t *jat[STORE_MAX_MISS_JUMPS], *miss, *start;
    int i, nj;
    extra_assert(!is_virtualtime(s->obj->source) &&
                 !is_virtualtime(s->value->source));
    if (!is_respawning(po) && po->codelimit - po->code < 1024)
        PsycoObject_EmergencyCodeRoom(po); /* room for both passes */
    BEGIN_CODE
    NEED_CC();
    NEED_REGISTER(REG_X64_RAX);
    NEED_REGISTER(REG_X64_RCX);
    NEED_REGISTER(REG_X64_RDX);
    NEED_REGISTER(REG_X64_RSI);
    NEED_REGISTER(REG_X64_RDI);
    NEED_REGISTER(REG_X64_R8);
    NEED_REGISTER(REG_X64_R9);
    start = code;
    consts.n = 0;
    site_consts_operand(&consts, start, s->obj->source);
    site_consts_operand(&consts, start, s->value->source);
    site_const(&consts, start, (long)s->cache);
    if (IN_VALUES(s))
        site_const(&consts, start, (long)s->keys);
#if LE_HAS_DICT_KEYS_KIND
    site_const(&consts, start, (long)s->cache->name);
#endif
    for (i = 0; i < STORE_MAX_MISS_JUMPS; i++) near[i] = true;
    inline_store_body(po, start, s, &consts, near, jat, &nj, &miss);
    for (i = 0; i < nj; i++) /* each later jump made short shortens it */
        near[i] = miss - jat[i] - 6 > 127;
    code = inline_store_body(po, start, s, &consts, near, jat, &nj, &miss);
    for (i = 0; i < nj; i++) {
        if (near[i]) {
            *(dword_t *)(jat[i] + 2) = REL32(jat[i] + 6, miss);
            le_trace_jump_cond(jat[i], jat[i] + 6, miss);
        }
        else {
            extra_assert(miss - (jat[i] + 2) < 128);
            jat[i][1] = (code_t)(miss - (jat[i] + 2));
            le_trace_jump_cond(jat[i], jat[i] + 2, miss);
        }
    }
    END_CODE
}

DEFINEFN
void
psyco_inline_store_end(PsycoObject *po, inline_store_t *s)
{
    int i;
    /* while respawning, the error check moved po->code back to the start
       of the trash buffer: nothing to point */
    if (is_respawning(po))
        return;
    for (i = 0; i < STORE_DONE_JUMPS; i++) {
        code_t *at = s->done[i], *end = at + (at[0] == 0xE9 ? 5 : 6);
        *(dword_t *)(end - 4) = REL32(end, po->code);
        le_trace_jump(at, po->code);
    }
}

/* The load (inline_load_t). RG, the result's register, is chosen first;
   OBJ is the object's register (RDI if it has none):

     rcx = [OBJ + dictoffset]; test rcx, rcx; jz NONE
   expected present:
     r8 = values (jz MISS: combined), r9 = keys, rdx = cache
     cmp shape, [rdx]; jne MISS
     cmp [r9 + kword*8], name; jne MISS
     RG = [r8 + ix*8]; jmp DONE
   expected absent, keys with 1-byte indices (dk_size < 128):
     r8 = mask, rdi = i = hash & mask
     rax = indices[i] (sign-extended); cmp rax, -1; jne TAKEN
     NONE: xor RG, RG; jmp DONE
     TAKEN: rdx = hash, rsi = perturb
     LOOP: >= 0: the entry's me_hash == hash: MISS (it may be the name)
       (-2, a dummy, goes on)
       perturb >>= 5; i = (i*5 + perturb + 1) & mask
       rax = indices[i]; cmp rax, -1; jne LOOP; jmp NONE
   with 'keys' (a managed dict's values, at the values pointer):
     rcx = OBJ; r8 = [OBJ + dictoffset]; test r8, r8; jz MISS
     (or, with the values in the object, test [OBJ + dictoffset + valid];
     jz MISS; r8 = OBJ, and the array is further on)
     r9 = keys, rdx = cache
     cmp [r9 + kword*8], name; je PRESENT
     cmp [r9 + dk_nentries], [rdx + nentries]; jne MISS
     xor RG, RG; jmp DONE
     PRESENT: RG = [r8 + ix*8]; jmp DONE
   MISS: rdi = rcx, rsi = cache; call miss_glue; RG = rax
   DONE:

   An entry whose hash differs can't hold the name, so the probe never
   compares keys and runs no code; it is lookdict_split's (and, with
   dummies skipped, lookdict's and lookdict_unicode's), so a dict of any
   kind gives the answer its own lookup would. hash & 127 is the first
   index's, as the mask is less than 127. Forward jumps that may be far are
   near, the others short. */
_Static_assert(offsetof(le_dictentry_t, me_hash) == 0 &&
                   sizeof(le_dictentry_t) == 24,
               "an entry is found at 24 * index and its hash first");
#define LOAD_MAX_JUMPS 12
#define LOAD_MAX_FORWARD 24
/* the forward jumps of the load, in the order emitted: where each is and
   goes, and whether it is short; all are near in the first pass */
typedef struct {
    code_t *at[LOAD_MAX_FORWARD], *to[LOAD_MAX_FORWARD];
    bool is_short[LOAD_MAX_FORWARD];
    int n;
} load_jumps_t;

static code_t *
inline_load_body(PsycoObject *po, code_t *code, inline_load_t *s, reg_t rg,
                 const site_consts_t *consts, load_jumps_t *fj)
{
    code_t *start = code, *at;
    int jmiss[LOAD_MAX_JUMPS], jnone[2], jdone[5];
    int nmiss = 0, nnone = 0, ndone = 0, i;
#if LE_HAS_DICT_KEYS_KIND
    int jdict[2];
    int ndict = 0;
#endif
    reg_t ro;
#define FORWARD(list, cnt)                      \
    do {                                        \
        extra_assert(fj->n < LOAD_MAX_FORWARD); \
        fj->at[fj->n] = code;                   \
        list[cnt++] = fj->n++;                  \
    } while (0)
#define NEAR_JCC(cc, list, cnt)            \
    do {                                   \
        bool _short = fj->is_short[fj->n]; \
        FORWARD(list, cnt);                \
        if (_short) {                      \
            WRITE_2(0x70 | (cc), 0);       \
        }                                  \
        else {                             \
            WRITE_2(0x0F, 0x80 | (cc));    \
            code += 4;                     \
        }                                  \
    } while (0)
#define NEAR_JMP(list, cnt)                \
    do {                                   \
        bool _short = fj->is_short[fj->n]; \
        FORWARD(list, cnt);                \
        if (_short) {                      \
            WRITE_2(0xEB, 0);              \
        }                                  \
        else {                             \
            WRITE_1(0xE9);                 \
            code += 4;                     \
        }                                  \
    } while (0)
#define POINT(list, cnt, target)                            \
    for (i = 0; i < (cnt); i++) {                           \
        at = fj->at[list[i]];                               \
        fj->to[list[i]] = (target);                         \
        if (at[0] == 0xE9) {                                \
            *(dword_t *)(at + 1) = REL32(at + 5, (target)); \
            le_trace_jump(at, (target));                    \
        }                                                   \
        else if (at[0] == 0x0F) {                           \
            *(dword_t *)(at + 2) = REL32(at + 6, (target)); \
            le_trace_jump_cond(at, at + 6, (target));       \
        }                                                   \
        else {                                              \
            extra_assert((target) - (at + 2) < 128);        \
            at[1] = (code_t)((target) - (at + 2));          \
            if (at[0] == 0xEB)                              \
                le_trace_jump(at, (target));                \
            else                                            \
                le_trace_jump_cond(at, at + 2, (target));   \
        }                                                   \
    }
#define SHORT_BACK(cc, target)                            \
    do {                                                  \
        extra_assert((target) - (code + 2) >= -128);      \
        WRITE_2((cc) < 0 ? 0xEB : 0x70 | (cc),            \
                (code_t)((target) - (code + 2)));         \
        if ((cc) < 0)                                     \
            le_trace_jump(code - 2, (target));            \
        else                                              \
            le_trace_jump_cond(code - 2, code, (target)); \
    } while (0)
    if (is_runtime(s->obj->source) && !RUNTIME_REG_IS_NONE(s->obj)) {
        ro = RUNTIME_REG(s->obj);
    }
    else {
        ro = REG_X64_RDI;
        load_operand(po, &code, s->obj->source, ro, start, consts);
    }
    if (IN_VALUES(s)) {
#if LE_MANAGED_VALUES
        code_t *jpresent;
        long values_at = 0;       /* the values' array, from R8 */
        MOV_R_R(REG_X64_RCX, ro); /* for the miss call */
#if LE_HAS_INLINE_VALUES
        TEST_I8_BO(ro, s->dictoffset + LE_VALUES_VALID, 1);
        NEAR_JCC(CC_E, jdict, ndict); /* no longer its attributes */
        LEA_R_O(REG_X64_R8, ro, s->dictoffset);
        values_at = LE_VALUES_ARRAY;
#elif LE_HAS_TAGGED_DICT_OR_VALUES
        TEST_I8_BO(ro, s->dictoffset, 1);
        NEAR_JCC(CC_E, jdict, ndict); /* the dict, or none */
        MOV_R_O(REG_X64_R8, ro, s->dictoffset);
        values_at = 1;
#else
        MOV_R_O(REG_X64_R8, ro, s->dictoffset);
        TEST_R_R(REG_X64_R8, REG_X64_R8);
        NEAR_JCC(CC_E, jdict, ndict); /* the dict was made */
#endif
        LOAD_ADDRESS(REG_X64_R9, s->keys);
        LOAD_ADDRESS(REG_X64_RDX, s->cache);
        MOV_R_O(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, kword));
        MOV_R_X8(REG_X64_RAX, REG_X64_R9, 0, REG_X64_RAX, 8);
        CMP_R_O8(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, name));
        jpresent = code;
        WRITE_2(0x70 | CC_E, 0);
        MOV_R_O(REG_X64_RAX, REG_X64_R9, offsetof(le_dictkeys_t, dk_nentries));
        CMP_R_O8(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, nentries));
        NEAR_JCC(CC_NE, jmiss, nmiss); /* keys that may hold it now */
        XOR_R_R(rg, rg);
        NEAR_JMP(jdone, ndone);
        jpresent[1] = (code_t)(code - (jpresent + 2));
        le_trace_jump_cond(jpresent, jpresent + 2, code);
        MOV_R_O(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, ix));
        MOV_R_X8(rg, REG_X64_R8, values_at, REG_X64_RAX, 8);
        NEAR_JMP(jdone, ndone);
        POINT(jdict, ndict, code);
        ndict = 0;
        MOV_R_O(REG_X64_RSI, REG_X64_RCX, LE_MANAGED_DICT_OFFSET);
        TEST_R_R(REG_X64_RSI, REG_X64_RSI);
        NEAR_JCC(CC_E, jmiss, nmiss);
        MOV_R_O(REG_X64_R8, REG_X64_RSI, CPY_OFFSET(PyDictObject, ma_values));
        TEST_R_R(REG_X64_R8, REG_X64_R8);
        NEAR_JCC(CC_E, jdict, ndict); /* combined: the probe */
        NEAR_JMP(jmiss, nmiss);
#endif
    }
    /* the probe reads entries that hold their hash, which keys with a kind
       may not: there an absent name is a miss */
    else if (!s->absent || LE_HAS_DICT_KEYS_KIND) {
        MOV_R_O(REG_X64_RCX, ro, s->dictoffset);
        TEST_R_R(REG_X64_RCX, REG_X64_RCX);
        NEAR_JCC(CC_E, jnone, nnone); /* no dict yet */
        MOV_R_O(REG_X64_R8, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_values));
        TEST_R_R(REG_X64_R8, REG_X64_R8);
#if LE_HAS_DICT_KEYS_KIND
        NEAR_JCC(CC_E, jdict, ndict); /* combined: the probe */
#else
        NEAR_JCC(CC_E, jmiss, nmiss); /* combined */
#endif
        MOV_R_O(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_keys));
        LOAD_ADDRESS(REG_X64_RDX, s->cache);
        KEYS_SHAPE_LOAD(REG_X64_RAX, REG_X64_R9);
        CMP_R_A(REG_X64_RAX, REG_X64_RDX);
        NEAR_JCC(CC_NE, jmiss, nmiss); /* keys of another shape */
        MOV_R_O(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, kword));
        MOV_R_X8(REG_X64_RAX, REG_X64_R9, 0, REG_X64_RAX, 8);
        CMP_R_O8(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, name));
        NEAR_JCC(CC_NE, jmiss, nmiss); /* the name isn't at the cached entry */
        MOV_R_O(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, ix));
        MOV_R_X8(rg, REG_X64_R8, 0, REG_X64_RAX, 8);
        NEAR_JMP(jdone, ndone);
        POINT(jnone, nnone, code);
        XOR_R_R(rg, rg);
        NEAR_JMP(jdone, ndone);
    }
#if !LE_HAS_DICT_KEYS_KIND
    else {
        code_t *taken, *none, *body, *jtaken, *jneg;
        long ind = offsetof(le_dictkeys_t, dk_indices);
        MOV_R_O(REG_X64_RCX, ro, s->dictoffset);
        TEST_R_R(REG_X64_RCX, REG_X64_RCX);
        NEAR_JCC(CC_E, jnone, nnone); /* no dict yet */
        MOV_R_O(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_keys));
        MOV_R_O(REG_X64_R8, REG_X64_R9, offsetof(le_dictkeys_t, dk_size));
        CMP_I8_R(127, REG_X64_R8);
        NEAR_JCC(CC_uG, jmiss, nmiss); /* wider indices */
        SUB_R_I8(REG_X64_R8, 1);
        MOV_R_IABS(REG_X64_RDI, s->hash & 127);
        AND_R_R(REG_X64_RDI, REG_X64_R8);
        MOVSX_R_BX8(REG_X64_RAX, REG_X64_R9, ind, REG_X64_RDI, 1);
        CMP_I8_R(-1, REG_X64_RAX);
        jtaken = code;
        WRITE_2(0x70 | CC_NE, 0);
        POINT(jnone, nnone, code);
        nnone = 0;
        none = code;
        XOR_R_R(rg, rg);
        NEAR_JMP(jdone, ndone);
        taken = code;
        jtaken[1] = (code_t)(taken - (jtaken + 2));
        le_trace_jump_cond(jtaken, jtaken + 2, taken);
        MOV_R_IABS(REG_X64_RDX, s->hash);
        MOV_R_R(REG_X64_RSI, REG_X64_RDX);
        body = code;
        TEST_R_R(REG_X64_RAX, REG_X64_RAX);
        jneg = code;
        WRITE_2(0x70 | CC_S, 0); /* a dummy */
        /* entries[ix] is at keys + ind + dk_size + 24 * ix */
        LEA_R_X8(REG_X64_RAX, REG_X64_RAX, 0, REG_X64_RAX, 2);
        LEA_R_X8(REG_X64_RAX, REG_X64_R8, 0, REG_X64_RAX, 8);
        CMP_R_X8(REG_X64_RDX, REG_X64_R9, ind + 1, REG_X64_RAX, 1);
        NEAR_JCC(CC_E, jmiss, nmiss); /* the same hash */
        jneg[1] = (code_t)(code - (jneg + 2));
        le_trace_jump_cond(jneg, jneg + 2, code);
        SHIFT_RIGHT_BY(REG_X64_RSI, 5);
        LEA_R_X8(REG_X64_RDI, REG_X64_RDI, 0, REG_X64_RDI, 4);
        LEA_R_X8(REG_X64_RDI, REG_X64_RDI, 1, REG_X64_RSI, 1);
        AND_R_R(REG_X64_RDI, REG_X64_R8);
        MOVSX_R_BX8(REG_X64_RAX, REG_X64_R9, ind, REG_X64_RDI, 1);
        CMP_I8_R(-1, REG_X64_RAX);
        SHORT_BACK(CC_NE, body);
        SHORT_BACK(-1, none); /* empty: absent */
    }
#endif
#if LE_HAS_DICT_KEYS_KIND
    if (ndict > 0) {
        /* the probe of a combined dict, in RSI, or RCX outside values */
        code_t *body, *jnext, *jfound;
        long ind = offsetof(le_dictkeys_t, dk_indices);
        POINT(jdict, ndict, code);
        nnone = 0;
        if (!IN_VALUES(s))
            MOV_R_R(REG_X64_RSI, REG_X64_RCX);
        MOV_R_O(REG_X64_R9, REG_X64_RSI, CPY_OFFSET(PyDictObject, ma_keys));
        MOVZX_DR_BO(REG_X64_RAX, REG_X64_R9, offsetof(le_dictkeys_t, dk_kind));
        CMP_I8_R(DICT_KEYS_UNICODE, REG_X64_RAX);
        NEAR_JCC(CC_NE, jmiss, nmiss); /* keys that hold hashes */
        MOVZX_DR_BO(REG_X64_R8, REG_X64_R9,
                    offsetof(le_dictkeys_t, dk_log2_size));
        CMP_I8_R(7, REG_X64_R8);
        NEAR_JCC(CC_uG, jmiss, nmiss); /* wider indices */
        LOAD_ADDRESS(REG_X64_RDX, le_probe_masks);
        MOVSX_R_BX8(REG_X64_R8, REG_X64_RDX, 0, REG_X64_R8, 1);
        MOV_R_IABS(REG_TRANSIENT_1, s->hash);
        MOV_R_R(REG_X64_RSI, REG_TRANSIENT_1);
        LOAD_ADDRESS(REG_TRANSIENT_2, s->cache->name);
        MOV_R_IABS(REG_X64_RDI, s->hash & 127);
        AND_R_R(REG_X64_RDI, REG_X64_R8);
        MOVSX_R_BX8(REG_X64_RAX, REG_X64_R9, ind, REG_X64_RDI, 1);
        body = code;
        CMP_I8_R(-1, REG_X64_RAX);
        NEAR_JCC(CC_E, jnone, nnone); /* empty: absent */
        TEST_R_R(REG_X64_RAX, REG_X64_RAX);
        jnext = code;
        WRITE_2(0x70 | CC_S, 0); /* a dummy */
        /* the entry's key is at keys + ind + size + 16 * ix */
        SHIFT_LEFT_BY(REG_X64_RAX, 4);
        ADD_R_R(REG_X64_RAX, REG_X64_R8);
        MOV_R_X8(REG_X64_RDX, REG_X64_R9, ind + 1, REG_X64_RAX, 1);
        CMP_R_R(REG_TRANSIENT_2, REG_X64_RDX);
        jfound = code;
        WRITE_2(0x70 | CC_E, 0);
        CMP_R_O8(REG_TRANSIENT_1, REG_X64_RDX,
                 CPY_OFFSET(PyASCIIObject, hash));
        NEAR_JCC(CC_E, jmiss, nmiss); /* the same hash */
        jnext[1] = (code_t)(code - (jnext + 2));
        le_trace_jump_cond(jnext, jnext + 2, code);
        SHIFT_RIGHT_BY(REG_X64_RSI, 5);
        LEA_R_X8(REG_X64_RDI, REG_X64_RDI, 0, REG_X64_RDI, 4);
        LEA_R_X8(REG_X64_RDI, REG_X64_RDI, 1, REG_X64_RSI, 1);
        AND_R_R(REG_X64_RDI, REG_X64_R8);
        MOVSX_R_BX8(REG_X64_RAX, REG_X64_R9, ind, REG_X64_RDI, 1);
        SHORT_BACK(-1, body);
        jfound[1] = (code_t)(code - (jfound + 2));
        le_trace_jump_cond(jfound, jfound + 2, code);
        MOV_R_X8(rg, REG_X64_R9, ind + 9, REG_X64_RAX, 1);
        NEAR_JMP(jdone, ndone);
        POINT(jnone, nnone, code);
        nnone = 0;
        XOR_R_R(rg, rg);
        NEAR_JMP(jdone, ndone);
    }
#endif
    POINT(jmiss, nmiss, code);
    MOV_R_R(REG_X64_RDI, REG_X64_RCX);
    LOAD_ADDRESS(REG_X64_RSI, s->cache);
    CALL_GLUE(s->miss_glue, start);
    MOV_R_R(rg, REG_X64_RAX);
    POINT(jdone, ndone, code);
    return code;
#undef FORWARD
#undef NEAR_JCC
#undef NEAR_JMP
#undef POINT
#undef SHORT_BACK
}

DEFINEFN
vinfo_t *
psyco_inline_load(PsycoObject *po, inline_load_t *s)
{
    load_jumps_t fj;
    site_consts_t consts;
    code_t *start;
    vinfo_t *v;
    reg_t rg;
    int i;
    extra_assert(!is_virtualtime(s->obj->source));
    if (!is_respawning(po) && po->codelimit - po->code < 512)
        PsycoObject_EmergencyCodeRoom(po);
    BEGIN_CODE
    NEED_CC();
    NEED_REGISTER(REG_X64_RAX);
    NEED_REGISTER(REG_X64_RCX);
    NEED_REGISTER(REG_X64_RDX);
    NEED_REGISTER(REG_X64_RSI);
    NEED_REGISTER(REG_X64_RDI);
    NEED_REGISTER(REG_X64_R8);
    NEED_REGISTER(REG_X64_R9);
    NEED_FREE_REG(rg); /* callee-saved: the miss call keeps it */
    start = code;
    consts.n = 0;
    site_consts_operand(&consts, start, s->obj->source);
    site_const(&consts, start, (long)s->cache);
    if (IN_VALUES(s))
        site_const(&consts, start, (long)s->keys);
#if LE_HAS_DICT_KEYS_KIND
    site_const(&consts, start, (long)s->cache->name);
#endif
    /* the first pass, all near, gives an upper bound on each distance */
    for (i = 0; i < LOAD_MAX_FORWARD; i++) {
        fj.is_short[i] = false;
        fj.to[i] = NULL;
    }
    fj.n = 0;
    inline_load_body(po, start, s, rg, &consts, &fj);
    for (i = 0; i < fj.n; i++)
        fj.is_short[i] = fj.to[i] != NULL && fj.to[i] - (fj.at[i] + 2) < 128;
    fj.n = 0;
    code = inline_load_body(po, start, s, rg, &consts, &fj);
    END_CODE
    v = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = v;
    return v;
}

/* A type guard (pobject.c:type_changed): tp's tp_flags and tp_version_tag
   compared with a guard's two words, which are read at run time. The
   expected words are reached RIP-relative when in reach; otherwise
   through RAX, which is freed either way, so that the compiler's state
   doesn't depend on where the code is (a replay runs elsewhere). */
DEFINEFN
condition_code_t
psyco_type_guard(PsycoObject *po, PyTypeObject *tp, unsigned long *flags,
                 unsigned long *tag)
{
    long off;
    BEGIN_CODE
    NEED_CC();
    NEED_REGISTER(REG_X64_RAX);
    MOV_R_I(REG_TRANSIENT_1, (long)tp);
    MOV_R_O(REG_TRANSIENT_2, REG_TRANSIENT_1,
            CPY_OFFSET(PyTypeObject, tp_flags));
    MOV_DR_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
             CPY_OFFSET(PyTypeObject, tp_version_tag));
    off = (long)flags - ((long)code + 7);
    if (FITS_IN_32BITS(off) && FITS_IN_32BITS((long)tag - ((long)code + 14))) {
        XOR_R_RIP(REG_TRANSIENT_2, off);
        XOR_R_RIP(REG_TRANSIENT_1, (long)tag - ((long)code + 7));
    }
    else {
        MOV_R_I(REG_X64_RAX, (long)flags);
        XOR_R_O(REG_TRANSIENT_2, REG_X64_RAX, 0);
        off = (char *)tag - (char *)flags;
        XOR_R_O(REG_TRANSIENT_1, REG_X64_RAX, off);
    }
    OR_R_R(REG_TRANSIENT_1, REG_TRANSIENT_2);
    END_CODE
    return CC_NE;
}

/* The entry form, as Psyco's DICT_ITEM_KEYVALUE, r10 the keys:
     cmp [r10 + shape], SHAPE; jne DONE    (KEYS_SHAPE_CMP)
     cmp [r10 + KEY], name; jne DONE
     cmp [r10 + KEY + 8], value
   DONE: ZF clear if changed.
   SHAPE and the two displacements are fields of the code, which
   psyco_global_guard_move rewrites when the entry moves. Keys of the same
   shape are as large and lay their entries out alike, so the entry read
   is inside them whichever keys the dict has now. The absent form first
   walks the probe of globals:
     cmp [r10 + shape], SHAPE; jne DONE
     for each slot walked: cmp [r10 + SLOT], what it held; jne DONE
       and for an entry: cmp dword [r10 + its me_hash], low 32 bits; jne DONE
       or, where the entries hold no hash, the hash its key, a str, keeps:
         mov r11, [r10 + its key]; cmp dword [r11 + hash], low 32 bits;
         jne DONE
     cmp [r10 + the empty slot], -1; jne DONE
     rax = builtins' ma_version_tag; mov r10, TAG; cmp rax, r10
   TAG a 64-bit field of the code. The tags form reads its tags from
   'tags' through r11. An absent guard shown again for a probe it can't
   walk compares globals' tag instead of the probe, as the tags form does
   but with the tag in the code (global_absent_emit). */
/* the address of a dict's tag, and its load into register 'r': where dicts
   have no version tag, the count of LeJIT's watcher (le_dict_counter),
   which the guard's form was chosen knowing there is */
#if LE_HAS_DICT_VERSION_TAG
#define LE_DICT_TAG_ADDRESS(d) (&((PyDictObject *)(d))->ma_version_tag)
#define LOAD_DICT_TAG(mov, r, d)                                 \
    do {                                                         \
        mov(r, (long)(d));                                       \
        MOV_R_O(r, r, CPY_OFFSET(PyDictObject, ma_version_tag)); \
    } while (0)
#else
#define LE_DICT_TAG_ADDRESS(d) le_dict_counter((PyDictObject *)(d))
#define LOAD_DICT_TAG(mov, r, d)              \
    do {                                      \
        mov(r, (long)LE_DICT_TAG_ADDRESS(d)); \
        MOV_R_A(r, r);                        \
    } while (0)
#endif
_Static_assert(offsetof(le_dictentry_t, me_key) ==
                   offsetof(le_dictentry_t, me_hash) + sizeof(PyObject *),
               "an entry's hash is the word before its key");
DEFINEFN
bool
psyco_global_absent(PyObject *globals, PyObject *name, global_absent_t *a)
{
    PyDictObject *mp = (PyDictObject *)globals;
    le_dictkeys_t *keys = (le_dictkeys_t *)mp->ma_keys;
    Py_ssize_t size = le_dict_size(keys);
    Py_hash_t hash = PyObject_Hash(name);
    size_t mask = (size_t)size - 1, perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;
    long ix, at, ind = offsetof(le_dictkeys_t, dk_indices);
    int k;
    if (hash == -1) {
        PyErr_Clear();
        return false;
    }
    /* keys of 2^24 slots or fewer: every offset fits a 32-bit field */
    if (mp->ma_values != NULL || size > (1L << 24))
        return false;
    a->shape = le_dict_shape(keys);
    a->hashed = le_dict_hashed(keys);
    a->width = size <= 0xff ? 1 : size <= 0xffff ? 2 : 4;
    /* a slot the probe comes back to is shown once: what it holds sends the
       probe on the same way each time */
    for (a->n = 0;;) {
        at = ind + i * a->width;
        ix = a->width == 1   ? ((int8_t *)keys->dk_indices)[i]
             : a->width == 2 ? ((int16_t *)keys->dk_indices)[i]
                             : ((int32_t *)keys->dk_indices)[i];
        if (ix == -1) {
            a->slot[a->n] = at;
            break; /* DKIX_EMPTY */
        }
        for (k = 0; k < a->n && a->slot[k] != at; k++);
        if (k == a->n) {
            if (a->n == GLOBAL_PROBE_MAX)
                return false;
            a->slot[a->n] = at;
            a->index[a->n] = ix;
            if (ix >= 0) {
                if ((int32_t)le_dict_hash(keys, ix) == (int32_t)hash)
                    return false;
                /* the word before the key, or the key */
                PyObject **key = le_dict_key_slot(keys, ix);
                a->hash_at[a->n] =
                    (char *)(a->hashed ? key - 1 : key) - (char *)keys;
                a->hash[a->n] = (int32_t)le_dict_hash(keys, ix);
            }
            a->n++;
        }
        perturb >>= 5; /* PERTURB_SHIFT */
        i = (i * 5 + perturb + 1) & mask;
    }
    return true;
}

/* The absent form from 'mov r10, globals' on, at 'code'; returns its end.
   With no 'absent', globals' tag 'gtag' stands for the probe:
     r10 = globals' ma_version_tag; mov r11, GTAG; cmp r11, r10; jne DONE
   which is never longer than a probe of no slots. Its jumps go to 'done',
   or to its end if NULL. The jumps are traced only with 'trace': the code
   may be written aside to be measured. */
static code_t *
global_absent_emit(code_t *code, code_t *done, PyObject *globals,
                   PyObject *builtins, const global_absent_t *absent,
                   uint64_t gtag, uint64_t tag, bool trace)
{
    code_t *jne[2 * GLOBAL_PROBE_MAX + 2];
    int i, nj = 0;
    long v;
#define JNE_DONE()                   \
    do {                             \
        jne[nj++] = code;            \
        WRITE_2(0x0F, 0x80 | CC_NE); \
        code += 4;                   \
    } while (0)
#if LE_HAS_DICT_VERSION_TAG
    MOV_R_IABS(REG_TRANSIENT_1, (long)globals);
    if (absent == NULL) {
        MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
                CPY_OFFSET(PyDictObject, ma_version_tag));
#else
    if (absent == NULL) {
        LOAD_DICT_TAG(MOV_R_IABS, REG_TRANSIENT_1, globals);
#endif
        MOV_R_UI(REG_TRANSIENT_2, (long)gtag);
        CMP_R_R(REG_TRANSIENT_2, REG_TRANSIENT_1);
        JNE_DONE();
    }
    else {
#if !LE_HAS_DICT_VERSION_TAG
        MOV_R_IABS(REG_TRANSIENT_1, (long)globals);
#endif
        MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
                CPY_OFFSET(PyDictObject, ma_keys));
        KEYS_SHAPE_CMP(absent->shape, REG_TRANSIENT_1);
        JNE_DONE();
    }
    for (i = 0; absent != NULL && i <= absent->n; i++) {
        v = i < absent->n ? absent->index[i] : -1;
        if (absent->width == 1)
            CMP_I8_BO(v, REG_TRANSIENT_1, absent->slot[i]);
        else if (absent->width == 2)
            CMP_I16_WO(v, REG_TRANSIENT_1, absent->slot[i]);
        else
            CMP_I32_DO(v, REG_TRANSIENT_1, absent->slot[i]);
        JNE_DONE();
        if (i < absent->n && v >= 0 && absent->hashed) {
            CMP_I32_DO(absent->hash[i], REG_TRANSIENT_1, absent->hash_at[i]);
            JNE_DONE();
        }
        else if (i < absent->n && v >= 0) {
            MOV_R_O(REG_TRANSIENT_2, REG_TRANSIENT_1, absent->hash_at[i]);
            CMP_I32_DO(absent->hash[i], REG_TRANSIENT_2,
                       CPY_OFFSET(PyASCIIObject, hash));
            JNE_DONE();
        }
    }
#undef JNE_DONE
    LOAD_DICT_TAG(MOV_R_IABS, REG_X64_RAX, builtins);
    MOV_R_UI(REG_TRANSIENT_1, (long)tag);
    CMP_R_R(REG_TRANSIENT_1, REG_X64_RAX);
    if (done == NULL)
        done = code;
    for (i = 0; i < nj; i++) {
        *(dword_t *)(jne[i] + 2) = REL32(jne[i] + 6, done);
        if (trace)
            le_trace_jump_cond(jne[i], jne[i] + 6, done);
    }
    return code;
}

DEFINEFN
condition_code_t
psyco_global_guard(PsycoObject *po, PyObject *globals, PyObject *builtins,
                   PyObject *name, PyObject *value, global_patch_t *entry,
                   long shape, long offset, const global_absent_t *absent,
                   global_absent_patch_t *patch, uint64_t *tags)
{
    int i;
    if (absent != NULL && !is_respawning(po) &&
        po->codelimit - po->code < GLOBAL_ABSENT_CODE_MAX)
        PsycoObject_EmergencyCodeRoom(po);
    BEGIN_CODE
    NEED_CC();
    NEED_REGISTER(REG_X64_RAX);
    if (absent != NULL) {
        patch->start = code;
        code = global_absent_emit(code, NULL, globals, builtins, absent, 0,
                                  tags[1], true);
        patch->end = code;
        patch->fresh = NULL;
        LE_RELOC_SLOT(&patch->start);
        LE_RELOC_SLOT_END(&patch->end);
    }
    else if (entry != NULL) {
        code_t *js[2];
        MOV_R_I(REG_TRANSIENT_1, (long)globals);
        MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
                CPY_OFFSET(PyDictObject, ma_keys));
        KEYS_SHAPE_CMP(shape, REG_TRANSIENT_1);
        entry->at_shape = (shape_imm_t *)code - 1;
        js[0] = code;
        WRITE_2(0x70 | CC_NE, 0);
        MOV_R_I(REG_X64_RAX, (long)name);
        CMP_R_O32(REG_X64_RAX, REG_TRANSIENT_1, offset);
        entry->at_key = (dword_t *)code - 1;
        js[1] = code;
        WRITE_2(0x70 | CC_NE, 0);
        MOV_R_I(REG_X64_RAX, (long)value);
        CMP_R_O32(REG_X64_RAX, REG_TRANSIENT_1, offset + sizeof(PyObject *));
        entry->at_value = (dword_t *)code - 1;
        LE_RELOC_SLOT(&entry->at_shape);
        LE_RELOC_SLOT(&entry->at_key);
        LE_RELOC_SLOT(&entry->at_value);
        for (i = 0; i < 2; i++) {
            extra_assert(code - (js[i] + 2) < 128);
            js[i][1] = (code_t)(code - (js[i] + 2));
            le_trace_jump_cond(js[i], js[i] + 2, code);
        }
    }
    else {
#if LE_HAS_DICT_VERSION_TAG
        MOV_R_I(REG_TRANSIENT_1, (long)globals);
        MOV_R_I(REG_TRANSIENT_2, (long)tags);
        MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
                CPY_OFFSET(PyDictObject, ma_version_tag));
#else
        LOAD_DICT_TAG(MOV_R_I, REG_TRANSIENT_1, globals);
        MOV_R_I(REG_TRANSIENT_2, (long)tags);
#endif
        XOR_R_O(REG_TRANSIENT_1, REG_TRANSIENT_2, 0);
        LOAD_DICT_TAG(MOV_R_I, REG_X64_RAX, builtins);
        XOR_R_O(REG_X64_RAX, REG_TRANSIENT_2, sizeof(uint64_t));
        OR_R_R(REG_X64_RAX, REG_TRANSIENT_1);
    }
    END_CODE
    return CC_NE;
}

/* The guard is written so that it ends at the end of its room, where the
   code goes on, and its start jumps over what is left of the room before
   it: in place, nothing or one jump more on the pass path; in fresh
   memory, a jump there and a jump back. A probe it can't show, or fresh
   memory out of reach, gives globals' tag in place of the probe, which
   fits in place. The code is entered only at 'start', and no thread is
   in it while the GIL is held here. */
DEFINEFN
void
psyco_global_absent_rearm(global_absent_patch_t *patch, PyObject *globals,
                          PyObject *builtins, PyObject *name)
{
    global_absent_t a, *ap = &a;
    code_t scratch[GLOBAL_ABSENT_CODE_MAX];
    code_t *code, *at, *end = patch->end;
    uint64_t tag = *LE_DICT_TAG_ADDRESS(builtins);
    uint64_t gtag = *LE_DICT_TAG_ADDRESS(globals);
    long n;
    if (!psyco_global_absent(globals, name, &a))
        ap = NULL;
    n = global_absent_emit(scratch, NULL, globals, builtins, ap, gtag, tag,
                           false) -
        scratch;
    extra_assert(n <= GLOBAL_ABSENT_CODE_MAX - SIZE_OF_FAR_JUMP);
    if (n > patch->end - patch->start && patch->fresh == NULL) {
        code_t *fresh = psyco_code_take(GLOBAL_ABSENT_CODE_MAX);
        if (FITS_IN_32BITS(fresh - patch->end) &&
            FITS_IN_32BITS(fresh - patch->start)) {
            patch->fresh = fresh;
            LE_RELOC_SLOT(&patch->fresh);
            code = fresh + GLOBAL_ABSENT_CODE_MAX - SIZE_OF_FAR_JUMP;
            le_trace_begin_simple_code(code);
            JUMP_TO(patch->end);
            le_trace_end_simple_code(code);
        }
        else { /* the memory stays the owner's, unused */
            ap = NULL;
            n = global_absent_emit(scratch, NULL, globals, builtins, ap, gtag,
                                   tag, false) -
                scratch;
        }
    }
    if (n > patch->end - patch->start)
        end = patch->fresh + GLOBAL_ABSENT_CODE_MAX - SIZE_OF_FAR_JUMP;
    at = end - n;
    code = at;
    le_trace_begin_simple_code(code);
    code =
        global_absent_emit(code, end, globals, builtins, ap, gtag, tag, true);
    le_trace_end_simple_code(code);
    extra_assert(code == end);
    /* the start goes to the guard, over the rest of the room */
    code = patch->start;
    if (at != code) {
        le_trace_begin_simple_code(code);
        if (at - code >= SIZE_OF_FAR_JUMP || end != patch->end)
            JUMP_TO(at);
        else if (at - code >= 2) {
            le_trace_jump(code, at);
            WRITE_2(0xEB, (code_t)(at - (code + 2)));
        }
        else
            WRITE_1(0x90); /* nop */
        le_trace_end_simple_code(code);
    }
}

DEFINEFN
void
psyco_global_guard_move(global_patch_t *entry, long shape, long offset)
{
    *entry->at_shape = (shape_imm_t)shape;
    *entry->at_key = (dword_t)offset;
    if (entry->at_value != NULL)
        *entry->at_value = (dword_t)(offset + sizeof(PyObject *));
}

/*   mov r10, globals; mov r10, [r10 + ma_keys]
     cmp [r10 + shape], SHAPE; jne STUB
     mov r11, name; cmp [r10 + KEY], r11; jne STUB
   Only the transient registers and the flags, so the state is the one the
   code was entered with. */
DEFINEFN
void
psyco_global_stale(PsycoObject *po, PyObject *globals, PyObject *name,
                   global_patch_t *check, long shape, long offset,
                   code_t *stub)
{
    extra_assert(!HAS_CCREG(po));
    if (po->codelimit - po->code < 64)
        PsycoObject_EmergencyCodeRoom(po);
    BEGIN_CODE
    MOV_R_I(REG_TRANSIENT_1, (long)globals);
    MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
            CPY_OFFSET(PyDictObject, ma_keys));
    KEYS_SHAPE_CMP(shape, REG_TRANSIENT_1);
    check->at_shape = (shape_imm_t *)code - 1;
    FAR_COND_JUMP_TO(stub, CC_NE);
    MOV_R_I(REG_TRANSIENT_2, (long)name);
    CMP_R_O32(REG_TRANSIENT_2, REG_TRANSIENT_1, offset);
    check->at_key = (dword_t *)code - 1;
    FAR_COND_JUMP_TO(stub, CC_NE);
    check->at_value = NULL;
    LE_RELOC_SLOT(&check->at_shape);
    LE_RELOC_SLOT(&check->at_key);
    END_CODE
}

/*   mov r10, globals; mov r10, [r10 + ma_keys]
     cmp [r10 + shape], SHAPE; jne MISS
     cmp [r10 + KEY], name; jne MISS
     RG = [r10 + KEY + 8]
     (where there are lazy imports: cmp [RG + ob_type], proxy type; je MISS)
     incref RG; jmp DONE
   MISS: rdi = arg; call miss_glue; RG = rax
   DONE: test RG, RG
   Hand-allocated like psyco_inline_load, RG callee-saved, so the
   compiler's state is the same on both paths and whatever SHAPE and KEY
   are. */
DEFINEFN
vinfo_t *
psyco_thread_word_load(PsycoObject *po, long offset)
{
    vinfo_t *v;
    reg_t rg;
    BEGIN_CODE
    NEED_FREE_REG(rg);
    MOV_R_FS(rg, offset);
    END_CODE
    v = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = v;
    return v;
}

DEFINEFN
vinfo_t *
psyco_global_load(PsycoObject *po, PyObject *globals, PyObject *name,
                  global_patch_t *entry, long shape, long offset,
                  code_t *miss_glue, void *arg)
{
    code_t *jmiss[3], *jdone, *start;
    vinfo_t *v;
    reg_t rg;
    int i, nmiss = 2;
    if (!is_respawning(po) && po->codelimit - po->code < 128)
        PsycoObject_EmergencyCodeRoom(po);
    BEGIN_CODE
    NEED_CC();
    NEED_REGISTER(REG_X64_RAX);
    NEED_REGISTER(REG_X64_RCX);
    NEED_REGISTER(REG_X64_RDX);
    NEED_REGISTER(REG_X64_RSI);
    NEED_REGISTER(REG_X64_RDI);
    NEED_REGISTER(REG_X64_R8);
    NEED_REGISTER(REG_X64_R9);
    NEED_FREE_REG(rg); /* callee-saved: the miss call keeps it */
    start = code;
    MOV_R_I(REG_TRANSIENT_1, (long)globals);
    MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
            CPY_OFFSET(PyDictObject, ma_keys));
    KEYS_SHAPE_CMP(shape, REG_TRANSIENT_1);
    entry->at_shape = (shape_imm_t *)code - 1;
    jmiss[0] = code;
    WRITE_2(0x70 | CC_NE, 0);
    MOV_R_I(REG_X64_RAX, (long)name);
    CMP_R_O32(REG_X64_RAX, REG_TRANSIENT_1, offset);
    entry->at_key = (dword_t *)code - 1;
    jmiss[1] = code;
    WRITE_2(0x70 | CC_NE, 0);
    MOV_R_O32(rg, REG_TRANSIENT_1, offset + sizeof(PyObject *));
    entry->at_value = (dword_t *)code - 1;
    LE_RELOC_SLOT(&entry->at_shape);
    LE_RELOC_SLOT(&entry->at_key);
    LE_RELOC_SLOT(&entry->at_value);
#if LE_HAS_LAZY_IMPORT_REIFY_LINKABLE
    /* a proxy is the miss's to replace with its module */
    MOV_R_I(REG_X64_RAX, (long)&PyLazyImport_Type);
    CMP_R_O8(REG_X64_RAX, rg, offsetof(PyObject, ob_type));
    jmiss[nmiss++] = code;
    WRITE_2(0x70 | CC_E, 0);
#endif
    INC_OB_REFCNT_internal(rg);
    jdone = code;
    WRITE_2(0xEB, 0);
    for (i = 0; i < nmiss; i++) {
        extra_assert(code - (jmiss[i] + 2) < 128);
        jmiss[i][1] = (code_t)(code - (jmiss[i] + 2));
        le_trace_jump_cond(jmiss[i], jmiss[i] + 2, code);
    }
    MOV_R_I(REG_X64_RDI, (long)arg);
    CALL_GLUE(miss_glue, start);
    MOV_R_R(rg, REG_X64_RAX);
    extra_assert(code - (jdone + 2) < 128);
    jdone[1] = (code_t)(code - (jdone + 2));
    le_trace_jump(jdone, code);
    TEST_R_R(rg, rg);
    END_CODE
    v = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = v;
    return v;
}

#if !LE_HAS_LONG_VALUE
DEFINEFN vinfo_t *
psyco_long_size_times_digit(PsycoObject *po, vinfo_t *v, vinfo_t *size)
{
    vinfo_t *r;
    reg_t rg, base;
    BEGIN_CODE
    NEED_CC();
    COPY_IN_REG(size, rg);
    if (is_runtime(v->source) && !RUNTIME_REG_IS_NONE(v))
        base = RUNTIME_REG(v);
    else {
        base = REG_TRANSIENT_1;
        LOAD_REG_FROM(v->source, base);
    }
    TEST_R_R(rg, rg);
    BEGIN_SHORT_COND_JUMP(0, CC_E);
    MOV_DR_O(REG_TRANSIENT_1, base, CPY_OFFSET(PyLongObject, ob_digit));
    IMUL_R_R(rg, REG_TRANSIENT_1);
    END_SHORT_JUMP(0);
    END_CODE
    r = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = r;
    return r;
}
#endif

#if LE_HAS_FRAME_STACKREFS
_Static_assert(Py_TAG_REFCNT == 1 && offsetof(_PyStackRef, bits) == 0 &&
                   sizeof(_PyStackRef) == sizeof(long),
               "LeJIT: a _PyStackRef is a word whose tag is its low bit");

DEFINEFN vinfo_t *
psyco_stackref_steal(PsycoObject *po, vinfo_t *w)
{
    vinfo_t *v;
    reg_t rg;
    BEGIN_CODE
    NEED_CC();
    COPY_IN_REG(w, rg);
    TEST_R_I(rg, Py_TAG_REFCNT);
    BEGIN_SHORT_COND_JUMP(0, CC_E);
    AND_R_I8(rg, ~Py_TAG_REFCNT);
    /* PyStackRef_NULL, now zero */
    BEGIN_SHORT_COND_JUMP(1, CC_E);
    INC_OB_REFCNT_internal(rg);
    END_SHORT_JUMP(1);
    END_SHORT_JUMP(0);
    END_CODE
    v = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = v;
    return v;
}

/* the object in 'rg' (not NULL) tagged as an immortal is */
#define STACKREF_TAG(rg)                                                \
    do {                                                                \
        MOVZX_DR_WO(REG_TRANSIENT_1, rg, offsetof(PyObject, ob_flags)); \
        AND_R_I8(REG_TRANSIENT_1, Py_TAG_REFCNT);                       \
        OR_R_R(rg, REG_TRANSIENT_1);                                    \
    } while (0)

DEFINEFN vinfo_t *
psyco_stackref_from_steal(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *w;
    reg_t rg;
    if (is_compiletime(v->source)) {
        PyObject *o = (PyObject *)CompileTime_Get(v->source)->value;
        return vinfo_new(CompileTime_New(
            o == NULL ? (long)PyStackRef_NULL.bits
                      : (long)PyStackRef_FromPyObjectSteal(o).bits));
    }
    BEGIN_CODE
    NEED_CC();
    COPY_IN_REG(v, rg);
    if (is_maybe_null(v->source)) {
        TEST_R_R(rg, rg);
        BEGIN_SHORT_COND_JUMP(0, CC_NE);
        MOV_R_I(rg, PyStackRef_NULL.bits);
        BEGIN_SHORT_JUMP(1);
        END_SHORT_JUMP(0);
        STACKREF_TAG(rg);
        END_SHORT_JUMP(1);
    }
    else
        STACKREF_TAG(rg);
    END_CODE
    w = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = w;
    return w;
}
#endif
