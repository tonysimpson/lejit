#include "ipyencoding.h"
#include "../pycodegen.h"
#include "../codemanager.h"
/* LE_GC_UNTRACKED_TEST */
#include "../mechanisms/store_gc.h"

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
    if (RBP_IS_RESERVED) {
        pre_return_stack_correction += sizeof(long);
    }
    STACK_CORRECTION(pre_return_stack_correction);
    po->stack_depth += pre_return_stack_correction;
    if (RBP_IS_RESERVED) {
        POP_R(REG_X64_RBP);
        psyco_dec_stackdepth(po);
    }
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

DEFINEFN
code_t *
decref_dealloc_calling(code_t *code, PsycoObject *po, reg_t rg, destructor fn)
{
    extra_assert(CPY_OFFSET(PyObject, ob_type) < 128);
    extra_assert(CPY_OFFSET(PyTypeObject, tp_dealloc) < 128);
#if LE_HAS_IMMORTAL_OBJECTS
    /* as Py_DECREF, an immortal (its count's low 32 bits negative) is left
       alone */
    NEED_CC_REG(rg);
    CMP_I8_DA(0, rg);
    BEGIN_SHORT_COND_JUMP(1, CC_S);
    LE_SUB_OB_REFCNT(rg);
#else
    DEC_OB_REFCNT_NZ(rg);
#endif
    BEGIN_SHORT_COND_JUMP(0, CC_NE); /* NE is the not zero flag */
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
    END_SHORT_JUMP(0);
#if LE_HAS_IMMORTAL_OBJECTS
    END_SHORT_JUMP(1);
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
   dict and no version tag: R8 is read from the object, or is the object
   itself where the values are in it (LE_HAS_INLINE_VALUES), R9 is the
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

/* Which encodings reach: decided once from where the site starts, with a
   margin, so that both passes emit the same lengths. */
static bool
rip_near(code_t *at, void *addr)
{
    long d = (long)addr - (long)at;
    return -0x7f000000L < d && d < 0x7f000000L;
}
/* the address of a constant in a register: 5 bytes below 4 GB, else 7
   (RIP-relative) or 10 */
#define LOAD_ADDRESS(rg, addr, near)                        \
    do {                                                    \
        if ((unsigned long)(addr) > 0xFFFFFFFFUL && (near)) \
            LEA_R_RIP(rg, (long)(addr) - ((long)code + 7)); \
        else                                                \
            MOV_R_I(rg, (long)(addr));                      \
    } while (0)
#define CALL_GLUE(fn, near)                                \
    do {                                                   \
        if (near) {                                        \
            le_trace_call((code_t *)code, (code_t *)(fn)); \
            WRITE_1(0xE8);                                 \
            WRITE_32BIT((long)(fn) - ((long)code + 4));    \
        }                                                  \
        else {                                             \
            MOV_R_I(REG_TRANSIENT_1, (long)(fn));          \
            CALL_R(REG_TRANSIENT_1);                       \
        }                                                  \
    } while (0)
static void
load_operand(PsycoObject *po, code_t **pcode, Source src, reg_t rg,
             code_t *start)
{
    code_t *code = *pcode;
    if (is_compiletime(src)) {
        long k = KSOURCE_SOURCE(src)->value;
        LOAD_ADDRESS(rg, k, rip_near(start, (void *)k));
    }
    else {
        LOAD_REG_FROM(src, rg);
    }
    *pcode = code;
}

static code_t *
inline_store_body(PsycoObject *po, code_t *code, inline_store_t *s,
                  const bool *near, code_t **jat, int *pnj, code_t **pmiss)
{
    code_t *start = code;
    int nj = 0;
    code_t *fill_jz = NULL, *gc_ok = NULL, *store;
    long values_at = 0; /* the values' array, from R8 */
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
    load_operand(po, &code, s->obj->source, REG_X64_RDI, start);
    load_operand(po, &code, s->value->source, REG_X64_RSI, start);
    LOAD_ADDRESS(REG_X64_RDX, s->cache, rip_near(start, s->cache));
    if (IN_VALUES(s)) {
#if LE_HAS_INLINE_VALUES
        TEST_I8_BO(REG_X64_RDI, s->dictoffset + LE_VALUES_VALID, 1);
        JCC_MISS(CC_E); /* no longer its attributes */
        MOV_R_O(REG_X64_RAX, REG_X64_RDI, LE_MANAGED_DICT_OFFSET);
        TEST_R_R(REG_X64_RAX, REG_X64_RAX);
        JCC_MISS(CC_NE); /* a dict was made, which may be watched */
        MOV_R_R(REG_X64_R8, REG_X64_RDI);
        values_at = s->dictoffset + LE_VALUES_ARRAY;
#else
        MOV_R_O(REG_X64_R8, REG_X64_RDI, s->dictoffset);
        TEST_R_R(REG_X64_R8, REG_X64_R8);
        JCC_MISS(CC_E); /* the dict was made */
#endif
        LOAD_ADDRESS(REG_X64_R9, s->keys, rip_near(start, s->keys));
    }
    else {
        MOV_R_O(REG_X64_RCX, REG_X64_RDI, s->dictoffset);
        TEST_R_R(REG_X64_RCX, REG_X64_RCX);
        JCC_MISS(CC_E); /* no dict yet */
        MOV_R_O(REG_X64_R8, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_values));
        TEST_R_R(REG_X64_R8, REG_X64_R8);
        JCC_MISS(CC_E); /* combined */
#if LE_HAS_DICT_WATCHER_BITS
        TEST_I8_BO(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag),
                   LE_DICT_WATCHER_MASK);
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
#if LE_HAS_DICT_VERSION_TAG
    if (!IN_VALUES(s)) { /* a dict's new version tag */
        MOV_R_O(REG_TRANSIENT_1, REG_X64_RDX,
                offsetof(store_cache_t, version));
#if LE_HAS_DICT_WATCHER_BITS
        MOV_R_A(REG_TRANSIENT_2, REG_TRANSIENT_1);
        ADD_R_I32(REG_TRANSIENT_2, LE_DICT_VERSION_STEP);
        MOV_A_R(REG_TRANSIENT_1, REG_TRANSIENT_2);
        MOV_O_R(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag),
                REG_TRANSIENT_2);
#else
        INC_A(REG_TRANSIENT_1);
        MOV_R_A(REG_TRANSIENT_1, REG_TRANSIENT_1);
        MOV_O_R(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag),
                REG_TRANSIENT_1);
