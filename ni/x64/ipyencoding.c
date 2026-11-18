#include "ipyencoding.h"
#include "../pycodegen.h"
/* NI_GC_UNTRACKED_TEST (D77) */
#include "mechanism_store_gc_gen.h"

DEFINEFN void function_return(PsycoObject *po, Source retval, int nframelocal,
                              long retpos) {
  int post_return_stack_depth = 0;
  int pre_return_stack_correction = 0;
  int return_depth = sizeof(long) + INITIAL_STACK_DEPTH;
  BEGIN_CODE
  /* load the return value into EAX for regular functions, EBX for functions
   * with a prologue */
  if (retval != SOURCE_DUMMY) {
    reg_t rg = nframelocal > 0 ? REG_ANY_CALLEE_SAVED : REG_FUNCTIONS_RETURN;
    LOAD_REG_FROM(retval, rg);
  }

  if (nframelocal > 0) {
    /* psyco_emit_header() was used; first clear the stack only up to and not
     * including the frame-local data */
    int framelocpos = getstack(LOC_CONTINUATION->array->items[0]->source);
    STACK_CORRECTION(framelocpos - po->stack_depth);
    po->stack_depth = framelocpos;
    /* perform Python-specific cleanup */
    FINALIZE_FRAME_LOCALS();
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
  } else {
    RET_N(post_return_stack_depth);
  }
  po->stack_depth = 0;

  END_CODE
}

DEFINEFN
code_t *decref_dealloc_calling(code_t *code, PsycoObject *po, reg_t rg,
                               destructor fn) {
  extra_assert(CPY_OFFSET(PyObject, ob_type) < 128);
  extra_assert(CPY_OFFSET(PyTypeObject, tp_dealloc) < 128);
  DEC_OB_REFCNT_NZ(rg);
  BEGIN_SHORT_COND_JUMP(0, CC_NE); /* NE is the not zero flag */
  if (fn == NULL) {
    BEGIN_CALL(1);
    CALL_SET_ARG_FROM_REG(rg, 0);
    MOV_R_O8(REG_TRANSIENT_1, rg, CPY_OFFSET(PyObject, ob_type));
    MOV_R_O8(REG_TRANSIENT_1, REG_TRANSIENT_1,
             CPY_OFFSET(PyTypeObject, tp_dealloc));
    END_CALL_R(REG_TRANSIENT_1);
  } else {
    BEGIN_CALL(1);
    CALL_SET_ARG_FROM_REG(rg, 0);
    END_CALL_I(fn);
  }
  END_SHORT_JUMP(0);
  return code;
}

DEFINEFN
void decref_create_new_ref(PsycoObject *po, vinfo_t *w) {
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
bool decref_create_new_lastref(PsycoObject *po, vinfo_t *w) {
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

/* Inline attribute store (ipyencoding.h; STORES.md "Shrinking the store
   sites"). Registers: RDI the object, RSI the value, RDX the cache, RCX
   the dict, R8 its values, R9 its keys then the index, RAX scratch then
   the old value, R10 scratch. Laid out as

       checks ............ jcc MISS (each failure)
       old = values[ix]; test; jz FILL
     STORE:  values[ix] = value; incref value; new version tag
             dec old; jnz DONE; call dealloc_glue; jmp DONE
     FILL:   ix == ma_used, else MISS; ma_used++; old = value; jmp STORE
     MISS:   call miss_glue; test eax, eax
             (the caller's error check: jne <respawn>)
     DONE:

   FILL makes the old value the new one, increfed once more, so STORE's
   decref can't reach zero. FILL is only where filling the next slot in
   order is likely (s->fill: stores to self in __init__); elsewhere an
   empty slot is a miss, which the helper stores as CPython does. The checks come before any write, and nothing
   before the old value's decref runs Python code. The jumps to MISS are
   short where they reach: the body is emitted twice, first with every
   one near, which gives an upper bound on each distance. */
_Static_assert(Py_TPFLAGS_HAVE_GC == 1UL << 14,
               "the value's GC flag is tested in tp_flags' second byte");
_Static_assert(offsetof(store_cache_t, dk_size) == 0,
               "the size check compares [cache] directly");
#define STORE_MAX_MISS_JUMPS 8

/* Which encodings reach: decided once from where the site starts, with a
   margin, so that both passes emit the same lengths. */
static bool rip_near(code_t *at, void *addr) {
  long d = (long)addr - (long)at;
  return -0x7f000000L < d && d < 0x7f000000L;
}
/* the address of a constant in a register: 5 bytes below 4 GB, else 7
   (RIP-relative) or 10 */
#define LOAD_ADDRESS(rg, addr, near)                                           \
  do {                                                                         \
    if ((unsigned long)(addr) > 0xFFFFFFFFUL && (near))                        \
      LEA_R_RIP(rg, (long)(addr) - ((long)code + 7));                          \
    else                                                                       \
      MOV_R_I(rg, (long)(addr));                                               \
  } while (0)
#define CALL_GLUE(fn, near)                                                    \
  do {                                                                         \
    if (near) {                                                                \
      ni_trace_call((code_t *)code, (code_t *)(fn));                           \
      WRITE_1(0xE8);                                                           \
      WRITE_32BIT((long)(fn) - ((long)code + 4));                              \
    } else {                                                                   \
      MOV_R_I(REG_TRANSIENT_1, (long)(fn));                                    \
      CALL_R(REG_TRANSIENT_1);                                                 \
    }                                                                          \
  } while (0)
static void load_operand(PsycoObject *po, code_t **pcode, Source src,
                         reg_t rg, code_t *start) {
  code_t *code = *pcode;
  if (is_compiletime(src)) {
    long k = KSOURCE_SOURCE(src)->value;
    LOAD_ADDRESS(rg, k, rip_near(start, (void *)k));
  } else {
    LOAD_REG_FROM(src, rg);
  }
  *pcode = code;
}

static code_t *inline_store_body(PsycoObject *po, code_t *code,
                                 inline_store_t *s, const bool *near,
                                 code_t **jat, int *pnj, code_t **pmiss) {
  code_t *start = code;
  int nj = 0;
  code_t *fill_jz = NULL, *gc_ok = NULL, *store;
#define JCC_MISS(cc)                                                           \
  do {                                                                         \
    jat[nj] = code;                                                            \
    if (near[nj]) {                                                            \
      WRITE_2(0x0F, 0x80 | (cc));                                              \
      code += 4;                                                               \
    } else {                                                                   \
      WRITE_2(0x70 | (cc), 0);                                                 \
    }                                                                          \
    nj++;                                                                      \
  } while (0)
  load_operand(po, &code, s->obj->source, REG_X64_RDI, start);
  load_operand(po, &code, s->value->source, REG_X64_RSI, start);
  LOAD_ADDRESS(REG_X64_RDX, s->cache, rip_near(start, s->cache));
  MOV_R_O(REG_X64_RCX, REG_X64_RDI, s->dictoffset);
  TEST_R_R(REG_X64_RCX, REG_X64_RCX);
  JCC_MISS(CC_E); /* no dict yet */
  MOV_R_O(REG_X64_R8, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_values));
  TEST_R_R(REG_X64_R8, REG_X64_R8);
  JCC_MISS(CC_E); /* combined */
  MOV_R_O(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_keys));
  MOV_R_O(REG_X64_RAX, REG_X64_R9, CPY_OFFSET(ni_dictkeys_t, dk_size));
  CMP_R_A(REG_X64_RAX, REG_X64_RDX);
  JCC_MISS(CC_NE); /* keys of another size */
  MOV_R_O(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, kword));
  MOV_R_X8(REG_X64_RAX, REG_X64_R9, 0, REG_X64_RAX, 8);
  CMP_R_O8(REG_X64_RAX, REG_X64_RDX, offsetof(store_cache_t, name));
  JCC_MISS(CC_NE); /* the name isn't at the cached entry */
  MOV_R_O(REG_X64_R9, REG_X64_RDX, offsetof(store_cache_t, ix));
  if (s->gc != STORE_GC_NONE) { /* MAINTAIN_TRACKING */
    NI_GC_UNTRACKED_TEST(REG_X64_RCX); /* the store_gc mechanism */
    if (s->gc == STORE_GC_DICT) {
      JCC_MISS(CC_E);
    } else {
      gc_ok = code;
      WRITE_2(0x70 | CC_NE, 0); /* the dict is tracked */
      MOV_R_O(REG_TRANSIENT_1, REG_X64_RSI, CPY_OFFSET(PyObject, ob_type));
      TEST_I8_BO(REG_TRANSIENT_1, CPY_OFFSET(PyTypeObject, tp_flags) + 1,
                 Py_TPFLAGS_HAVE_GC >> 8);
      JCC_MISS(CC_NE);
      gc_ok[1] = (code_t)(code - (gc_ok + 2));
    }
  }
  MOV_R_X8(REG_X64_RAX, REG_X64_R8, 0, REG_X64_R9, 8);
  TEST_R_R(REG_X64_RAX, REG_X64_RAX);
  if (s->fill) {
    fill_jz = code;
    WRITE_2(0x70 | CC_E, 0);
  } else {
    JCC_MISS(CC_E); /* an empty slot */
  }
  store = code;
  MOV_X8_R(REG_X64_R8, 0, REG_X64_R9, 8, REG_X64_RSI);
  INC_A(REG_X64_RSI);
  MOV_R_O(REG_TRANSIENT_1, REG_X64_RDX, offsetof(store_cache_t, version));
  INC_A(REG_TRANSIENT_1);
  MOV_R_A(REG_TRANSIENT_1, REG_TRANSIENT_1);
  MOV_O_R(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_version_tag),
          REG_TRANSIENT_1);
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
  ni_trace_jump_cond(fill_jz, fill_jz + 2, code);
  CMP_R_O8(REG_X64_R9, REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_used));
  JCC_MISS(CC_NE); /* out of order: CPython converts the dict */
  INC_O8(REG_X64_RCX, CPY_OFFSET(PyDictObject, ma_used));
  MOV_R_R(REG_X64_RAX, REG_X64_RSI);
  INC_A(REG_X64_RSI);
  extra_assert(store - (code + 2) >= -128);
  WRITE_2(0xEB, (code_t)(store - (code + 2)));
  ni_trace_jump(code - 2, store);
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
void psyco_inline_store_begin(PsycoObject *po, inline_store_t *s) {
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
  for (i = 0; i < STORE_MAX_MISS_JUMPS; i++)
    near[i] = true;
  inline_store_body(po, start, s, near, jat, &nj, &miss);
  for (i = 0; i < nj; i++) /* each later jump made short shortens it */
    near[i] = miss - jat[i] - 6 > 127;
  code = inline_store_body(po, start, s, near, jat, &nj, &miss);
  for (i = 0; i < nj; i++) {
    if (near[i]) {
      *(dword_t *)(jat[i] + 2) = miss - (jat[i] + 6);
      ni_trace_jump_cond(jat[i], jat[i] + 6, miss);
    } else {
      extra_assert(miss - (jat[i] + 2) < 128);
      jat[i][1] = (code_t)(miss - (jat[i] + 2));
      ni_trace_jump_cond(jat[i], jat[i] + 2, miss);
    }
  }
  END_CODE
}