#endif
    }
#endif
#if LE_HAS_IMMORTAL_OBJECTS
    CMP_I8_DA(0, REG_X64_RAX); /* an immortal is left alone */
    s->done[2] = code;
    WRITE_2(0x70 | CC_S, 0);
#endif
    DEC_A(REG_X64_RAX); /* last: may run any code */
    s->done[0] = code;
    WRITE_2(0x70 | CC_NE, 0);
    CALL_GLUE(psyco_dealloc_rax_glue, rip_near(start, psyco_dealloc_rax_glue));
    s->done[1] = code;
    WRITE_2(0xEB, 0);
    if (!s->fill)
        goto miss;
    extra_assert(code - (fill_jz + 2) < 128);
    fill_jz[1] = (code_t)(code - (fill_jz + 2));
    le_trace_jump_cond(fill_jz, fill_jz + 2, code);
    if (IN_VALUES(s)) {
        /* the order's length is the byte below the values, its entries the
           bytes below that, the first nearest; the values have room for
           every entry of the keys */
        MOVZX_DR_BO(REG_X64_RCX, REG_X64_R8, -2);
        ADD_R_I8(REG_X64_RCX, 1);
        MOV_BO_R(REG_X64_R8, -2, REG_X64_RCX);
        UNARY_INSTR_ON_REG(3, REG_X64_RCX); /* neg */
        MOV_BX8_R(REG_X64_R8, -2, REG_X64_RCX, 1, REG_X64_R9);
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
miss:
    *pmiss = code;
    CALL_GLUE(s->miss_glue, rip_near(start, s->miss_glue));
    TEST_DR_DR(REG_X64_RAX, REG_X64_RAX);
    extra_assert(nj <= STORE_MAX_MISS_JUMPS);
    *pnj = nj;
    return code;
#undef JCC_MISS
}