DEFINEFN
void psyco_inline_store_end(PsycoObject *po, inline_store_t *s) {
  int i;
  /* while respawning, the error check moved po->code back to the start
     of the trash buffer: nothing to point */
  if (is_respawning(po))
    return;
  for (i = 0; i < 2; i++) {
    psyco_assert(po->code - (s->done[i] + 2) < 128);
    s->done[i][1] = (code_t)(po->code - (s->done[i] + 2));
    ni_trace_jump(s->done[i], po->code);
  }
}

/* A type guard (pobject.c:type_changed): tp's tp_flags and tp_version_tag
   compared with a guard's two words, which are read at run time. The
   expected words are reached RIP-relative when in reach; otherwise
   through RAX, which is freed either way, so that the compiler's state
   doesn't depend on where the code is (a replay runs elsewhere). */
DEFINEFN
condition_code_t psyco_type_guard(PsycoObject *po, PyTypeObject *tp,
                                  unsigned long *flags, unsigned long *tag) {
  long off;
  BEGIN_CODE
  NEED_CC();
  NEED_REGISTER(REG_X64_RAX);
  MOV_R_I(REG_TRANSIENT_1, (long)tp);
  MOV_R_O(REG_TRANSIENT_2, REG_TRANSIENT_1, CPY_OFFSET(PyTypeObject, tp_flags));
  MOV_DR_O(REG_TRANSIENT_1, REG_TRANSIENT_1,
           CPY_OFFSET(PyTypeObject, tp_version_tag));
  off = (long)flags - ((long)code + 7);
  if (FITS_IN_32BITS(off) && FITS_IN_32BITS((long)tag - ((long)code + 14))) {
    XOR_R_RIP(REG_TRANSIENT_2, off);
    XOR_R_RIP(REG_TRANSIENT_1, (long)tag - ((long)code + 7));
  } else {
    MOV_R_I(REG_X64_RAX, (long)flags);
    XOR_R_O(REG_TRANSIENT_2, REG_X64_RAX, 0);
    off = (char *)tag - (char *)flags;
    XOR_R_O(REG_TRANSIENT_1, REG_X64_RAX, off);
  }
  OR_R_R(REG_TRANSIENT_1, REG_TRANSIENT_2);
  END_CODE
  return CC_NE;
}