DEFINEFN
void
psyco_inline_store_begin(PsycoObject *po, inline_store_t *s)
{
    bool near[STORE_MAX_MISS_JUMPS];
    code_t *jat[STORE_MAX_MISS_JUMPS], *miss, *start;
    int i, nj;
    extra_assert(!is_virtualtime(s->obj->source) &&
                 !is_virtualtime(s->value->source));
    if (!is_respawning(po) && po->codelimit - po->code < 512)
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
    for (i = 0; i < STORE_MAX_MISS_JUMPS; i++) near[i] = true;
    inline_store_body(po, start, s, near, jat, &nj, &miss);
    for (i = 0; i < nj; i++) /* each later jump made short shortens it */
        near[i] = miss - jat[i] - 6 > 127;
    code = inline_store_body(po, start, s, near, jat, &nj, &miss);
    for (i = 0; i < nj; i++) {
        if (near[i]) {
            *(dword_t *)(jat[i] + 2) = miss - (jat[i] + 6);
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
        psyco_assert(po->code - (s->done[i] + 2) < 128);
        s->done[i][1] = (code_t)(po->code - (s->done[i] + 2));
        le_trace_jump(s->done[i], po->code);
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
#define LOAD_MAX_JUMPS 8
DEFINEFN
vinfo_t *
psyco_inline_load(PsycoObject *po, inline_load_t *s)
{
    code_t *jmiss[LOAD_MAX_JUMPS], *jnone[2], *jdone[3], *start, *at;
    int nmiss = 0, nnone = 0, ndone = 0, i;
    vinfo_t *v;
    reg_t rg, ro;
#define NEAR_JCC(cc, list, n)       \
    do {                            \
        list[n++] = code;           \
        WRITE_2(0x0F, 0x80 | (cc)); \
        code += 4;                  \
    } while (0)
#define NEAR_JMP(list, n) \
    do {                  \
        list[n++] = code; \
        WRITE_1(0xE9);    \
        code += 4;        \
    } while (0)
#define POINT(list, n, target)                          \
    for (i = 0; i < (n); i++) {                         \
        at = list[i];                                   \
        if (at[0] == 0xE9) {                            \
            *(dword_t *)(at + 1) = (target) - (at + 5); \
            le_trace_jump(at, (target));                \
        }                                               \
        else {                                          \
            *(dword_t *)(at + 2) = (target) - (at + 6); \
            le_trace_jump_cond(at, at + 6, (target));   \
        }                                               \
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
    extra_assert(!is_virtualtime(s->obj->source));
    if (!is_respawning(po) && po->codelimit - po->code < 256)
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
    if (is_runtime(s->obj->source) && !RUNTIME_REG_IS_NONE(s->obj)) {
        ro = RUNTIME_REG(s->obj);
    }
    else {
        ro = REG_X64_RDI;
        load_operand(po, &code, s->obj->source, ro, start);
    }
    if (IN_VALUES(s)) {
#if LE_MANAGED_VALUES
        code_t *jpresent;
        long values_at = 0;       /* the values' array, from R8 */
        MOV_R_R(REG_X64_RCX, ro); /* for the miss call */
#if LE_HAS_INLINE_VALUES
        TEST_I8_BO(ro, s->dictoffset + LE_VALUES_VALID, 1);
        NEAR_JCC(CC_E, jmiss, nmiss); /* no longer its attributes */
        MOV_R_R(REG_X64_R8, ro);
        values_at = s->dictoffset + LE_VALUES_ARRAY;
#else
        MOV_R_O(REG_X64_R8, ro, s->dictoffset);
        TEST_R_R(REG_X64_R8, REG_X64_R8);
        NEAR_JCC(CC_E, jmiss, nmiss); /* the dict was made */
#endif
        LOAD_ADDRESS(REG_X64_R9, s->keys, rip_near(start, s->keys));
        LOAD_ADDRESS(REG_X64_RDX, s->cache, rip_near(start, s->cache));
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
        NEAR_JCC(CC_E, jmiss, nmiss); /* combined */
        MOV_R_O(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_keys));
        LOAD_ADDRESS(REG_X64_RDX, s->cache, rip_near(start, s->cache));
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
        MOV_R_I(REG_X64_RDI, s->hash & 127);
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
        MOV_R_I(REG_X64_RDX, s->hash);
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
    POINT(jmiss, nmiss, code);
    MOV_R_R(REG_X64_RDI, REG_X64_RCX);
    LOAD_ADDRESS(REG_X64_RSI, s->cache, rip_near(start, s->cache));
    CALL_GLUE(s->miss_glue, rip_near(start, s->miss_glue));
    MOV_R_R(rg, REG_X64_RAX);
    POINT(jdone, ndone, code);
    END_CODE
    v = vinfo_new(RunTime_New(rg, false, false));
    REG_NUMBER(po, rg) = v;
    return v;
#undef NEAR_JCC
#undef NEAR_JMP
#undef POINT
#undef SHORT_BACK
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
#define LOAD_DICT_TAG(r, d)                                      \
    do {                                                         \
        MOV_R_I(r, (long)(d));                                   \
        MOV_R_O(r, r, CPY_OFFSET(PyDictObject, ma_version_tag)); \
    } while (0)
#else
#define LE_DICT_TAG_ADDRESS(d) le_dict_counter((PyDictObject *)(d))
#define LOAD_DICT_TAG(r, d)                       \
    do {                                          \
        MOV_R_I(r, (long)LE_DICT_TAG_ADDRESS(d)); \
        MOV_R_A(r, r);                            \
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
    MOV_R_I(REG_TRANSIENT_1, (long)globals);
    if (absent == NULL) {
        MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
                CPY_OFFSET(PyDictObject, ma_version_tag));
#else
    if (absent == NULL) {
        LOAD_DICT_TAG(REG_TRANSIENT_1, globals);
#endif
        MOV_R_UI(REG_TRANSIENT_2, (long)gtag);
        CMP_R_R(REG_TRANSIENT_2, REG_TRANSIENT_1);
        JNE_DONE();
    }
    else {
#if !LE_HAS_DICT_VERSION_TAG
        MOV_R_I(REG_TRANSIENT_1, (long)globals);
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
                       offsetof(PyASCIIObject, hash));
            JNE_DONE();
        }
    }
#undef JNE_DONE
    LOAD_DICT_TAG(REG_X64_RAX, builtins);
    MOV_R_UI(REG_TRANSIENT_1, (long)tag);
    CMP_R_R(REG_TRANSIENT_1, REG_X64_RAX);
    if (done == NULL)
        done = code;
    for (i = 0; i < nj; i++) {
        *(dword_t *)(jne[i] + 2) = done - (jne[i] + 6);
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
        LOAD_DICT_TAG(REG_TRANSIENT_1, globals);
        MOV_R_I(REG_TRANSIENT_2, (long)tags);
#endif
        XOR_R_O(REG_TRANSIENT_1, REG_TRANSIENT_2, 0);
        LOAD_DICT_TAG(REG_X64_RAX, builtins);
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
    END_CODE
}

/*   mov r10, globals; mov r10, [r10 + ma_keys]
     cmp [r10 + shape], SHAPE; jne MISS
     cmp [r10 + KEY], name; jne MISS
     RG = [r10 + KEY + 8]; incref RG; jmp DONE
   MISS: rdi = arg; call miss_glue; RG = rax
   DONE: test RG, RG
   Hand-allocated like psyco_inline_load, RG callee-saved, so the
   compiler's state is the same on both paths and whatever SHAPE and KEY
   are. */
DEFINEFN
vinfo_t *
psyco_global_load(PsycoObject *po, PyObject *globals, PyObject *name,
                  global_patch_t *entry, long shape, long offset,
                  code_t *miss_glue, void *arg)
{
    code_t *jmiss[2], *jdone, *start;
    vinfo_t *v;
    reg_t rg;
    int i;
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
    INC_OB_REFCNT_internal(rg);
    jdone = code;
    WRITE_2(0xEB, 0);
    for (i = 0; i < 2; i++) {
        extra_assert(code - (jmiss[i] + 2) < 128);
        jmiss[i][1] = (code_t)(code - (jmiss[i] + 2));
        le_trace_jump_cond(jmiss[i], jmiss[i] + 2, code);
    }
    LOAD_ADDRESS(REG_X64_RDI, arg, rip_near(start, arg));
    CALL_GLUE(miss_glue, rip_near(start, miss_glue));
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
