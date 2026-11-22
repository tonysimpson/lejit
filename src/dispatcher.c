/*
 * "Have we compiled this state before?" and the run-time switches.
 *  - Snapshots: each code buffer at a merge point keeps a frozen copy of
 *    po->vlocals. With VLOCALS_OPC it is compressed into a pseudo-code
 *    (fz_compress) and compared without being decompressed (compatible_array).
 *  - psyco_compatible: a linear scan of the snapshots. Fixed constants
 *    (SkFlagFixed) must match exactly; other differing constants are returned
 *    in 'diff' to be un-promoted, and so is a virtual int's known value where
 *    the snapshot's is run-time. Before comparing, canonical_int writes
 *    a constant small int or bool in a local as a virtual int, so the two
 *    forms of a counter compare. A 16-byte stack-alignment test
 *    (compare_return_address) rejects otherwise good matches whose
 *    return-address slot differs mod 16: x86-64 call alignment.
 *    A run-time value's known field that the snapshot lacks is dropped if
 *    nothing since the last merge point relied on it (rule 1,
 *    compatible_unused_fact), except in the compare a promotion's
 *    continuation makes at its own instruction, where it would loop.
 *  - psyco_forget_values: the cap per merge point's generalisation
 *    (vcompiler.c:mp_capped). A constant it makes run-time is a
 *    forgotten value: flagged RunTime_Megamorphic, as a megamorphic site's
 *    value is, so its promotions take the generic path.
 *  - Respawn: the rare branch of a run-time condition keeps no snapshot. When
 *    it is first taken, compilation is replayed from the previous snapshot
 * into a throw-away buffer until the same point is reached (do_respawn).
 *  - Promotion: a run-time value is switched on at run time. Each new value
 *    compiles a specialised continuation, kept in a move-to-front list and
 *    in the site's polymorphic inline cache (idispatcher.h), with a
 *    megamorphic cut-off after MEGAMORPHIC_MAX values. A promoted value is
 *    held until the code's owner dies; do_promotion_static_type only
 *    specialises on static types and sends anything else to the
 *    megamorphic version.
 *  - Parked states: the state a coding pause or a promotion site compiles
 *    from stays live for the PARK_LIVE_MAX newest, and is frozen after,
 *    with what a snapshot leaves out kept beside it (psyco_park).
 * psyco_forget_dead_vars drops what is known about the dead locals that
 * mergepoints.c lists first in each fatlist, keeping their values. State
 * is global and non-re-entrant (cmpinternal,
 * the static result in psyco_compatible).
 */
#include "dispatcher.h"
#include "Objects/pobject.h"
#include "Objects/pboolobject.h"
#include "Objects/plongobject.h"
#include "Python/pycompiler.h" /* for pyc_data_xxx() */
#include "blockalloc.h"
#include "codegen.h"
#include "codemanager.h"
#include "mergepoints.h"
#include "psyco.h"
#include "pycodegen.h" /* psyco_decref_c */
#include <idispatcher.h>

/***************************************************************/
/***                       Snapshots                           ***/
/***************************************************************/

#if !VLOCALS_OPC

/* Store-by-copy: this implementation is kept around for reference.
   See the "!VLOCALS_OPC" version of compatible_array().
*/

PSY_INLINE void
fz_build(FrozenPsycoObject *fpo, vinfo_array_t *aa)
{
    fpo->fz_vlocals = array_new(aa->count);
    duplicate_array(fpo->fz_vlocals, aa);
}

PSY_INLINE void
fz_unfreeze(vinfo_array_t *aa, FrozenPsycoObject *fpo)
{
    assert_cleared_tmp_marks(fpo->fz_vlocals);
    duplicate_array(aa, fpo->fz_vlocals);
    clear_tmp_marks(fpo->fz_vlocals);
}

PSY_INLINE void
fz_release(FrozenPsycoObject *fpo)
{
    array_delete(fpo->fz_vlocals, NULL);
}

/* invariant: all snapshot.fz_vlocals in the fatlist have
   all their 'tmp' fields set to NULL. */
PSY_INLINE void
fz_check_invariant(FrozenPsycoObject *fpo)
{
    assert_cleared_tmp_marks(fpo->fz_vlocals);
}

PSY_INLINE void
fz_restore_invariant(FrozenPsycoObject *fpo)
{
    clear_tmp_marks(fpo->fz_vlocals);
}

PSY_INLINE int
fz_top_array_count(FrozenPsycoObject *fpo)
{
    return fpo->fz_vlocals->count;
}

static void
fz_find_rt1(vinfo_array_t *bb, fz_find_fn callback, void *extra, bool clear
#if ALL_CHECKS
            ,
            vinfo_array_t *aa
#endif
)
{
    int i = bb->count;
    while (i--) {
        vinfo_t *b = bb->items[i];
        if (b != NULL && b->tmp != NULL) {
#if ALL_CHECKS
            extra_assert(i < aa->count);
            if (is_runtime(b->source))
                extra_assert(aa->items[i] == b->tmp);
#endif
            if (is_runtime(b->source))
                (*callback)(b->tmp, b->source, extra);
            if (clear)
                b->tmp = NULL; /* don't consider the same 'b' more than once */
            if (b->array != NullArray)
                fz_find_rt1(b->array, callback, extra, clear
#if ALL_CHECKS
                            ,
                            aa->items[i]->array
#endif
                );
        }
    }
}

DEFINEFN void
fz_find_runtimes(vinfo_array_t *aa, FrozenPsycoObject *fpo,
                 fz_find_fn callback, void *extra, bool clear)
{
    fz_find_rt1(fpo->fz_vlocals, callback, extra, clear
#if ALL_CHECKS
                ,
                aa
#endif
    );
}

#else /* if VLOCALS_OPC */

#define COMPRESS_COMPILETIME_SUBITEMS 0

struct vcilink_s {
    int time;
    union {
        vinfo_t *preva;
        vinfo_t **fix;
        void *data;
    } v;
    struct vcilink_s *next;
};

BLOCKALLOC_STATIC(vci, struct vcilink_s, 128)

typedef struct {
    /* pointers to an internal buffer for the creation of the pseudo-code
       copies, as well as for lookups */
    signed char *buf_begin;
    signed char *buf_end;
    signed char *buf_opc;      /* current opcode position */
    Source *buf_args;          /* current opargs position */
    long tmp_counter;          /* for links */
    struct vcilink_s *vcilink; /* pending links during decompression */
    struct vcilink_s sentinel;
    Source hint; /* the type hint read with the last opcode, or 0 */
} vcompat_internal_t;

/* XXX Argh, a global variable -- this is (just) tolerable because the
   algorithms depending on it are not re-entrent anyway. This lets us
   share a global all-purpose buffer between the pseudo-code creation
   and the lookups. */
static vcompat_internal_t cmpinternal = {NULL, NULL};

static signed char *
fz_internal_copy(vcompat_internal_t *current, int nsize)
{
    int opc_size = current->buf_end - current->buf_opc;
    signed char *buf = (signed char *)PyMem_MALLOC(nsize);
    if (buf == NULL)
        OUT_OF_MEMORY();
    /* the first buffer is NULL, and memcpy from NULL is undefined even for
       0 bytes */
    if (current->buf_begin != NULL) {
        memcpy(buf, current->buf_begin,
               ((signed char *)current->buf_args) - current->buf_begin);
        memcpy(buf + nsize - opc_size, current->buf_opc, opc_size);
    }
    return buf;
}

static void
fz_internal_expand(void)
{
    signed char *nbuf;
    int opc_size = cmpinternal.buf_end - cmpinternal.buf_opc;
    int arg_size =
        ((signed char *)cmpinternal.buf_args) - cmpinternal.buf_begin;
    int nsize = (cmpinternal.buf_end - cmpinternal.buf_begin) * 3 / 2;
    if (nsize < 64)
        nsize = 64;
    nbuf = fz_internal_copy(&cmpinternal, nsize);
    if (cmpinternal.buf_begin != cmpinternal.buf_end)
        PyMem_FREE(cmpinternal.buf_begin);
    cmpinternal.buf_begin = nbuf;
    cmpinternal.buf_end = nbuf + nsize;
    cmpinternal.buf_opc = cmpinternal.buf_end - opc_size;
    cmpinternal.buf_args = (Source *)(nbuf + arg_size);
}

#define FZ_OPC_EXT (-1)
#define FZ_OPC_NULL (-2)
/* an item's type_hint, read before the item's own opcode by
   fz_getopc, which leaves it in cmpinternal.hint: only fz_uncompress and
   fz_parse look at it, so states are compared without it */
#define FZ_OPC_HINT (-3)
#define FZ_OPC_LINK (-4) /* and the following negative numbers */

/* Note: the pseudo-code is built in left-to-right array order,
   but it is intended to be read backwards, thus uncompression creates
   the arrays right-to-left. */
PSY_INLINE void
fz_putarg(Source arg)
{
    if (cmpinternal.buf_opc < (signed char *)(cmpinternal.buf_args + 1))
        fz_internal_expand();
    *cmpinternal.buf_args++ = arg;
}

PSY_INLINE void
fz_putopc(int opc)
{
    if (!(-128 <= opc && opc < 128)) {
        fz_putarg((Source)opc);
        opc = FZ_OPC_EXT;
    }
    if (cmpinternal.buf_opc == (signed char *)cmpinternal.buf_args)
        fz_internal_expand();
    *--cmpinternal.buf_opc = opc;
}

#if COMPRESS_COMPILETIME_SUBITEMS
static void
fz_compress(vinfo_array_t *aa, bool nolinks)
{
    int i;
    int length = aa->count;
    /*while (length > 0 && aa->items[length-1] == NULL)
      length--;  ---- invalid optimization ---- */
    for (i = 0; i < length; i++) {
        vinfo_t *a = aa->items[i];
        if (a == NULL) {
            fz_putopc(FZ_OPC_NULL); /* emit FZ_OPC_NULL */
        }
        else if (a->tmp != NULL) {
            int prevcounter = (int)a->tmp; /* already seen, emit a link */
            fz_putopc(FZ_OPC_LINK - (cmpinternal.tmp_counter - prevcounter));
        }
        else {
            bool nosublinks = nolinks;
            int length;
            Source arg = a->source;
            if (is_compiletime(arg)) {
                sk_incref(CompileTime_Get(arg));
                nosublinks = true;
            }
            ++cmpinternal.tmp_counter;
            if (!nolinks)
                a->tmp = (vinfo_t *)cmpinternal.tmp_counter;
            length = a->array->count;
            if (length) /* avoid recursive call if unneeded */
                fz_compress(a->array, nosublinks); /* store the subarray */
            fz_putarg(a->source); /* store the 'source' field */
            fz_putopc(length);    /* store the length of the subarray */
        }
    }
}
#else /* !COMPRESS_COMPILETIME_SUBITEMS */
/* An item already stored is marked in its 'tmp' with its number, odd,
   from a count that goes on across snapshots and never wraps: a mark left
   by an earlier snapshot is at most fz_mark_base, a pointer or KEEP_MARK
   left by another pass is even or 1, and vinfo_new starts with NULL, so
   the marks are not cleared first. 'forget' is the state whose constants'
   checked types fpo_build drops (Psyco_ForgetConstantClasses): each
   constant is met here, and what is under it, which the snapshot leaves
   out, is walked apart. */
static unsigned long fz_mark_base = 0;
#define FZ_MARK(n) ((vinfo_t *)((((unsigned long)(n)) << 1) | 1))
#define FZ_MARKED(t) \
    ((((unsigned long)(t)) & 1) && (((unsigned long)(t)) >> 1) > fz_mark_base)

static void
fz_compress(vinfo_array_t *aa, PsycoObject *forget)
{
    int i;
    int length = aa->count;
    /*while (length > 0 && aa->items[length-1] == NULL)
      length--;  ---- invalid optimization ---- */
    for (i = 0; i < length; i++) {
        vinfo_t *a = aa->items[i];
        if (a == NULL) {
            fz_putopc(FZ_OPC_NULL); /* emit FZ_OPC_NULL */
        }
        else if (FZ_MARKED(a->tmp)) {
            /* already seen, emit a link */
            long prevcounter = (long)(((unsigned long)a->tmp) >> 1);
            fz_putopc(FZ_OPC_LINK - (cmpinternal.tmp_counter - prevcounter));
        }
        else {
            int length;
            Source arg;
            a->tmp = FZ_MARK(++cmpinternal.tmp_counter);
            arg = a->source;
            if (is_compiletime(arg)) {
                sk_incref(CompileTime_Get(arg));
                length = 0;
                if (forget != NULL && a->array != NullArray)
                    Psyco_ForgetConstantClass(forget, a);
            }
            else {
                length = a->array->count;
                if (length) /* avoid recursive call if unneeded */
                    fz_compress(a->array, forget); /* store the subarray */
            }
            fz_putarg(a->source); /* store the 'source' field */
            fz_putopc(length);    /* store the length of the subarray */
            if (a->type_hint != NULL) {
                sk_incref(a->type_hint);
                fz_putarg(CompileTime_NewSk(a->type_hint));
                fz_putopc(FZ_OPC_HINT);
            }
        }
    }
}
#endif /* COMPRESS_COMPILETIME_SUBITEMS */
/* Compression note: if we implement sharing of common initial segments
   (which could save another 50% memory) we get better results by storing
   the 'source' field *before* its sub-array. This will complicate
   compatible_array() a bit. */

PSY_INLINE Source
fz_getarg(void)
{
    return *--cmpinternal.buf_args;
}

PSY_INLINE int
fz_getopc(void)
{
    int result = *cmpinternal.buf_opc++;
    cmpinternal.hint = 0;
    if (result == FZ_OPC_HINT) {
        cmpinternal.hint = fz_getarg();
        result = *cmpinternal.buf_opc++;
    }
    if (result == FZ_OPC_EXT)
        result = (int)fz_getarg();
    return result;
}

static void
fz_pushstack(int opc, void *ndata)
{
    int ntime = cmpinternal.tmp_counter + (FZ_OPC_LINK - opc);
    struct vcilink_s **q;
    struct vcilink_s *p = psyco_llalloc_vci();
    p->time = ntime;
    p->v.data = ndata;

    /* Record the pending link by inserting it inon the vcilink linked list.
       'ntime' is the value that tmp_counter will have when the pending
       item is found, so currently 'ntime >= tmp_counter'.
       This linked list is sorted against the time, smallest time first. */
    q = &cmpinternal.vcilink;
    while ((*q)->time < ntime) q = &(*q)->next;
    p->next = *q; /* insert new item at the correct position */
    *q = p;
}

static void
fz_uncompress(vinfo_array_t *result)
{
    int i = result->count;
    while (i--) {
        vinfo_t *a;
        int opc = fz_getopc();
        if (opc >= 0) { /* new item, potentially with its sub-array */
            Source hint = cmpinternal.hint;
            a = vinfo_new_skref(fz_getarg());
            if (hint != 0) {
                a->type_hint = CompileTime_Get(hint);
                sk_incref(a->type_hint);
            }
            if (opc != 0) { /* test to make the common path (opc==0) faster */
                a->array = array_new(opc);
                fz_uncompress(a->array);
            }
        }
        else if (opc == FZ_OPC_NULL) { /* NULL */
            continue; /* there is already NULL in result->items[i] */
        }
        else { /* link to an item not built yet */
            fz_pushstack(opc, &result->items[i]);
            continue;
        }

        /* At this point we have a real item to store into the array */
        while (cmpinternal.tmp_counter == cmpinternal.vcilink->time) {
            struct vcilink_s *p = cmpinternal.vcilink;
            cmpinternal.vcilink = p->next;
            /* resolve link */
            vinfo_incref(a);
            *p->v.fix = a;
            psyco_llfree_vci(p);
        }
        cmpinternal.tmp_counter++;
        result->items[i] = a;
    }
}

static void
fz_parse(int length, bool clear)
{ /* highly simplified version of
     fz_uncompress() that does nothing */
    extra_assert(length >= 0);
    while (length--) {
        int opc = fz_getopc();
        if (opc >= 0) {
            Source hint = cmpinternal.hint;
            Source arg = fz_getarg();
            fz_parse(opc, clear);
            if (clear && is_compiletime(arg))
                sk_decref(CompileTime_Get(arg));
            if (clear && hint != 0)
                sk_decref(CompileTime_Get(hint));
        }
    }
}

static void
fz_find_rt1(vinfo_array_t *aa, int length, fz_find_fn callback, void *extra)
{ /* simplified version of fz_uncompress()
     that does not build anything */
    extra_assert(length >= 0);
    while (length--) {
        int opc = fz_getopc();
        if (opc >= 0) {
            vinfo_t *a;
            Source source = fz_getarg();
            if (is_compiletime(source)) {
                /* no run-time value should exist under a compile-time one */
                extra_assert(aa == NULL || length < aa->count);
                if (opc > 0)
                    fz_find_rt1(NULL, opc, NULL, NULL);
            }
            else {
                extra_assert(aa != NULL && length < aa->count);
                a = aa->items[length];
                if (is_runtime(source))
                    (*callback)(a, source, extra);
                if (opc > 0)
                    fz_find_rt1(a->array, opc, callback, extra);
            }
        }
    }
}

PSY_INLINE void
fz_load_fpo(FrozenPsycoObject *fpo)
{
    cmpinternal.buf_opc = (signed char *)fpo->fz_vlocals_opc;
    cmpinternal.buf_args = fpo->fz_vlocals_opc;
}

DEFINEFN void
fz_find_runtimes(vinfo_array_t *aa, FrozenPsycoObject *fpo,
                 fz_find_fn callback, void *extra, bool clear)
{
    fz_load_fpo(fpo);
    fz_find_rt1(aa, fz_getopc(), callback, extra);
}

static void
fz_build_forget(FrozenPsycoObject *fpo, vinfo_array_t *aa, PsycoObject *forget)
{
    int opc_size, arg_size;
    signed char *nbuf;
    cmpinternal.buf_opc = cmpinternal.buf_end;
    cmpinternal.buf_args = (Source *)cmpinternal.buf_begin;
    cmpinternal.tmp_counter = fz_mark_base;
    fz_compress(aa, forget);
    fz_mark_base = cmpinternal.tmp_counter;
    fz_putopc(aa->count);
    opc_size = cmpinternal.buf_end - cmpinternal.buf_opc;
    arg_size = ((signed char *)cmpinternal.buf_args) - cmpinternal.buf_begin;
    nbuf = fz_internal_copy(&cmpinternal, arg_size + opc_size);
    fpo->fz_vlocals_opc = (Source *)(nbuf + arg_size);
}

PSY_INLINE void
fz_build(FrozenPsycoObject *fpo, vinfo_array_t *aa)
{
    fz_build_forget(fpo, aa, NULL);
}

PSY_INLINE void
fz_load_fpo_stack(FrozenPsycoObject *fpo)
{
    fz_load_fpo(fpo);
    cmpinternal.tmp_counter = 0;
    cmpinternal.vcilink = &cmpinternal.sentinel;
    cmpinternal.sentinel.time = INT_MAX; /* sentinel */
}

PSY_INLINE int
fz_top_array_count(FrozenPsycoObject *fpo)
{
    int result;
    if (fpo->fz_vlocals_opc == NULL)
        return 0;
    fz_load_fpo(fpo);
    result = fz_getopc();
    extra_assert(result >= 0);
    return result;
}

PSY_INLINE void
fz_unfreeze(vinfo_array_t *aa, FrozenPsycoObject *fpo)
{
    fz_load_fpo_stack(fpo);
    aa->count = fz_getopc();
    fz_uncompress(aa);
    /* no more pending link should be left */
    extra_assert(cmpinternal.vcilink == &cmpinternal.sentinel);
}

PSY_INLINE void
fz_release(FrozenPsycoObject *fpo)
{
    if (fpo->fz_vlocals_opc != NULL) {
        fz_load_fpo(fpo);
        fz_parse(fz_getopc(),
                 true); /* find the beginning of the pseudo-code */
        PyMem_FREE(cmpinternal.buf_args);
    }
}

PSY_INLINE void
fz_check_invariant(FrozenPsycoObject *fpo)
{
}

PSY_INLINE void
fz_restore_invariant(FrozenPsycoObject *fpo)
{
}

#endif /* VLOCALS_OPC */

DEFINEFN int
psyco_top_array_count(FrozenPsycoObject *fpo) /*for psyco.c*/
{
    return fz_top_array_count(fpo);
}

/***************************************************************/

/* A snapshot's pyc_data is shared by every snapshot whose frozen pyc_data
   is the same byte for byte once next_instr, which the snapshot keeps
   itself, is left out: most of one code object's snapshots differ only
   there. Equal bytes make an equal copy for fpo_unfreeze, so sharing
   changes nothing a snapshot says; the shared copy holds its own
   reference to changing_globals, so an equal pointer is the same object.
   A hash set by contents, chained, each item counted by the snapshots
   holding it. */
typedef struct pyc_shared_s {
    struct pyc_shared_s *next;
    unsigned int hash, refcnt;
    pyc_data_t pyc; /* partially allocated (frozen_size), next_instr 0 */
} pyc_shared_t;
static pyc_shared_t **pyc_set = NULL;
static size_t pyc_set_mask = 0, pyc_set_count = 0;

static void
pyc_set_grow(void)
{
    size_t i, size = pyc_set == NULL ? 256 : 2 * (pyc_set_mask + 1);
    pyc_shared_t **n = PyMem_Calloc(size, sizeof(pyc_shared_t *));
    if (n == NULL) {
        if (pyc_set == NULL)
            OUT_OF_MEMORY();
        return; /* longer chains */
    }
    for (i = 0; pyc_set != NULL && i <= pyc_set_mask; i++)
        while (pyc_set[i] != NULL) {
            pyc_shared_t *e = pyc_set[i];
            pyc_set[i] = e->next;
            e->next = n[e->hash & (size - 1)];
            n[e->hash & (size - 1)] = e;
        }
    PyMem_Free(pyc_set);
    pyc_set = n;
    pyc_set_mask = size - 1;
}

_Static_assert(offsetof(pyc_data_t, blockstack) % 4 == 0 &&
                   sizeof(le_block_t) % 4 == 0,
               "pyc_share hashes 32-bit words");

static pyc_data_t *
pyc_share(pyc_data_t *pr)
{
    pyc_data_t key;
    size_t i, n = frozen_size(pr);
    uint64_t h64 = 14695981039346656037ULL;
    unsigned int h;
    pyc_shared_t *e;
    memcpy(&key, pr, n);
    key.next_instr = 0;
    for (i = 0; i < n / 4; i++)
        h64 = (h64 ^ ((uint32_t *)&key)[i]) * 1099511628211ULL;
    h = (unsigned int)(h64 ^ (h64 >> 32));
    if (pyc_set_count >= pyc_set_mask)
        pyc_set_grow();
    for (e = pyc_set[h & pyc_set_mask]; e != NULL; e = e->next)
        if (e->hash == h && frozen_size(&e->pyc) == n &&
            !memcmp(&e->pyc, &key, n)) {
            e->refcnt++;
            return &e->pyc;
        }
    e = PyMem_Malloc(offsetof(pyc_shared_t, pyc) + n);
    if (e == NULL)
        OUT_OF_MEMORY();
    e->hash = h;
    e->refcnt = 1;
    frozen_copy(&e->pyc, &key);
    e->next = pyc_set[h & pyc_set_mask];
    pyc_set[h & pyc_set_mask] = e;
    pyc_set_count++;
    return &e->pyc;
}

static void
pyc_unshare(pyc_data_t *p)
{
    pyc_shared_t *e =
        (pyc_shared_t *)((char *)p - offsetof(pyc_shared_t, pyc));
    pyc_shared_t **pe;
    if (--e->refcnt > 0)
        return;
    for (pe = &pyc_set[e->hash & pyc_set_mask]; *pe != e; pe = &(*pe)->next);
    *pe = e->next;
    pyc_set_count--;
    Py_XDECREF(e->pyc.changing_globals);
    PyMem_Free(e);
}

DEFINEFN
void
fpo_build(FrozenPsycoObject *fpo, PsycoObject *po)
{
    /* drops the constants' checked types, which are not in the snapshot */
    fz_build_forget(fpo, &po->vlocals, po);
#if ALL_CHECKS
    {
        long forgotten = psyco_stat_forgotten_types;
        Psyco_ForgetConstantClasses(po); /* fz_compress left none */
        extra_assert(psyco_stat_forgotten_types == forgotten);
    }
#endif
    psyco_assert_coherent1(po, false);
    fpo->fz_stuff.fz_stack_depth = po->stack_depth;
    SAVE_PROCESSOR_FROZENOBJECT(fpo, po);
    fpo->fz_next_instr = po->pr.next_instr;
    fpo->fz_pyc_data = pyc_share(&po->pr);
}

DEFINEFN
void
fpo_release(FrozenPsycoObject *fpo)
{
    if (fpo->fz_pyc_data != NULL)
        pyc_unshare(fpo->fz_pyc_data);
    fz_release(fpo);
}

static void
fpo_find_regs_array(vinfo_array_t *source, PsycoObject *po)
{
#if HAVE_CCREG
    condition_code_t cc;
#endif
    int i = source->count;
    while (i--) {
        vinfo_t *a = source->items[i];
        if (a != NULL) {
            Source src = a->source;
#if REG_TOTAL > 0
            if (is_runtime(src) && !is_reg_none(src))
                REG_NUMBER(po, getreg(src)) = a;
            else
#endif
#if HAVE_CCREG
                if ((cc = psyco_vsource_cc(src)) != CC_ALWAYS_FALSE)
                po->ccregs[INDEX_CC(cc)] = a;
            else
#endif
            { /* nothing */
            }
            if (a->array != NullArray)
                fpo_find_regs_array(a->array, po);
        }
    }
}

DEFINEFN
PsycoObject *
fpo_unfreeze(FrozenPsycoObject *fpo)
{
    /* rebuild a PsycoObject from 'this' */
    PsycoObject *po = PsycoObject_New(fz_top_array_count(fpo));
    po->stack_depth = get_stack_depth(fpo);
    RESTORE_PROCESSOR_FROZENOBJECT(fpo, po);
    fz_unfreeze(&po->vlocals, fpo);
    fpo_find_regs_array(&po->vlocals, po);
    frozen_copy(&po->pr, fpo->fz_pyc_data);
    po->pr.next_instr = fpo->fz_next_instr;
    pyc_data_build(po, psyco_get_merge_points(po->pr.co, -1));
    psyco_assert_coherent(po);
    return po;
}

DEFINEVAR long le_parked_live = 0;
#if ALL_CHECKS
DEFINEVAR long psyco_stat_parked_frozen = 0;
#endif
static parked_state_t *live_oldest = NULL, *live_newest = NULL;

static void
live_unlink(parked_state_t *ps)
{
    if (ps->older != NULL)
        ps->older->newer = ps->newer;
    else if (live_oldest == ps)
        live_oldest = ps->newer;
    else
        return; /* not listed */
    if (ps->newer != NULL)
        ps->newer->older = ps->older;
    else
        live_newest = ps->older;
    ps->older = ps->newer = NULL;
    le_parked_live--;
}

static void
live_append(parked_state_t *ps)
{
    ps->older = live_newest;
    ps->newer = NULL;
    if (live_newest != NULL)
        live_newest->newer = ps;
    else
        live_oldest = ps;
    live_newest = ps;
    le_parked_live++;
}

/* the indices from 'a' to 'v' at exactly 'depth' more levels, not under
   a constant (fz_compress drops what is known under one) */
static bool
fixpath_at(vinfo_array_t *a, vinfo_t *v, short *path, int depth)
{
    int i;
    for (i = 0; i < a->count; i++) {
        vinfo_t *x = a->items[i];
        if (x == NULL)
            continue;
        *path = i;
        if (depth == 0 ? x == v
                       : x->array != NullArray && !is_compiletime(x->source) &&
                             fixpath_at(x->array, v, path + 1, depth - 1))
            return true;
    }
    return false;
}

/* the shortest such path, ended by -1 if shorter than FIXPATH_MAX: the
   variable is nearly always a local or on the stack */
static bool
fixpath_find(vinfo_array_t *a, vinfo_t *v, short *path)
{
    int depth;
    for (depth = 0; depth < FIXPATH_MAX; depth++)
        if (fixpath_at(a, v, path, depth)) {
            if (depth + 1 < FIXPATH_MAX)
                path[depth + 1] = -1;
            return true;
        }
    return false;
}

/* the oldest live state, frozen, or left live for good */
static void
park_freeze(parked_state_t *ps)
{
    PsycoObject *po = ps->live;
    FrozenPsycoObject fz;
    short fixpath[FIXPATH_MAX];
    live_unlink(ps);
    fixpath[0] = -1;
    if (ps->fix != NULL && !fixpath_find(&po->vlocals, ps->fix, fixpath))
        return;
    psyco_assert_coherent(po);
    fpo_mark_new(&fz);
    fz.fz_next_instr = po->pr.next_instr;
    fz.fz_pyc_data = pyc_share(&po->pr);
    fz_build(&fz, &po->vlocals);
    fz.fz_stuff.fz_stack_depth = po->stack_depth;
    SAVE_PROCESSOR_FROZENOBJECT((&fz), po);
    /* the live part is overwritten from here */
    memcpy(ps->fixpath, fixpath, sizeof(fixpath));
    ps->fz = fz;
    ps->respawn_proxy = po->respawn_proxy;
    ps->merge_points = po->pr.merge_points;
    ps->mp_serial = po->mp_serial;
    ps->respawn_cnt = po->respawn_cnt;
    ps->stack_base = po->pr.stack_base;
    ps->stack_level = po->pr.stack_level;
    ps->split_after_global = po->pr.split_after_global;
    ps->live = NULL;
    LE_STAT(psyco_stat_parked_frozen++);
    PsycoObject_Delete(po);
}

DEFINEFN void
psyco_park(parked_state_t *ps, PsycoObject *po, vinfo_t *fix)
{
    ps->live = po;
    ps->fix = fix;
    ps->older = ps->newer = NULL;
    if (po->pr.exc != NULL || po->pr.val != NULL || po->pr.tb != NULL)
        return;
    live_append(ps);
    if (le_parked_live > PARK_LIVE_MAX)
        park_freeze(live_oldest);
}

DEFINEFN PsycoObject *
psyco_unpark(parked_state_t *ps, vinfo_t **fix)
{
    PsycoObject *po = ps->live;
    if (po != NULL) {
        /* the copy has the 'tmp' marks of 'po' pointing into it */
        PsycoObject *copy = PsycoObject_Duplicate(po);
        *fix = ps->fix;
        ps->live = copy;
        if (ps->fix != NULL)
            ps->fix = ps->fix->tmp;
        return po;
    }
    else {
        vinfo_t *v = NULL;
        vinfo_array_t *a;
        int i;
        po = fpo_unfreeze(&ps->fz);
        po->respawn_proxy = ps->respawn_proxy;
        po->pr.merge_points = ps->merge_points;
        po->mp_serial = ps->mp_serial;
        po->respawn_cnt = ps->respawn_cnt;
        po->pr.stack_base = ps->stack_base;
        po->pr.stack_level = ps->stack_level;
        po->pr.split_after_global = ps->split_after_global;
        psyco_assert_coherent(po);
        a = &po->vlocals;
        for (i = 0; i < FIXPATH_MAX && ps->fixpath[i] >= 0; i++) {
            v = a->items[ps->fixpath[i]];
            a = v->array;
        }
        *fix = v;
        return po;
    }
}

DEFINEFN void
psyco_park_release(parked_state_t *ps)
{
    if (ps->live != NULL) {
        live_unlink(ps);
        PsycoObject_Delete(ps->live);
        ps->live = NULL;
    }
    else if (ps->fz.fz_vlocals_opc != NULL || ps->fz.fz_pyc_data != NULL) {
        LE_STAT(psyco_stat_parked_frozen--);
        fpo_release(&ps->fz);
    }
    fpo_mark_unused(&ps->fz);
}

/*****************************************************************
 * Respawning is restoring a frozen compiler state into a live
 * PsycoObject and restarting the compilation. It will produce
 * exactly the same code up to a given point. This point was
 * a jump in the run-time code, pointing to code that we did
 * not compile yet. The purpose of 'replaying' the compilation
 * is to rebuild exactly the same state as the compiler had when
 * it emitted the jump instruction in the first place. At this
 * point we can go on with the real compilation of the missing
 * code.
 *
 * This is all based on the idea that we want to avoid tons
 * of copies of PsycoObjects all around for all pending
 * compilation branches, and there are a lot of them -- e.g. all
 * instructions that could trigger an exception have such a
 * (generally uncompiled) branch.
 *****/

#if ALL_CHECKS
typedef struct {
    unsigned long x;  /* what is compared (replay check, below) */
    long value;       /* a constant's value, else 0 */
    PyTypeObject *tp; /* a constant object's type, for the report */
    short index;      /* in its array, */
    short depth;      /* and that array's nesting depth */
} respawn_item_t;
#endif

typedef struct respawn_s {
    void *write_jmp;
    int respawn_cnt;
    CodeBufferObject *respawn_from;
#if ALL_CHECKS
    struct respawn_check_s {
        const char
            *file; /* where the point was made (LE_RESPAWN_SITE), or NULL */
        int line;
        int next_instr;
        int stack_depth;
        int count;
        respawn_item_t *items; /* po->vlocals, flattened */
    } check;
#endif
} respawn_t;

#if ALL_CHECKS
/* Replay check. A replay must reach its respawn point in exactly the state
   the first compile was in there (see the warning in dispatcher.h). Each
   respawn point records where it was made and a flattened copy of the
   state; the replay compares them when it detects the point, and stops at
   the first difference instead of compiling a branch for the wrong state.
   Fields known about run-time values are left out (see below).
   Only the final point of a chain of respawned buffers is checked. */
DEFINEVAR const char *psyco_respawn_file = NULL;
DEFINEVAR int psyco_respawn_line = 0;

static int
respawn_flatten(vinfo_array_t *a, int depth, respawn_item_t *out, int n)
{
    int i;
    for (i = 0; i < a->count; i++) {
        vinfo_t *v = a->items[i];
        respawn_item_t it = {1, 0, NULL, (short)i, (short)depth};
        if (v == NULL)
            ;
        else if (is_compiletime(v->source)) {
            /* folding can build an immutable value afresh on each compile (a
               constant tuple, or a slice of constants forced): compare those
               by type. Any other object, such as a class or a function, by
               identity, but see respawn_same_constant */
            source_known_t *sk = CompileTime_Get(v->source);
            PyObject *o = (PyObject *)sk->value;
            it.value = sk->value;
            if (sk->refcount1_flags & SkFlagPyObj)
                it.tp = Py_TYPE(o);
            if (it.tp != NULL &&
                (PyTuple_CheckExact(o) || PyUnicode_CheckExact(o) ||
                 PyBytes_CheckExact(o) || PyLong_CheckExact(o) ||
                 PyFloat_CheckExact(o) || PyFrozenSet_CheckExact(o) ||
                 PySlice_Check(o)))
                it.x = (unsigned long)it.tp * 4 + 3;
            else if (it.tp == &PyCFunction_Type)
                /* a built-in bound method, made by folding an attribute lookup
                   on a constant: compare which C function it wraps */
                it.x = (unsigned long)((PyCFunctionObject *)o)->m_ml * 4 + 3;
            else
                it.x = (unsigned long)sk->value * 4 + 2;
        }
        else
            it.x =
                (unsigned long)v->source; /* a location, or a virtual type */
        if (out != NULL)
            out[n] = it;
        n++;
        /* a type hint decides whether a guard is emitted */
        if (v != NULL && v->type_hint != NULL) {
            respawn_item_t h = {(unsigned long)v->type_hint->value * 4 + 2,
                                v->type_hint->value, NULL, -1,
                                (short)(depth + 1)};
            if (out != NULL)
                out[n] = h;
            n++;
        }
        /* a virtual object's fields are part of the state; under a run-time or
           compile-time value they are only known fields, which a snapshot may
           drop, so a replay can legitimately know less */
        if (v != NULL && is_virtualtime(v->source) && v->array != NullArray)
            n = respawn_flatten(v->array, depth + 1, out, n);
    }
    return n;
}

/* a point's own copy lives as long as its owner; the replay's is freed */
static void
respawn_check_fill(struct respawn_check_s *c, PsycoObject *po, bool owned)
{
    c->file = psyco_respawn_file;
    c->line = psyco_respawn_line;
    c->next_instr = po->pr.next_instr;
    c->stack_depth = po->stack_depth;
    c->count = respawn_flatten(&po->vlocals, 0, NULL, 0);
    if (owned)
        c->items = psyco_owner_alloc(c->count * sizeof(respawn_item_t) + 1);
    else
        c->items = PyMem_Malloc(c->count * sizeof(respawn_item_t) + 1);
    if (c->items == NULL)
        OUT_OF_MEMORY();
    respawn_flatten(&po->vlocals, 0, c->items, 0);
}

static const char *
respawn_item_kind(respawn_item_t *it)
{
    switch (it->x & 3) {
        case 1:
            return "null";
        case 2:
            return it->tp ? "constant object" : "constant";
        case 3:
            return "constant immutable value";
        default:
            return "run-time or virtual source";
    }
}

/* the same constant, whatever its flags (a constant object made without
   SkFlagPyObj, such as LOAD_CONST's, and the same object with it) */
static bool
respawn_same_constant(respawn_item_t *a, respawn_item_t *b)
{
    return (a->x & 2) && (b->x & 2) && a->value == b->value;
}

static void
respawn_check(respawn_t *rs, PsycoObject *po)
{
    struct respawn_check_s now;
    int i, diff = -1;
    respawn_check_fill(&now, po, false);
    for (i = 0; i < now.count && i < rs->check.count; i++)
        if ((now.items[i].x != rs->check.items[i].x &&
             !respawn_same_constant(&now.items[i], &rs->check.items[i])) ||
            now.items[i].depth != rs->check.items[i].depth) {
            diff = i;
            break;
        }
    if (diff < 0 && now.count != rs->check.count)
        diff = i;
    if (now.file != rs->check.file || now.line != rs->check.line ||
        now.next_instr != rs->check.next_instr ||
        now.stack_depth != rs->check.stack_depth || diff >= 0) {
        fprintf(stderr,
                "psyco: replay reached respawn point %d in a different state\n"
                "  code %s, first compile / replay:\n"
                "  made at: %s:%d / %s:%d\n"
                "  next_instr: %d / %d\n"
                "  stack_depth: %d / %d\n"
                "  state items: %d / %d\n",
                rs->respawn_cnt, PyUnicode_AsUTF8(po->pr.co->co_name),
                rs->check.file ? rs->check.file : "?", rs->check.line,
                now.file ? now.file : "?", now.line, rs->check.next_instr,
                now.next_instr, rs->check.stack_depth, now.stack_depth,
                rs->check.count, now.count);
        if (diff >= 0) {
            respawn_item_t *a =
                diff < rs->check.count ? &rs->check.items[diff] : NULL;
            respawn_item_t *b = diff < now.count ? &now.items[diff] : NULL;
            fprintf(stderr, "  first difference at flattened item %d:\n",
                    diff);
            if (a)
                fprintf(stderr,
                        "    first compile: [%d] at depth %d, %s %#lx%s%s\n",
                        a->index, a->depth, respawn_item_kind(a), a->x,
                        a->tp ? ", type " : "", a->tp ? a->tp->tp_name : "");
            if (b)
                fprintf(stderr,
                        "    replay:        [%d] at depth %d, %s %#lx%s%s\n",
                        b->index, b->depth, respawn_item_kind(b), b->x,
                        b->tp ? ", type " : "", b->tp ? b->tp->tp_name : "");
        }
        Py_FatalError("psyco: respawn replay diverged");
    }
    PyMem_Free(now.items);
}
#endif

static code_t *
do_respawn_body(respawn_t *rs);
/* a stub function: 'value' (RSI) is used only by promotions */
static code_t *
do_respawn(respawn_t *rs, long value)
{
    code_t *r;
    le_compile_guard_t g;
#if LE_CODESTATS
    long t0 = le_now_ns();
    psyco_cs_load_respawn = false;
#endif
    LE_COMPILE_BEGIN(LE_ENTER_RESPAWN);
    psyco_guard_push(&g);
    if (sigsetjmp(g.env, 0) != 0)
        psyco_guard_retry(&g); /* the replay starts from the snapshot again */
    r = do_respawn_body(rs);
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_RESPAWN);
#if LE_CODESTATS
    if (psyco_cs_load_respawn) { /* a load guard's failure branch */
        psyco_cs_load[4] += le_now_ns() - t0;
        psyco_cs_load_respawn = false;
    }
#endif
    return r;
}

static code_t *
do_respawn_body(respawn_t *rs)
{
    /* called when entering a not-compiled branch requiring a respawn */
    code_t *code;
    CodeBufferObject *firstcodebuf;
    CodeBufferObject *codebuf;
    PsycoObject *po;

    /* we might have a chain of code buffers, each containing a conditional
       jump to the next one. It ends with a proxy (not-yet-compiled) calling
       the function do_respawn().

       +----------+
       |  ...     |
       |  Jcond ----------->  +----------+
       |  ...     |           |  ...     |
       +----------+           |  Jcond ----------->   +-----------------+
                              |  ...     |            | CALL do_respawn |
                              +----------+            +-----------------+

       The structure 'rs' is stored in the last block (the proxy).
       'rs->respawn_from' is the previous code buffer.
       'rs->respawn_from->snapshot.fz_respawned_from' is the previous one.
       etc.
    */
    int respawn_cnt = rs->respawn_cnt;
    /* find the first code buffer in the chain */
    for (firstcodebuf = rs->respawn_from;
         firstcodebuf->snapshot.fz_respawned_from != NULL;
         firstcodebuf = firstcodebuf->snapshot.fz_respawned_from)
        respawn_cnt = firstcodebuf->snapshot.fz_respawned_cnt;
    /* respawn there */
    po = fpo_unfreeze(&firstcodebuf->snapshot);

    codebuf = psyco_new_code_buffer(NULL, NULL, &po->codelimit);
    codebuf->snapshot.fz_stuff.respawning = rs;
    codebuf->snapshot.fz_respawned_cnt = rs->respawn_cnt;
    codebuf->snapshot.fz_respawned_from = firstcodebuf;
    po->code = insn_code_label(codebuf->codestart);
    /* respawn by restarting the Python compiler at the beginning of the
       instruction where it left. It will probably re-emit a few machine
       instructions -- not needed, they will be trashed, but this has
       rebuilt the correct PsycoObject state. This occurs when eventually
       a positive detect_respawn() is issued. */
    po->respawn_cnt = -respawn_cnt;
    po->respawn_proxy = codebuf;

    code = GLOBAL_ENTRY_POINT(po);

    CLOSE_CODE("respawned", codebuf->codestart, code);
#if LE_CODESTATS
    if (psyco_cs_load_respawn)
        psyco_cs_load[5] += code - (code_t *)codebuf->codestart;
#endif
    /* make sure detect_respawn() succeeded */
    psyco_assert(codebuf->snapshot.fz_respawned_from == rs->respawn_from);

    /* fix the jump to point to 'codebuf->codestart' */
    change_cond_jump_target(rs->write_jmp, (code_t *)codebuf->codestart);

    /* the stub is never entered again; it and its record stay, unused, until
       the owner dies */
    return codebuf->codestart;
}

DEFINEFN
void
psyco_respawn_detected(PsycoObject *po)
{
    /* this is called when detect_respawn() succeeds. We can now proceed
       to the next code block in the picture above. When we reach the
       last one, it means we are about to compile the 'special' branch of
       a conditional jump -- the one that is not compiled yet.
    */
    CodeBufferObject *codebuf = po->respawn_proxy;
    CodeBufferObject *current = codebuf->snapshot.fz_respawned_from;
    respawn_t *rs = codebuf->snapshot.fz_stuff.respawning;

    /* 'codebuf' is the new block we are writing.
       'current' is the block we are currently respawning.
       'rs->respawn_from' is the last block before the proxy calling
          do_respawn(). */
    if (current == rs->respawn_from) {
        /* respawn finished */
#if ALL_CHECKS
        respawn_check(rs, po);
#endif
        extra_assert(fz_top_array_count(&codebuf->snapshot) == 0);
        fpo_build(&codebuf->snapshot, po);
    }
    else {
        /* proceed to the next block */
        CodeBufferObject *nextblock;
        int respawn_cnt = rs->respawn_cnt;
        for (nextblock = rs->respawn_from;
             nextblock->snapshot.fz_respawned_from != current;
             nextblock = nextblock->snapshot.fz_respawned_from)
            respawn_cnt = nextblock->snapshot.fz_respawned_cnt;
        codebuf->snapshot.fz_respawned_from = nextblock;
        po->respawn_cnt = -respawn_cnt;
        /* where that block's compile built its snapshot */
        Psyco_ForgetConstantClasses(po);
    }
    /* restart at the beginning of the buffer, overriding the code written
       so far. What the overwritten code made (constants, frame records) is
       held by the owner like the rest: a replay only wastes it. */
    po->code = (code_t *)codebuf->codestart;
    INIT_CODE_EMISSION(po->code);
}

/* Stubs (idispatcher.h): records on the heap (psyco_owner_alloc), code
   packed into pool chunks of executable memory, taken owned at once
   (psyco_code_take) so that no extent stays open. Both belong to the
   owner whose code was being compiled and are freed with it. The stub
   enters the compiler as that owner. A respawn's record is in the pool
   instead, after its stub (pooled_respawn_t, below), and each chunk
   starts with its owner. */
#define STUB_POOL_MIN 128
#define STUB_POOL_MAX 512

typedef struct {
    le_owner_t *owner;
    code_t *(*fn)(void *closure, long value);
    /* the closure follows, 8-aligned */
} stub_record_t;

/* 'value' is RSI as the stub left it: a promotion's run-time value (the
   stub copies it there). Respawn and pause functions take only the closure;
   under SysV the extra argument is harmless. */
DEFINEFN code_t *
psyco_stub_enter(void *record, long value)
{
    stub_record_t *r = (stub_record_t *)record;
    code_t *result;
    LE_OWNER_ENTER(r->owner);
    result = r->fn(r + 1, value);
    LE_OWNER_LEAVE();
    return result;
}

DEFINEFN void
psyco_stub_set_release(void *closure, void (*fn)(void *))
{
    le_owner_t *o = (((stub_record_t *)closure) - 1)->owner;
    if (o == NULL)
        return; /* kept for good */
    if (o->nreleases == o->releasescap) {
        int cap = o->releasescap ? 2 * o->releasescap : 4;
        void *n = PyMem_Realloc(o->releases, cap * sizeof(*o->releases));
        if (n == NULL)
            OUT_OF_MEMORY();
        o->releases = n;
        o->releasescap = cap;
    }
    o->releases[o->nreleases].fn = fn;
    o->releases[o->nreleases].closure = closure;
    o->nreleases++;
}

DEFINEFN void
psyco_owner_release_records(le_owner_t *o)
{
    int i;
    for (i = 0; i < o->nreleases; i++)
        o->releases[i].fn(o->releases[i].closure);
    PyMem_Free(o->releases);
    o->releases = NULL;
    o->nreleases = o->releasescap = 0;
}

static void *
write_stub(void *fn, size_t size, code_t **at)
{
    stub_record_t *r = psyco_owner_alloc(sizeof(stub_record_t) + size);
    r->owner = psyco_owner;
    r->fn = (code_t * (*)(void *, long)) fn;
    *at = psyco_write_stub(*at, r);
    return r + 1;
}

DEFINEFN void *
psyco_new_stub_at(void *fn, size_t size, code_t **at)
{
    return write_stub(fn, size, at);
}

/* The stub pool also holds promotion chains. stub_pool_reserve makes room
   for 'n' bytes at the owner's stub_pool; the caller moves it past what it
   wrote. Each owner has its own, in chunks growing from STUB_POOL_MIN to
   STUB_POOL_MAX: the unused end of an owner's last chunk is lost while it
   lives, so small chunks waste less, and an owner with few stubs takes
   little. */
static le_owner_t stubs_for_good; /* while no owner is current */
#define POOL_OWNER (psyco_owner != NULL ? psyco_owner : &stubs_for_good)

static code_t *
stub_pool_reserve(size_t n)
{
    le_owner_t *o = POOL_OWNER;
    if ((size_t)(o->stub_pool_end - o->stub_pool) < n) {
        size_t size = o->stub_chunk == 0              ? STUB_POOL_MIN
                      : o->stub_chunk < STUB_POOL_MAX ? 2 * o->stub_chunk
                                                      : STUB_POOL_MAX;
        code_t *chunk;
        while (size < n + sizeof(le_owner_t *)) size *= 2;
        o->stub_chunk = size;
        chunk = psyco_code_take(size);
        *(le_owner_t **)chunk = psyco_owner;
        o->stub_pool = chunk + sizeof(le_owner_t *);
        o->stub_pool_end = chunk + size;
    }
    return o->stub_pool;
}

DEFINEFN void *
psyco_new_stub(void *fn, size_t size, code_t **stub)
{
    *stub = stub_pool_reserve(STUB_SIZE_MAX);
    return write_stub(fn, size, &POOL_OWNER->stub_pool);
}

/* A respawn's record in the stub pool, unaligned, after its stub. Of a
   respawn_t it keeps what can't be found again: the jump to repoint, as
   an offset from the record, the count, and the base as its index in the
   owner's blocks; the owner is at the start of the record's chunk, and
   the function is do_respawn. A respawn whose fields don't fit, or made
   while no owner is current, has a heap record (psyco_new_stub). */
typedef struct {
    int32_t jmp;    /* write_jmp - record */
    uint32_t from;  /* respawn_from == owner->bufs[from] */
    uint16_t cnt;   /* respawn_cnt */
    uint16_t chunk; /* record - the chunk's start */
#if ALL_CHECKS
    struct respawn_check_s *check;
#endif
} __attribute__((packed)) pooled_respawn_t;

/* the index of 'b' in the owner's blocks, or -1. A respawn's base is
   nearly always the block made last, or the one the last search found. */
static long
owner_buf_index(le_owner_t *o, CodeBufferObject *b)
{
    static le_owner_t *last_o = NULL;
    static int last_i = 0;
    int i;
    if (last_o == o && last_i < o->nbufs && o->bufs[last_i] == b)
        return last_i;
    for (i = o->nbufs; i--;)
        if (o->bufs[i] == b) {
            last_o = o;
            last_i = i;
            return i;
        }
    return -1;
}

DEFINEFN code_t *
psyco_respawn_enter(code_t *record)
{
    pooled_respawn_t r;
    le_owner_t *o;
    respawn_t rs;
    code_t *result;
    memcpy(&r, record, sizeof(r));
    o = *(le_owner_t **)(record - r.chunk);
    rs.write_jmp = record + r.jmp;
    rs.respawn_cnt = r.cnt;
    rs.respawn_from = o->bufs[r.from];
#if ALL_CHECKS
    rs.check = *r.check;
#endif
    LE_OWNER_ENTER(o);
    result = do_respawn(&rs, 0);
    LE_OWNER_LEAVE();
    return result;
}

/* a respawn's stub and record in the pool, if its fields fit: the
   record, and the stub in '*stub' */
static code_t *
new_pooled_respawn(PsycoObject *po, code_t **stub)
{
    le_owner_t *o = psyco_owner;
    pooled_respawn_t r;
    code_t *record;
    long from;
    if (o == NULL || po->respawn_cnt < 0 || po->respawn_cnt > 0xFFFF)
        return NULL;
    from = owner_buf_index(o, po->respawn_proxy);
    if (from < 0)
        return NULL;
    *stub = stub_pool_reserve(RESPAWN_STUB_SIZE + sizeof(pooled_respawn_t));
    record = psyco_write_respawn_stub(*stub);
    if (record == NULL)
        return NULL;
    memset(&r, 0, sizeof(r));
    r.from = (uint32_t)from;
    r.cnt = (uint16_t)po->respawn_cnt;
    r.chunk = (uint16_t)(record - (o->stub_pool_end - o->stub_chunk));
    memcpy(record, &r, sizeof(r));
    o->stub_pool = record + sizeof(pooled_respawn_t);
    return record;
}

DEFINEFN
void *
psyco_prepare_respawn_ex(PsycoObject *po, condition_code_t jmpcondition,
                         void *fn, size_t extrasize)
{
    /* ignore calls to psyco_prepare_respawn() while currently respawning */
    if (!is_respawning(po)) {
        char *closure;
        respawn_t *rs;
        code_t *stub, *record = NULL;
        void *write_jmp;

        extra_assert((int)jmpcondition < CC_TOTAL);
        extra_assert(po->respawn_proxy != NULL);

        if (fn == &do_respawn && extrasize == 0)
            record = new_pooled_respawn(po, &stub);
        if (record != NULL) {
            int32_t jmp;
#if ALL_CHECKS
            struct respawn_check_s *c = psyco_owner_alloc(sizeof(*c));
            respawn_check_fill(c, po, true);
            memcpy(record + offsetof(pooled_respawn_t, check), &c, sizeof(c));
#endif
            psyco_resolved_cc(po, INVERT_CC(jmpcondition));
            write_jmp = conditional_jump_to(po, stub, jmpcondition);
            psyco_assert(FITS_IN_32BITS((code_t *)write_jmp - record));
            jmp = (int32_t)((code_t *)write_jmp - record);
            memcpy(record + offsetof(pooled_respawn_t, jmp), &jmp,
                   sizeof(jmp));
            return record;
        }

        /* the stub calls 'fn' with the closure, followed by a respawn_t */
        closure =
            (char *)psyco_new_stub(fn, extrasize + sizeof(respawn_t), &stub);
        rs = (respawn_t *)(closure + extrasize);
        /* fill in the respawn_t structure */
        rs->respawn_cnt = po->respawn_cnt;
        rs->respawn_from = po->respawn_proxy;
#if ALL_CHECKS
        respawn_check_fill(&rs->check, po, true);
#endif

        /* write the jump to the stub */
        psyco_resolved_cc(po,
                          INVERT_CC(jmpcondition)); /* no jump => cond false */
        rs->write_jmp = conditional_jump_to(po, stub, jmpcondition);
        return closure;
    }
    else {
        /* respawning: come back at the
           beginning of the trash memory for
           the next instructions. The replay takes the no-jump path, as the
           first compile did, so a condition held in the flags is resolved the
           same way */
        psyco_resolved_cc(po, INVERT_CC(jmpcondition));
        po->code = (code_t *)po->respawn_proxy->codestart;
        INIT_CODE_EMISSION(po->code);
        return NULL;
    }
}

DEFINEFN
bool
psyco_prepare_respawn(PsycoObject *po, condition_code_t jmpcondition)
{
    if (detect_respawn(po)) {
        /* the branch taken: the condition held. A value still held in the
           flags can't be read from them, since the stub's call clobbered them
           on the first run */
        psyco_resolved_cc(po, jmpcondition);
        return true;
    }
    psyco_prepare_respawn_ex(po, jmpcondition, &do_respawn, 0);
    return false;
}

DEFINEFN
code_t *
psyco_do_respawn(void *arg, int extrasize)
{
    respawn_t *rs = (respawn_t *)(((char *)arg) + extrasize);
    return do_respawn(rs, 0);
}

DEFINEFN
code_t *
psyco_dont_respawn(void *arg, int extrasize)
{
    respawn_t *rs = (respawn_t *)(((char *)arg) + extrasize);
    return resume_after_cond_jump(rs->write_jmp);
}

DEFINEFN
int(runtime_NON_NULL_f)(PsycoObject *po, vinfo_t *vi)
{
    condition_code_t cc = integer_NON_NULL(po, vi);
    return cc == CC_ERROR ? -1 : runtime_condition_f(po, cc);
}

DEFINEFN
int(runtime_NON_NULL_t)(PsycoObject *po, vinfo_t *vi)
{
    condition_code_t cc = integer_NON_NULL(po, vi);
    return cc == CC_ERROR ? -1 : runtime_condition_t(po, cc);
}

DEFINEFN
int(runtime_in_bounds)(PsycoObject *po, vinfo_t *vi, long lowbound,
                       long highbound)
{
    condition_code_t cc;
    if (highbound == LONG_MAX) {
        if (lowbound == LONG_MIN)
            return 1;
        cc = integer_cmp_i(po, vi, lowbound, Py_GE);
    }
    else if (lowbound == 0) {
        cc = integer_cmp_i(po, vi, highbound, Py_LE | COMPARE_UNSIGNED);
    }
    else {
        if (lowbound != LONG_MIN) {
            cc = integer_cmp_i(po, vi, lowbound, Py_GE);
            if (cc == CC_ERROR)
                return -1;
            if (!runtime_condition_t(po, cc))
                return 0;
        }
        cc = integer_cmp_i(po, vi, highbound, Py_LE);
    }
    return cc == CC_ERROR ? -1 : runtime_condition_t(po, cc);
}

DEFINEFN
void
PsycoObject_EmergencyCodeRoom(PsycoObject *po)
{
    if (!is_respawning(po)) {
        /* non-respawning case: start a new buffer, and code a JMP at
           the end of the current buffer. Note that it is exceptional
           to reach this point. Normally, pycompiler.c regularly checks
           that we are not reaching the end of the buffer, and if so,
           pauses compilation. */
        psyco_code_continue(&po->code, &po->codelimit);
    }
    else {
        /* respawning case: trash everything written so far */
        po->code = (code_t *)po->respawn_proxy->codestart;
        INIT_CODE_EMISSION(po->code);
    }
}

/*****************************************************************/

#if !VLOCALS_OPC

/* This implementation should probably not be used any more, but it
   is kept around for reference. Once you understand the comparison
   algorithm below you can read its more obscure implementation of
   the VLOCALS_OPC case.
*/
static bool
compatible_array(vinfo_array_t *aa, vinfo_array_t *bb, vinfo_array_t **result)
{
    /* 'aa' is the array from the live PsycoObject. 'bb' is from the snapshop.
       Test for compatibility. More precisely, it must be allowable for the
       living state 'aa' to jump back to the code already produced for the
       state 'bb'. There might be more information in 'aa' than in 'bb' (which
       will then just be discarded), but the converse is not allowable.
       Moreover, if two slots in the arrays point to the same vinfo_t in 'bb',
       they must also do so in 'aa' because the code compiled from 'bb' might
       have used this fact. Conversely, shared pointers in 'aa' need *not* be
       shared any more in 'bb'.

       The sharing rule only applies to run-time values: for compile-time and
       virtual-time values, being shared or not is irrelevant to the
       compilation process.

       Return value of compatible_array(): false if incompatible, otherwise
       fills the 'result' array with the variables to un-promote.
    */
    int i;
    int count = bb->count;
    if (aa->count != count) {
        if (aa->count <
            count) /* array too short; ok only if the extra items */
        {          /*   in 'bb' are all NULL.                     */
            for (i = aa->count; i < count; i++)
                if (bb->items[i] != NULL)
                    return false; /* differs */
            count = aa->count;
        }
        else /* array too long */
        {
#if 0
          /* the extra items in 'aa' must pass the test (b == NULL) below. */
          for (i=aa->count; i>count; )
            {
              vinfo_t* a = aa->items[--i];
              if (a != NULL && is_compiletime(a->source) &&
                  ((KNOWN_SOURCE(a)->refcount1_flags & SkFlagFixed) != 0))
                return false;  /* incompatible */
            }
#else
            /* the extra items in 'aa' must all be NULL. This is not strictly
               necessary for a compatibility (the above condition is more
               precise) but I guess that you get better results this way,
               because you have more information for a recompilation.
               XXX This assumption should be tested.
               XXX What about requiring that no NULL goes non-NULL in the
                   rest of the array as well? */
            for (i = aa->count; i > count;)
                if (aa->items[--i] != NULL)
                    return false; /* differs */
#endif
        }
    }
    for (i = 0; i < count; i++) {
        vinfo_t *a = aa->items[i];
        vinfo_t *b = bb->items[i];
        if (b == NULL) {
            /* if b == NULL, any value in 'a' is ok --
               with the exception of a fixed compile-time value, as
               created by a promotion. Without the following test,
               Psyco sometimes emits an infinite loop because the
               PsycoObject after promotion is found to be compatible
               with itself just before promotion. */
            if (a != NULL && is_compiletime(a->source) &&
                ((KNOWN_SOURCE(a)->refcount1_flags & SkFlagFixed) != 0))
                goto incompatible;
        }
        else {
            long diff;
            /* we store in the 'tmp' fields of the 'bb' arrays pointers to the
               vinfo_t that matched in 'aa'. We assume that all 'tmp' fields
               are NULL initially. If, walking in the 'bb' arrays, we encounter
               the same 'b' several times, we use these 'tmp' pointers to make
               sure they all matched the same 'a'. */
            if (b->tmp != NULL) {
                /* This 'b' has already be seen. */
                if (b->tmp == a)
                    continue; /* quotient graph */

                /* at this point, the graph 'aa' is not a quotient of the
                   graph 'bb', i.e. two nodes are shared in 'bb' but not
                   in 'aa'. This is not acceptable if it is a run-time
                   value, or if it is a virtual-time value whose identity
                   matters. */
                switch (gettime(b->source)) {
                    case RunTime:
                        goto incompatible;

                    case VirtualTime:
                        if (SVIRTUAL_MUTABLE(VirtualTime_Get(b->source)))
                            goto incompatible;
                        break;

                    default:;
                }
            }

            /* A new 'b', let's check if its 'a' matches. */
            if (a == NULL)
                goto incompatible; /* NULL not compatible with non-NULL */
            b->tmp = a;
            diff = ((long)a->source) ^ ((long)b->source);
            if (diff != 0) {
                if ((diff & TimeMask) != 0)
                    goto incompatible; /* not the same TIME_MASK */
                if (is_runtime(a->source)) {
                    if ((diff & RunTime_NoRef) != 0) {
                        /* from 'with ref' to 'without ref' or vice-versa:
                           a source in 'a' with reference cannot pass for
                           a source in 'b' without reference */
                        if ((a->source & RunTime_NoRef) == 0)
                            goto incompatible;
                    }
                }
                else {
                    if (is_virtualtime(a->source))
                        goto incompatible; /* different virtual sources */
                    if (KNOWN_SOURCE(a)->value != KNOWN_SOURCE(b)->value) {
                        if ((KNOWN_SOURCE(b)->refcount1_flags & SkFlagFixed) !=
                            0)
                            goto incompatible; /* b's value is fixed */
                        if ((KNOWN_SOURCE(a)->refcount1_flags & SkFlagFixed) !=
                                0 &&
                            KNOWN_SOURCE(a)->value == 0)
                            goto incompatible; /* hack: */
                        /* fixed known-to-be-zero values have a special
                           role to play with local variables: undefined
                           variables. These must *never* be un-promoted
                           to run-time, because we will get a NULL
                           pointer and a segfault. Argh. */
                        else {
                            /* approximative match, might un-promote 'a' from
                               compile-time to run-time. */
                            /*fprintf(stderr, "psyco: compatible_array() with
                             * vinfo_t* a=%p, b=%p\n", a, b);*/
                            int i, ocount = (*result)->count;
                            /* do not add several time the same value to the
                             * array */
                            for (i = 0; i < ocount; i++)
                                if ((*result)->items[i] == a)
                                    break;
                            if (i == ocount) {
                                *result = array_grow1(*result, ocount + 1);
                                (*result)->items[ocount] = a;
                            }
                        }
                    }
                }
            }
            if (a->array !=
                b->array) /* can only be equal if both ==NullArray */
            {
                if (is_compiletime(a->source)) {
                    /* For compile-time values we don't bother comparing
                       subarrays, because they only have a caching role
                       in this case; they should never contain information
                       different from what psyco_get_field() can load or
                       reload from immutable data. */
#if ALL_CHECKS
                    /* we just verify that there are only compile-time
                       subitems. */
                    int j;
                    for (j = 0; j < a->array->count; j++)
                        extra_assert(
                            a->array->items[j] == NULL ||
                            is_compiletime(a->array->items[j]->source));
                    for (j = 0; j < b->array->count; j++)
                        extra_assert(
                            b->array->items[j] == NULL ||
                            is_compiletime(b->array->items[j]->source));
#endif
                }
                else if (!compatible_array(a->array, b->array, result))
                    goto incompatible;
            }
        }
    }
    return true;

incompatible: /* we have to reset the 'tmp' fields to NULL,
                 but only as far as we actually progressed */
    for (; i >= 0; i--)
        if (bb->items[i] != NULL) {
            bb->items[i]->tmp = NULL;
            if (bb->items[i]->array != NullArray)
                clear_tmp_marks(bb->items[i]->array);
        }
    return false;
}

PSY_INLINE bool
fz_compatible_array(vinfo_array_t *aa, FrozenPsycoObject *fpo,
                    vcompatible_t *result)
{
    return compatible_array(aa, fpo->fz_vlocals, &result->diff);
}

#else /* if VLOCALS_OPC */

/* forward */
/* psyco_compatible_join: the known fields of a run-time value that the
   snapshot doesn't have are dropped at any depth, as compatible_array
   drops anything below the first two levels. A run-time object's known
   fields are loads of immutable fields (FMUT ones are never cached) or
   what it was computed from (a computed tuple's items), so the code they
   join reloads them; psyco_unify releases what they hold.
   Not the fields of other values: a virtual object's fields are its
   contents. compatible_rt_parent passes "aa belongs to a run-time value"
   into the next compatible_array() call. */
static bool compatible_drop_extra = false;
static bool compatible_rt_parent = false;
/* set by psyco_compatible_join: rule 1 does not apply at join points */
static bool compatible_at_join = false;

/* At a merge point, a known field of
   a run-time value (its type, mostly) that the snapshot lacks is dropped if
   nothing since the last merge point relied on it (sk->used, stamped by
   sk_mark_used, against po->mp_serial), instead of starting a new block:
   the block was compiled without the fact and loads or promotes it itself
   if it needs it. A match that drops is taken only if no block matches
   without dropping. Not at join points (they have their own rule), and
   not a virtual value's fields, which are its contents.
   Never in the compare that a promotion's continuation makes at its own
   instruction (psyco_compatible_keep_facts, cleared in vcompiler.c once
   that compare is over): the state there is the state before the
   promotion plus the promoted fact, and dropping it would jump back into
   the block that promoted, which promotes again, hits its own cache and
   jumps back, forever, without running an instruction. Every other compare
   comes after an instruction was compiled, so a chain of jumps between
   blocks without progress can only enter blocks that know more and
   more. */
DEFINEVAR bool psyco_compatible_keep_facts = false;

#if ALL_CHECKS
DEFINEVAR long psyco_stat_fact_drops = 0;
#endif
static bool compatible_drop_unused = false;
static unsigned long compatible_serial;
static int compatible_drops;

/* a compile was abandoned (codemanager.h, compile guards) */
DEFINEFN void
psyco_compile_abandoned(void)
{
    compatible_drop_extra = compatible_rt_parent = compatible_at_join = false;
    compatible_drop_unused = psyco_compatible_keep_facts = false;
}

static bool
compatible_unused_fact(vinfo_t *a, bool rt_parent)
{
    source_known_t *sk;
    if (!compatible_drop_unused || !rt_parent || !is_compiletime(a->source))
        return false;
    sk = CompileTime_Get(a->source);
    return (sk->refcount1_flags & SkFlagFixed) == 0 ||
           sk->used < compatible_serial;
}

static bool
compatible_array(vinfo_array_t *aa, int count, vinfo_array_t **result,
                 vinfo_array_t *reference, int recdepth, bool vint);
#if COMPRESS_COMPILETIME_SUBITEMS
static void
skip_compatible_array(int count);
#endif

/* approximative match: 'a' may be un-promoted from compile-time to run-time */
static void
compatible_diff(vinfo_array_t **result, vinfo_t *a)
{
    int i, ocount = (*result)->count;
    /* do not add several time the same value to the array */
    for (i = 0; i < ocount; i++)
        if ((*result)->items[i] == a)
            return;
    *result = array_grow1(*result, ocount + 1);
    (*result)->items[ocount] = a;
}

/* 'vint_value': 'a' is the value word of a virtual int or bool */
static bool
compatible_vinfo(vinfo_t *a, Source bsource, int bcount,
                 vinfo_array_t **result, vinfo_t *aref, int recdepth,
                 bool vint_value)
{
    /* Check if 'a' matches 'bsource'. */
    long diff;
    /*bool skip_subarray = false;*/

    /* If 'aref!=a' then 'aref' is a vinfo_t* that already passed the test
       against the same 'bsource'. In this case there is an extra test:
       as the two nodes 'a' and 'aref' are not shared in 'aa', but shared
       in 'bb', then they must not be run-time sources, because the
       compiler could have used this fact when compiling from 'bb'
       (typically, this single value was in a single register).  Nor can
       they be mutable virtual-time values, for identity reasons. */
    if (a != aref)
        switch (gettime(bsource)) {
            case RunTime:
                return false;

            case VirtualTime:
                if (SVIRTUAL_MUTABLE(VirtualTime_Get(bsource)))
                    return false;
                break;

            default:;
        }

    /* invariant */
    extra_assert(cmpinternal.tmp_counter <= cmpinternal.vcilink->time);

    if (a == NULL)
        return false; /* NULL not compatible with non-NULL */
    diff = ((long)a->source) ^ ((long)bsource);
    if (diff != 0) {
        if ((diff & TimeMask) != 0) {
            /* a virtual int's value, known here and run-time in 'b': the same
               family, so a partial match like two different constants;
               otherwise counters that are still 0 on some paths start
               a block per combination. Not a fixed value: code after it relies
               on it, and promotion would loop. */
            source_known_t *sk;
            if (is_compiletime(a->source) && is_megamorphic(bsource)) {
                /* forgotten (psyco_forget_values) or megamorphic in 'b': a
                   constant joins it, un-promoted, as two constants do;
                   otherwise the cap's forgotten form and the constant one
                   would each start a block at the merge points after it. Not a
                   fixed value, nor when 'b' knows more (a promoted type). */
                sk = CompileTime_Get(a->source);
                if ((sk->refcount1_flags & SkFlagFixed) != 0 ||
                    sk->value == 0 || bcount != 0)
                    return false;
                compatible_diff(result, a);
                return true;
            }
            if (!vint_value || !is_compiletime(a->source) ||
                !is_runtime(bsource))
                return false; /* not the same TIME_MASK */
            sk = CompileTime_Get(a->source);
            if ((sk->refcount1_flags & SkFlagFixed) != 0 ||
                (bsource & RunTime_NoRef) == 0 ||
                (sk->value < 0 && (bsource & RunTime_NonNeg) != 0))
                return false;
            compatible_diff(result, a);
            return bcount == 0 && a->array == NullArray;
        }

        switch (gettime(a->source)) {
            case RunTime:
                if ((diff & (RunTime_NoRef | RunTime_NonNeg |
                             RunTime_Megamorphic)) != 0) {
                    /* from 'with ref' to 'without ref' or vice-versa:
                       a source in 'a' with reference cannot pass for
                       a source in 'b' without reference */
                    if ((a->source & RunTime_NoRef) == 0 &&
                        (bsource & RunTime_NoRef) != 0)
                        return false;
                    /* from 'non-negative' to 'possibly negative' or
                       vice-versa: a source in 'a' which may be negative cannot
                       pass for a source in 'b' which cannot */
                    if ((a->source & RunTime_NonNeg) == 0 &&
                        (bsource & RunTime_NonNeg) != 0)
                        return false;
                    /* a source in 'a' which is flagged as megamorphic (or
                       forgotten) should not fall back to a source in 'b' which
                       is not: that code may promote it again */
                    if ((a->source & RunTime_Megamorphic) != 0 &&
                        (bsource & RunTime_Megamorphic) == 0)
                        return false;
                }
                break;

            case CompileTime:
                if (CompileTime_Get(a->source)->value !=
                    CompileTime_Get(bsource)->value) {
                    if ((CompileTime_Get(bsource)->refcount1_flags &
                         SkFlagFixed) != 0)
                        return false; /* b's value is fixed */
                    if ((CompileTime_Get(a->source)->refcount1_flags &
                         SkFlagFixed) != 0 &&
                        CompileTime_Get(a->source)->value == 0)
                        return false; /* hack: */
                    /* fixed known-to-be-zero values have a special
                       role to play with local variables: undefined
                       variables. These must *never* be un-promoted
                       to run-time, because we will get a NULL
                       pointer and a segfault. Argh. */
                    else
                        compatible_diff(result, a);
                }
                /*else
                  skip_subarray = true;*/
                break;

            default:          /* case VirtualTime */
                return false; /* different virtual sources */
        }
    }
    /*else
      skip_subarray = is_compiletime(bsource);*/

    if (bcount == 0 && a->array == NullArray)
        return true; /* shortcut */

#if COMPRESS_COMPILETIME_SUBITEMS
    if (/*skip_subarray*/ is_compiletime(bsource)) {
        /* For compile-time values we don't bother comparing
           subarrays, because they only have a caching role
           in this case; they should never contain information
           different from what psyco_get_field() can load or
           reload from immutable data. */
#if ALL_CHECKS
        /* we just verify that there are only compile-time
           subitems. */
        int j;
        for (j = 0; j < a->array->count; j++)
            extra_assert(a->array->items[j] == NULL ||
                         is_compiletime(a->array->items[j]->source));
#endif
        /* skip the subarray in the pseudo-code */
        skip_compatible_array(bcount);
        return true;
    }
#else  /* !COMPRESS_COMPILETIME_SUBITEMS */
    if (is_compiletime(bsource)) {
        extra_assert(bcount == 0);
        return true;
    }
#endif /* COMPRESS_COMPILETIME_SUBITEMS */

    compatible_rt_parent = is_runtime(a->source);
    return compatible_array(a->array, bcount, result, aref->array,
                            recdepth + 1, PsycoLong_IsVirtual(a));
}

/* The following function is the composition of fz_uncompress() and
   the above compatible_array(), optimized so that we need not build
   a whole uncompressed copy of the pseudo-code. */

static bool
compatible_array(vinfo_array_t *aa, int count, vinfo_array_t **result,
                 vinfo_array_t *reference, int recdepth, bool vint)
{
    /* In the following comments, 'bb' refers to the implicit intermediate
       array that would be created by fz_uncompress(). */
    /* 'reference' is a either 'aa', or another array of items that already
       passed the test against the same pseudo-code. This lets us check
       shared structures. */

    /* special-case test. See comments below. */
#define CHECK_FOR_NULL(a)                                               \
    if (a != NULL) {                                                    \
        if (compatible_unused_fact(a, rt_parent))                       \
            compatible_drops++;                                         \
        else {                                                          \
            /* if there is no proved progress, and 'a'                  \
               might just have been promoted,                           \
               then we have to let compilation go on */                 \
            if (*result == NullArray && is_compiletime(a->source) &&    \
                ((CompileTime_Get(a->source)->refcount1_flags &         \
                  SkFlagFixed) != 0))                                   \
                return false;                                           \
            /* if we are not too deep in the array, then we might as    \
               well try compiling with this extra information */        \
            if (recdepth <= 2 && !(compatible_drop_extra && rt_parent)) \
                return false;                                           \
        }                                                               \
    }

    bool rt_parent = compatible_rt_parent;
    int i;
    /* count = bb->count; */
    extra_assert(count >= 0);
    if (aa->count != count) {
        if (aa->count <
            count) /* array too short; ok only if the extra items */
        {          /*   in 'bb' are all NULL.                     */
            do {
                if (fz_getopc() != FZ_OPC_NULL)
                    return false; /* differs */
            } while (aa->count < --count);
        }
        else /* array too long */
        {
            /* the extra items in 'aa' must all be NULL. This is not strictly
               necessary for a compatibility (the above condition is more
               precise) but I guess that you get better results this way,
               because you have more information for a recompilation.
               XXX This assumption should be tested. */

            /* This potentially allows for unbounded array depth,
               so we disable it after a given depth: all extra items
               are then matched against b == NULL, as below. */
            for (i = aa->count; i > count;) {
                vinfo_t *a = aa->items[--i];
                CHECK_FOR_NULL(a)
            }
        }
    }
    for (i = count; i--;) {
        int opc = fz_getopc();
        vinfo_t *a = aa->items[i];

        /* invariant */
        extra_assert(cmpinternal.tmp_counter <= cmpinternal.vcilink->time);

        if (opc == FZ_OPC_NULL) /* 'b' is NULL */
        {
            /* if b == NULL, any value in 'a' is ok --
               with the exception of a fixed compile-time value, as
               created by a promotion. Without the following test,
               Psyco sometimes emits an infinite loop because the
               PsycoObject after promotion is found to be compatible
               with itself just before promotion. */
            CHECK_FOR_NULL(a)
        }
        else if (opc < 0) /* 'b' is a link to another 'b'. */
        {
            /* We cannot compare right now the 'a' with the corresponding 'b'
               because we do not know it yet. We record the comparison as
               "to be done later" in the linked list. */
            if (a == NULL)
                return false; /* shortcut: cannot match later */
            fz_pushstack(opc, a);
        }
        else /* 'b' is a vinfo_t */
        {
            Source bsource = fz_getarg();

            /* Save the current position (resolving links require backtracking)
             */
            signed char *saved_buf_opc = cmpinternal.buf_opc;
            Source *saved_buf_args = cmpinternal.buf_args;
            int saved_tmp_counter = cmpinternal.tmp_counter;

            /* Compare 'a' and 'bsource'. */
            extra_assert(i < reference->count);
            if (!compatible_vinfo(a, bsource, opc, result, reference->items[i],
                                  recdepth, vint && i == iLONG_VALUE))
                return false;

            /* Only after the above call we know if any link previously pending
               link just resolved into the current 'b'. */
            if (cmpinternal.tmp_counter == cmpinternal.vcilink->time) {
                bool ok = true;
                struct vcilink_s *pending =
                    NULL; /* links that resolve to 'b' */
                struct vcilink_s *p;
                vinfo_t *preva;

                while (1) {
                    /* Move all links with the current time to 'pending' */
                    while (cmpinternal.tmp_counter ==
                           cmpinternal.vcilink->time) {
                        p = cmpinternal.vcilink;
                        cmpinternal.vcilink = p->next;
                        p->next = pending;
                        pending = p;
                    }
                    extra_assert(cmpinternal.tmp_counter <
                                 cmpinternal.vcilink->time);
                    if (!pending)
                        break; /* done */

                    /* Resolve the next link */
                shortcut1:
                    p = pending;
                    pending = p->next;
                    preva = p->v.preva;
                    psyco_llfree_vci(p);

                    /* First check if the same link is found later in the
                       linked list, and ignore it if so (optimization). */
                    for (p = pending; p; p = p->next)
                        if (p->v.preva == preva)
                            goto shortcut1;

                    if (ok && preva != a) {
                        /* the two vinfo_ts shared in 'bb' are not shared in
                           'aa', i.e. two nodes are shared in 'bb' but not in
                           'aa'. We must compare the second node in 'aa' again
                           with the same single node of 'bb'. */
                        /* Backtracking */
#if ALL_CHECKS
                        signed char *buf_opc1 = cmpinternal.buf_opc;
                        Source *buf_args1 = cmpinternal.buf_args;
                        int tmp_counter1 = cmpinternal.tmp_counter;
#endif
                        cmpinternal.tmp_counter = saved_tmp_counter;
                        cmpinternal.buf_args = saved_buf_args;
                        cmpinternal.buf_opc = saved_buf_opc;
                        ok = compatible_vinfo(preva, bsource, opc, result, a,
                                              recdepth,
                                              vint && i == iLONG_VALUE);
#if ALL_CHECKS
                        if (ok) {
                            extra_assert(buf_opc1 == cmpinternal.buf_opc);
                            extra_assert(buf_args1 == cmpinternal.buf_args);
                            extra_assert(tmp_counter1 ==
                                         cmpinternal.tmp_counter);
                        }
#endif
                    }

                    /* go on with the loop even if !ok, to free all items left
                       in [pbegin, pend[. They must be freed now because they
                       are no longer members of the main linked list -- the
                       members of the linked list are freed automatically by
                       fz_compatible_array() */
                }

                if (!ok)
                    return false;
            }

            cmpinternal.tmp_counter++;
            extra_assert(cmpinternal.tmp_counter <= cmpinternal.vcilink->time);
        }
    }
    return true;
#undef CHECK_FOR_NULL
}

#if COMPRESS_COMPILETIME_SUBITEMS
/* same as 'compatible_array', faster, for when you are not
   interested in the result */
static void
skip_compatible_array(int count)
{
    int i;
    extra_assert(count >= 0);
    for (i = count; i--;) {
        int opc = fz_getopc();
        extra_assert(cmpinternal.tmp_counter <= cmpinternal.vcilink->time);
        if (opc == FZ_OPC_NULL)
            ;
        else if (opc < 0)
            ;
        else {
#if ALL_CHECKS
            Source bsource = fz_getarg();
            extra_assert(is_compiletime(bsource));
#else
            fz_getarg();
#endif
            skip_compatible_array(opc);

            /* no link should resolve now, fz_compress() takes care of that */
            extra_assert(cmpinternal.tmp_counter < cmpinternal.vcilink->time);
            cmpinternal.tmp_counter++;
        }
    }
}
#endif /* COMPRESS_COMPILETIME_SUBITEMS */

PSY_INLINE bool
fz_compatible_array(vinfo_array_t *aa, FrozenPsycoObject *fpo,
                    vcompatible_t *result)
{
    bool ok;
    fz_load_fpo_stack(fpo);
    compatible_rt_parent = false;
    ok = compatible_array(aa, fz_getopc(), &result->diff, aa, 0, false);

    /* free the linked list (it should already be free if 'ok') */
    while (cmpinternal.vcilink != &cmpinternal.sentinel) {
        struct vcilink_s *p = cmpinternal.vcilink;
        cmpinternal.vcilink = p->next;
        psyco_llfree_vci(p);
    }

    return ok;
}

#endif /* VLOCALS_OPC */

/* whether releasing 'a' would release a run-time reference, which needs
   code emitted */
static bool
holds_reference(vinfo_array_t *a)
{
    int i;
    for (i = 0; i < a->count; i++) {
        vinfo_t *vi = a->items[i];
        if (vi == NULL)
            continue;
        if (is_runtime(vi->source) && (vi->source & RunTime_NoRef) == 0)
            return true;
        if (holds_reference(vi->array))
            return true;
    }
    return false;
}

/* Dead locals keep their values, which a frame's f_locals, its traceback
   frame or a tracer can ask for after any call or raise, and which
   CPython keeps until they are rebound or deleted. Only what is known
   about a run-time one is dropped, its loaded fields (its type, mostly),
   since nothing reads it again: so states differing in those share code.
   Not a value something else holds too, nor a virtual one, whose fields
   are its contents, nor fields holding a reference, whose release would
   emit code (this runs before a function's first buffer, too). */
DEFINEFN
void
psyco_forget_dead_vars(PsycoObject *po, global_entries_t *ge)
{
    int i, limit;
    PyObject *plist = ge->fatlist;
    extra_assert(PyList_Check(plist));
    limit = PyList_GET_SIZE(plist);
    for (i = 0; i < limit; i++) {
        vinfo_t *vi;
        vinfo_array_t *a;
        PyObject *o1 = PyList_GET_ITEM(plist, i);
        if (!PyLong_Check(o1))
            break;
        vi = LOC_LOCALS_PLUS[PyLong_AS_LONG(o1)];
        if (vi == NULL || !is_runtime(vi->source) || vi->refcount != 1 ||
            vi->array == NullArray || holds_reference(vi->array))
            continue;
        a = vi->array;
        vi->array = NullArray;
        array_delete(a, po);
    }
}

/* Call alignment. Compiled code aligns its C calls
   from the position of the return-address slot (STACK_DEPTH_SINCE_CALL),
   and unification can't move that slot, so a state may only jump into a
   block whose return address is at the same position mod 16. Only that
   slot is compared: the frame locals stored under it (codegen.c) are
   visited after it and must not decide the test, or a call with more
   run-time arguments would take a block compiled for one with fewer. */
typedef struct {
    vinfo_t *ret; /* the live state's return-address slot */
    int match;    /* -1 until it is visited */
} alignment_test_t;

static vinfo_t *
return_address_slot(PsycoObject *po)
{
    if (po->pr.is_inlining && LOC_INLINING != NULL)
        return LOC_INLINING->array
            ->items[TOTAL_LOC_INLINE + INDEX_LOC_CONTINUATION];
    return LOC_CONTINUATION;
}

static void
compare_return_address(vinfo_t *a, RunTimeSource bsource, alignment_test_t *t)
{
    if (a == t->ret)
        t->match = (getstack(a->source) % 16) == (getstack(bsource) % 16);
}

/* One form for small ints and bools at merge points:
   a local or stack slot holding a constant int object becomes a virtual int
   with that constant value, the form arithmetic produces. Otherwise "still
   the constant 0" and "a virtual int" never match, and every combination of
   them starts a new block. Only small ints and bools, which PyLong_FromLong
   and PyBool_FromLong return as the same object when computed, so identity
   is kept; never a fixed (promoted) constant. Only locals: they always hold
   an object (LOAD_CONST's constants have no SkFlagPyObj to tell). Done
   from the state alone, before both the comparison and the snapshot: a
   respawn replay starts from the rewritten snapshot.
   The vinfo is rewritten in place only if locals are all that hold it.
   Otherwise it is also a known field of something, often of a constant
   ('a, b = t' with a constant tuple t, an inlined call's constant
   argument), which must stay constant there (vcompiler.c:coherent_array):
   the locals holding it get a new virtual int instead. */
static void
canonical_int(PsycoObject *po, int index)
{
    vinfo_t *a = LOC_LOCALS_PLUS[index];
    vinfo_t *v;
    source_known_t *sk;
    PyObject *o;
    source_virtual_t *sv;
    long value;
    int i, in_locals, nlocals = le_code_nlocals(po->pr.co);
    if (a == NULL || !is_compiletime(a->source))
        return;
    sk = CompileTime_Get(a->source);
    /* fixed: promoted, or 0 for an unbound local */
    if ((sk->refcount1_flags & SkFlagFixed) || sk->value == 0)
        return;
    o = (PyObject *)sk->value;
    if (Py_TYPE(o) == &PyBool_Type) {
        sv = &psyco_computed_bool;
        value = o == Py_True;
    }
    else if (Py_TYPE(o) == &PyLong_Type && -1 <= le_long_size(o) &&
             le_long_size(o) <= 1) {
        value = PyLong_AsLong(o);
        if (value < -5 || value > 256) /* longobject.c's small_ints */
            return;
        sv = &psyco_computed_long;
    }
    else
        return;
    /* the locals before 'index' hold no constant small int any more */
    in_locals = 0;
    for (i = index; i < nlocals; i++) in_locals += LOC_LOCALS_PLUS[i] == a;
    if (a->refcount == in_locals) {
        vinfo_array_shrink(po, a, 0); /* cached fields of the object */
        a->source = VirtualTime_New(sv);
        sk_decref(sk);
        v = a;
    }
    else
        v = vinfo_new(VirtualTime_New(sv));
    v->array = array_new(LONG_TOTAL);
    v->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)Py_TYPE(o)));
    v->array->items[iLONG_VALUE] = vinfo_new(CompileTime_New(value));
    if (v != a)
        for (i = index; i < nlocals; i++)
            if (LOC_LOCALS_PLUS[i] == a) {
                if (--in_locals > 0)
                    vinfo_incref(v);
                LOC_LOCALS_PLUS[i] = v;
                vinfo_decref(a, po);
            }
}

/* What the blocks at one merge point hold in each top-level item, built
   up as blocks are added: the cap's generalisation asks at every new
   state that matches none of them, and there may be hundreds. Per item:
   VARIES_NONE if no block reaches it, VARIES_ONE if every block that
   does holds the constant value[i], VARIES_MANY if two differ or one holds
   anything else (a link to a shared item counts). Dropped when a block
   leaves the list (psyco_ge_changed). */
#define VARIES_NONE 0
#define VARIES_ONE 1
#define VARIES_MANY 2
struct varies_s {
    Py_ssize_t seen; /* items of the fatlist read */
    int cap;
    long *value;
    char *state;
};

DEFINEFN void
psyco_ge_changed(global_entries_t *ge)
{
    struct varies_s *v = ge->varies;
    if (v != NULL) {
        ge->varies = NULL;
        PyMem_Free(v->value);
        PyMem_Free(v);
    }
}

static void
varies_read_block(struct varies_s *vc, CodeBufferObject *b)
{
    int n;
    fz_load_fpo(&b->snapshot);
    n = fz_getopc();
    if (n > vc->cap) {
        int cap = n + 8;
        long *v = PyMem_Realloc(vc->value, cap * (sizeof(long) + 1));
        if (v == NULL)
            OUT_OF_MEMORY();
        /* the states follow the values */
        memmove(v + cap, v + vc->cap, vc->cap);
        memset((char *)(v + cap) + vc->cap, VARIES_NONE, cap - vc->cap);
        vc->value = v;
        vc->state = (char *)(v + cap);
        vc->cap = cap;
    }
    while (n-- > INDEX_LOC_LOCALS_PLUS) {
        int opc = fz_getopc();
        Source s = 0;
        char *st = &vc->state[n];
        if (opc >= 0) {
            s = fz_getarg();
            fz_parse(opc, false);
        }
        if (*st == VARIES_MANY)
            continue;
        if (!is_compiletime(s))
            *st = VARIES_MANY;
        else if (*st == VARIES_NONE) {
            *st = VARIES_ONE;
            vc->value[n] = CompileTime_Get(s)->value;
        }
        else if (vc->value[n] != CompileTime_Get(s)->value)
            *st = VARIES_MANY;
    }
}

/* for each local i with want[i] != 0, whether a block at this merge point
   holds anything but the constant want[i] there: varies[i] */
static void
varies_at(global_entries_t *ge, int nlocals, const long *want, char *varies)
{
    PyObject *plist = ge->fatlist;
    Py_ssize_t j, len = PyList_GET_SIZE(plist);
    struct varies_s *vc = ge->varies;
    int k;
    if (vc == NULL) {
        vc = PyMem_Malloc(sizeof(struct varies_s));
        if (vc == NULL)
            OUT_OF_MEMORY();
        vc->seen = 0;
        vc->cap = 0;
        vc->value = NULL;
        vc->state = NULL;
        ge->varies = vc;
    }
    /* blocks are appended after the dead locals' numbers (mergepoints.c);
       a block read again, after running out of memory, changes nothing */
    for (j = vc->seen; j < len; j++) {
        PyObject *o1 = PyList_GET_ITEM(plist, j);
        if (CodeBuffer_Check(o1))
            varies_read_block(vc, (CodeBufferObject *)o1);
    }
    vc->seen = len;
    for (k = 0; k < nlocals; k++) {
        int index = INDEX_LOC_LOCALS_PLUS + k;
        int st = index < vc->cap ? vc->state[index] : VARIES_NONE;
        varies[k] = want[k] != 0 &&
                    (st == VARIES_MANY ||
                     (st == VARIES_ONE && vc->value[index] != want[k]));
    }
}

/* The cap's generalisation (vcompiler.c:mp_capped), in the
   locals only.
   - The value of each virtual int or bool becomes run-time, so the
     counters' many combinations of known values collapse into one block.
   - Each other constant that some block at this merge point doesn't share
     becomes run-time and forgotten (RunTime_Megamorphic): its promotions
     take the generic path (psyco_pyobj_atcompiletime_mega, the static-type
     one, and the calls in pfunction_call and PsycoCFunction_Call), so a
     constant function or class is not promoted again by the next call,
     which would be one more fixed form here (meteor_contest +5% compiled
     without the mark). A constant arriving later joins a forgotten value
     (compatible_vinfo). One that every block shares stays: forgetting it
     only spreads a second form to the merge points after this one
     (meteor_contest's bisect=bisect, +5% again). Its type is not kept:
     Psyco_NeedType promotes it once, the same type for any function.
   - Fixed values stay: something relies on them, and a promotion compiled
     from this merge point would match the state it was promoted from and
     loop.
   - Virtual values stay virtual: computing one emits a call with an error
     check, a respawn point that a replay from this block's snapshot could
     never reach. Only loads of constants are emitted.
   - The result must be as general as the states that will arrive, which
     have no sharing and no sign known: a run-time value shared in a
     snapshot never matches two separate ones (compatible_vinfo). So a
     local whose value is shared (a = b = 0) gets its own; for ints only
     small ints and bools, which computing gives back as the same object,
     so identity is kept (canonical_int); a constant object is loaded as
     the same object either way.
   Returns whether anything changed. */
DEFINEFN
bool
psyco_forget_values(PsycoObject *po, global_entries_t *ge)
{
    bool changed = false;
    int i, nlocals;
    long *want;
    char *varies;
    for (i = 0; i < le_code_nlocals(po->pr.co); i++) {
        vinfo_t *a = LOC_LOCALS_PLUS[i];
        vinfo_t *w;
        long value;
        if (a == NULL || !PsycoLong_IsVirtual(a) ||
            a->array->count <= iLONG_VALUE)
            continue;
        w = a->array->items[iLONG_VALUE];
        if (w == NULL || !is_compiletime(w->source) ||
            (CompileTime_Get(w->source)->refcount1_flags & SkFlagFixed))
            continue;
        value = CompileTime_Get(w->source)->value;
        if ((a->refcount > 1 || w->refcount > 1) && -5 <= value &&
            value <= 256) {
            vinfo_t *b = vinfo_new(a->source);
            b->array = array_new(LONG_TOTAL);
            if ((b->array->items[iOB_TYPE] = a->array->items[iOB_TYPE]) !=
                NULL)
                vinfo_incref(b->array->items[iOB_TYPE]);
            b->array->items[iLONG_VALUE] = w =
                vinfo_new(CompileTime_New(value));
            LOC_LOCALS_PLUS[i] = b;
            vinfo_decref(a, po);
        }
        psyco_unfix(po, w);
        w->source &= ~RunTime_NonNeg;
        changed = true;
    }
    /* kept between calls, so that a compile abandoned on the way leaks
       nothing */
    static long *table = NULL;
    static int tablecap = 0;
    nlocals = le_code_nlocals(po->pr.co);
    if (nlocals > tablecap) {
        long *t = PyMem_Realloc(table, nlocals * (sizeof(long) + 1));
        if (t == NULL)
            OUT_OF_MEMORY();
        table = t;
        tablecap = nlocals;
    }
    want = table;
    varies = (char *)(want + tablecap);
    for (i = 0; i < nlocals; i++) {
        vinfo_t *a = LOC_LOCALS_PLUS[i];
        source_known_t *sk;
        want[i] = 0;
        varies[i] = 0;
        if (a == NULL || !is_compiletime(a->source))
            continue;
        sk = CompileTime_Get(a->source);
        if ((sk->refcount1_flags & SkFlagFixed) == 0)
            want[i] = sk->value;
    }
    varies_at(ge, nlocals, want, varies);

    for (i = 0; i < nlocals; i++) {
        vinfo_t *a = LOC_LOCALS_PLUS[i];
        source_known_t *sk;
        if (!varies[i] || a == NULL || !is_compiletime(a->source))
            continue;
        sk = CompileTime_Get(a->source);
        if ((sk->refcount1_flags & SkFlagFixed) || sk->value != want[i])
            continue;
        if (a->refcount > 1) {
            vinfo_t *b;
            if (sk->refcount1_flags & SkFlagPyObj)
                Py_INCREF((PyObject *)sk->value);
            b = vinfo_new(CompileTime_NewSk(
                sk_new(sk->value, sk->refcount1_flags & SkFlagPyObj)));
            LOC_LOCALS_PLUS[i] = b;
            vinfo_decref(a, po);
            a = b;
        }
        psyco_unfix(po, a);
        a->source = (a->source | RunTime_Megamorphic) & ~RunTime_NonNeg;
        changed = true;
    }
    return changed;
}

/* A second form at a merge point (vcompiler.c:psyco_compile): each constant
   int object in a local that canonical_int left alone (outside -5..256)
   becomes run-time, unless fixed. An int local otherwise takes three forms
   that never match each other: a small int as a virtual int, a large one as
   a constant object, and a run-time object once two large constants meet
   (deltablue's change(v, 17 / 1050 / 5 / 2000)). Un-promoting keeps the
   very object, so identity is kept; writing it as a virtual int would not
   (computing it makes a new object). The run-time copy is a load of the
   constant, no respawn point. Returns whether anything changed. */
DEFINEFN
bool
psyco_unpromote_large_ints(PsycoObject *po)
{
    bool changed = false;
    int i;
    for (i = 0; i < le_code_nlocals(po->pr.co); i++) {
        vinfo_t *a = LOC_LOCALS_PLUS[i];
        source_known_t *sk;
        if (a == NULL || !is_compiletime(a->source))
            continue;
        sk = CompileTime_Get(a->source);
        if ((sk->refcount1_flags & SkFlagFixed) || sk->value == 0 ||
            Py_TYPE((PyObject *)sk->value) != &PyLong_Type)
            continue;
        psyco_unfix(po, a);
        changed = true;
    }
    return changed;
}

#if LE_CODESTATS
/* candidates the last psyco_compatible() rejected only for the alignment
   test */
DEFINEVAR int psyco_cs_align_rejects = 0;
#endif

/* The summary (dispatcher.h) holds only what compatible_array rejects on
   at the top level whatever else it finds: against a snapshot's run-time
   item that isn't megamorphic, only a run-time one passes (a constant
   joins only a megamorphic one); against a constant only a constant, and
   against a virtual only a virtual; NULL, a megamorphic item and anything
   past the first 64 accept anything. A shared item is compared as the
   item it is shared with, so the same holds for links. In the live state
   00 is NULL. Inside an inlined frame's LOC_INLINING, compared with the
   same rules, the fixed constants it starts with must be equal, and
   compatible_array reads them last, after the whole parent frame, whose
   items' first 64 are summarised as the top level's are. */
static unsigned long
summary_inline_key(vinfo_t *v, bool snapshot)
{
    unsigned long h = 0;
    int k;
    if (v == NULL || !is_virtualtime(v->source) ||
        v->array->count < TOTAL_LOC_INLINE)
        return 0;
    for (k = 0; k < TOTAL_LOC_INLINE; k++) {
        vinfo_t *w = v->array->items[k];
        source_known_t *sk;
        if (w == NULL || !is_compiletime(w->source))
            return 0;
        sk = CompileTime_Get(w->source);
        if (snapshot && (sk->refcount1_flags & SkFlagFixed) == 0)
            return 0;
        h = (h ^ (unsigned long)sk->value) * 0x9E3779B97F4A7C15UL;
    }
    return h | 1;
}

static void
summary_classes(vinfo_array_t *aa, int first, bool snapshot, unsigned long *c1,
                unsigned long *c0)
{
    int i, n = aa->count - first;
    unsigned long b1 = 0, b0 = 0;
    if (n > 64)
        n = 64;
    for (i = 0; i < n; i++) {
        vinfo_t *a = aa->items[first + i];
        if (a == NULL)
            continue;
        if (is_compiletime(a->source))
            b1 |= 1UL << i;
        else if (is_virtualtime(a->source)) {
            b1 |= 1UL << i;
            b0 |= 1UL << i;
        }
        else if (!snapshot || !is_megamorphic(a->source))
            b0 |= 1UL << i;
    }
    *c1 = b1;
    *c0 = b0;
}

static void
summary_of(fz_summary_t *s, vinfo_array_t *aa, bool snapshot)
{
    vinfo_t *inl =
        aa->count > INDEX_LOC_INLINING ? aa->items[INDEX_LOC_INLINING] : NULL;
    summary_classes(aa, 0, snapshot, &s->head.b1, &s->head.b0);
    s->head.b1 &= ~SUMMARY_REST;
    s->head.b0 |= SUMMARY_REST;
    s->inline_key = summary_inline_key(inl, snapshot);
    if (inl != NULL && is_virtualtime(inl->source) &&
        inl->array->count > TOTAL_LOC_INLINE)
        summary_classes(inl->array, TOTAL_LOC_INLINE, snapshot, &s->p1,
                        &s->p0);
    else
        s->p1 = s->p0 = 0;
}

DEFINEFN bool
psyco_summary_short(vinfo_array_t *aa)
{
    vinfo_t *inl =
        aa->count > INDEX_LOC_INLINING ? aa->items[INDEX_LOC_INLINING] : NULL;
    return inl == NULL || !is_virtualtime(inl->source);
}

DEFINEFN void
psyco_summary_build(fz_summary_t *s, vinfo_array_t *aa)
{
    summary_of(s, aa, true);
}

DEFINEFN void
psyco_summary_build_short(fz_summary_head_t *s, vinfo_array_t *aa)
{
    fz_summary_t full;
    summary_of(&full, aa, true);
    extra_assert(psyco_summary_short(aa) && full.inline_key == 0 &&
                 full.p1 == 0 && full.p0 == 0);
    *s = full.head;
    s->b0 &= ~SUMMARY_REST;
}

/* whether the summaries say that 'a' matches no state the snapshot of 'b'
   matches: by the top-level items, then, if 'b' has SUMMARY_REST (which
   'a' has too, so that bit never rejects), by the inlined frame */
PSY_INLINE bool
summary_rejects(fz_summary_t *a, CodeBufferObject *codebuf)
{
    fz_summary_head_t *b = &codebuf->summary.head;
    fz_summary_t *f;
    if (((b->b1 | b->b0) & ((a->head.b1 ^ b->b1) | (a->head.b0 ^ b->b0))) != 0)
        return true;
    if (!(b->b0 & SUMMARY_REST))
        return false;
    f = &codebuf->summary.full;
    return (f->inline_key != 0 && a->inline_key != f->inline_key) ||
           ((f->p1 | f->p0) & ((a->p1 ^ f->p1) | (a->p0 ^ f->p0))) != 0;
}

DEFINEFN
vcompatible_t *
psyco_compatible(PsycoObject *po, global_entries_t *patterns)
{
    static vcompatible_t
        result; /* argh, a global variable -- well, the rest of
                   the implementation is not re-entrant anyway */
    vinfo_array_t *bestresult = NULL; /* best result so far */
    CodeBufferObject *bestbuf = NULL;
    CodeBufferObject *dropbuf = NULL; /* first match that drops facts */
    fz_summary_t summary;
    bool summarised = false;
    int i;
    PyObject *plist = patterns->fatlist;
    extra_assert(PyList_Check(plist));
    for (i = 0; i < le_code_nlocals(po->pr.co); i++) canonical_int(po, i);
    compatible_drop_unused =
        !compatible_at_join && !psyco_compatible_keep_facts;
    /* facts stamped since the last merge point are in use. 0 (all kept)
       at a call's entry, where the caller's facts about the arguments are
       what the callee specialises on, and in a respawned branch, which
       starts from a snapshot that doesn't keep the serial */
    compatible_serial = po->mp_serial;
#if LE_CODESTATS
    psyco_cs_align_rejects = 0;
#endif
    i = PyList_GET_SIZE(plist);
    while (i--) /* the most dummy algorithm: step by step in the list, */
    {           /* checking for a match at each step.                  */
        CodeBufferObject *codebuf;
        PyObject *o1 = PyList_GET_ITEM(plist, i);
        /* the dead locals' numbers come first (mergepoints.c) */
        if (PyLong_CheckExact(o1))
            break;
        codebuf = (CodeBufferObject *)o1;
        if (!summarised) {
            summary_of(&summary, &po->vlocals, false);
            summarised = true;
        }
        if (summary_rejects(&summary, codebuf)) {
#if ALL_CHECKS
            /* the full compare must reject it too */
            vcompatible_t check = {codebuf, NullArray};
            int drops = compatible_drops;
            extra_assert(!fz_compatible_array(&po->vlocals, &codebuf->snapshot,
                                              &check));
            fz_check_invariant(&codebuf->snapshot);
            array_release(check.diff);
            compatible_drops = drops;
#endif
            continue;
        }
        result.matching = codebuf;
        result.diff = NullArray;
        compatible_drops = 0;
        fz_check_invariant(&codebuf->snapshot);
        if (fz_compatible_array(&po->vlocals, &codebuf->snapshot, &result)) {
            alignment_test_t alignment = {return_address_slot(po), -1};
            fz_find_runtimes(&po->vlocals, &codebuf->snapshot,
                             (fz_find_fn)&compare_return_address, &alignment,
                             false);
            /*
             * compatible_array() leaves data in the 'tmp' fields. It must be
             * cleared unless it is the final result of psyco_compatible()
             * itself.
             */
            if (alignment.match != 1) {
#if LE_CODESTATS
                psyco_cs_align_rejects++;
#endif
                fz_restore_invariant(&codebuf->snapshot);
                array_release(result.diff);
                continue;
            }
            if (result.diff == NullArray && compatible_drops > 0) {
                /* a match that drops unused facts: taken only if no block
                   matches without dropping */
                fz_restore_invariant(&codebuf->snapshot);
                if (dropbuf == NULL)
                    dropbuf = codebuf;
            }
            else if (result.diff == NullArray) {
                /* Total match */
                if (bestresult != NULL)
                    array_release(bestresult);
                compatible_drop_unused = false;
                return &result;
            }
            else {
                /* Partial match, clear 'tmp' fields */
                fz_restore_invariant(&codebuf->snapshot);
                if (bestresult != NULL) {
                    if (bestresult->count <= result.diff->count) {
                        array_release(result.diff);
                        continue; /* not better than the previous partial match
                                   */
                    }
                    array_release(bestresult);
                }
                /* Record the best partial match we found so far */
                bestresult = result.diff;
                bestbuf = codebuf;
            }
        }
        else {
            /* compatible_array() should have reset all 'tmp' fields; the
               differences it found before failing are dropped */
            fz_check_invariant(&codebuf->snapshot);
            array_release(result.diff);
        }
    }
    if (dropbuf != NULL) {
        /* compare again, to leave the 'tmp' fields that psyco_unify reads */
        bool ok;
        if (bestresult != NULL)
            array_release(bestresult);
        result.matching = dropbuf;
        result.diff = NullArray;
        ok = fz_compatible_array(&po->vlocals, &dropbuf->snapshot, &result);
        psyco_assert(ok && result.diff == NullArray);
        compatible_drop_unused = false;
        LE_STAT(psyco_stat_fact_drops++);
        return &result;
    }
    compatible_drop_unused = false;
    if (bestresult == NULL)
        return NULL;
    else {
        result.matching = bestbuf;
        result.diff = bestresult;
        return &result;
    }
}

DEFINEFN
vcompatible_t *
psyco_compatible_join(PsycoObject *po, global_entries_t *patterns,
                      bool drop_fields)
{
    vcompatible_t *r;
    compatible_drop_extra = drop_fields;
    compatible_at_join = true;
    r = psyco_compatible(po, patterns);
    compatible_at_join = false;
    compatible_drop_extra = false;
    return r;
}

DEFINEFN
void
psyco_stabilize(vcompatible_t *lastmatch)
{
    if (lastmatch->diff == NullArray)
        fz_restore_invariant(&lastmatch->matching->snapshot);
    else
        fz_check_invariant(&lastmatch->matching->snapshot);
    array_release(lastmatch->diff);
}

/***************************************************************/
/***                         Unification                       ***/
/***************************************************************/

/* psyco_unify() is implemented in idispatcher.c */

DEFINEFN
CodeBufferObject *
psyco_unify_code(PsycoObject *po, vcompatible_t *lastmatch)
{
    /* simplified interface to psyco_unify() without using a previously
       existing code buffer. */

    CodeBufferObject *target;
    code_t localbuf[GUARANTEED_MINIMUM];
    /* relies on the fact that psyco_unify() has no room at all in localbuf.
       Anything but the final JMP will trigger the creation of a new code
       buffer. */
    po->code = localbuf;
    INIT_CODE_EMISSION(po->code);
    po->codelimit = NULL;
    psyco_unify(po, lastmatch, &target);
    return target;
}

#define KEEP_MARK ((vinfo_t *)1)

static int
mark_to_keep(vinfo_array_t *array, bool virtual_parent)
{
    int i, total = 0;
    for (i = array->count; i--;) {
        vinfo_t *vi = array->items[i];
        if (vi != NULL) {
            if (is_runtime(vi->source) && vi->tmp == NULL) {
                if (!virtual_parent)
                    continue;
                /* mark this item to be kept */
                vi->tmp = KEEP_MARK;
                total++;
            }
            if (vi->array != NullArray)
                total += mark_to_keep(vi->array, is_virtualtime(vi->source));
        }
    }
    return total;
}

static void
remove_non_marked(vinfo_array_t *array, PsycoObject *po)
{
    int i;
    for (i = array->count; i--;) {
        vinfo_t *vi = array->items[i];
        if (vi != NULL) {
            if (is_runtime(vi->source) && vi->tmp == NULL) {
                /* remove this item */
                array->items[i] = NULL;
                vinfo_decref(vi, po);
            }
            else if (vi->array != NullArray) {
                if (is_compiletime(vi->source)) {
                    /* remove all sub-items */
                    vinfo_array_t *array = vi->array;
#if ALL_CHECKS
                    /* we just verify that there are only compile-time
                       subitems. */
                    int j;
                    for (j = 0; j < array->count; j++)
                        extra_assert(array->items[j] == NULL ||
                                     is_compiletime(array->items[j]->source));
#endif
                    vi->array = NullArray;
                    array_delete(array, po);
                }
                else
                    remove_non_marked(vi->array, po);
            }
        }
    }
}

DEFINEFN
int
psyco_simplify_array(vinfo_array_t *array, PsycoObject *po)
{
    /* We remove a run-time vinfo_t if it is not directly in 'array' and
       if it is not in the sub-vinfo_array_t of any virtual-time source. */

    /* First mark with a non-NULL value all run-time sources that must
       be kept. */
    int total;
    assert_cleared_tmp_marks(array);
    total = mark_to_keep(array, true);

    /* Remove all non-marked run-time sources */
    remove_non_marked(array, po);

    /* Done */
    return total;
}

static void
remove_non_compiletime(vinfo_t *v, PsycoObject *po)
{
    vinfo_array_t *array = v->array;
    int j, length = array->count, newlength = 0;
    for (j = 0; j < length; j++) {
        vinfo_t *vi = array->items[j];
        if (vi != NULL) {
            if (!is_compiletime(vi->source)) {
                array->items[j] = NULL;
                vinfo_decref(vi, po);
            }
            else
                newlength = j + 1;
        }
    }
    vinfo_array_shrink(po, v, newlength);
}

/*****************************************************************/
/***                Promotion and un-promotion                 ***/
/*****************************************************************/

/*****************************************************************/
/***   Promotion of a run-time variable into a fixed           ***/
/***   compile-time one                                        ***/
/*****************************************************************/

/* Implementation tactics */

/*
 A promotion's code buffers are in a chained list, with the most used
 items moving forward in the list. Ordering them as a binary search tree,
 the most used nodes moving up, is not implemented.
*/

typedef struct rt_local_buf_s {
    struct rt_local_buf_s *next;
    long key;
} rt_local_buf_t;

typedef struct { /* produced at compile time and read by the dispatcher */
    INTERNAL_PROMOTION_FIELDS

    parked_state_t state; /* before promotion, and the variable to promote */

    rt_local_buf_t *local_chained_list;
#if LE_CODESTATS
    int cs_site; /* pycompiler.c, psyco_cs_promo_site */
#endif
} rt_promotion_t;

/* this macro might be redefined below */
#define QUICK_LOOKUP_PROMOTION_VALUE(fs, value, result) \
    result = lookup_old_promotion_values(fs, value)

#if PROMOTION_FAST_COMMON_CASE
static
#else
PSY_INLINE
#endif
    code_t *
    lookup_old_promotion_values(rt_promotion_t *fs, long value)
{
    rt_local_buf_t *prevbuf;
    if (fs->local_chained_list == NULL)
        return NULL; /* not found (list is empty) */

#if PROMOTION_FAST_COMMON_CASE
    /* 'fs->local_chained_list' points to the current head
       of the list, which we know is not what we are looking
       for because otherwise the CMP/JE instructions
       would have found it and we would not be here */
    /* XXX However, I've actually seen this failing once.  I couldn't make
       sense of the failure except by assuming that the CPU instruction
       cache was somehow out of date.  To cope for that case, see
       the XXX below... */
    /*extra_assert(fs->local_chained_list->key != value);*/
#else
    if (fs->local_chained_list->key == value)
        return (code_t *)(fs->local_chained_list +
                          1); /* it is the head of the list */
#endif

    prevbuf = fs->local_chained_list;
    while (1) {
        rt_local_buf_t *buf = prevbuf->next;
        if (buf == NULL)
            break; /* not found (list exhausted) */
        if (buf->key == value) {
            /* found inside the list, put it at the head */
            prevbuf->next = buf->next;
            buf->next = fs->local_chained_list;
            fs->local_chained_list = buf;
            return (code_t *)(buf + 1);
        }
        prevbuf = buf;
    }
#if PROMOTION_FAST_COMMON_CASE
    /* XXX the following condition is normally impossible,
       but see the XXX above. */
    if (fs->local_chained_list->key == value)
        return (code_t *)(fs->local_chained_list + 1);
#endif
    return NULL;
}

#if PROMOTION_FAST_COMMON_CASE
static int quick_lookup_counter = 0;
#undef QUICK_LOOKUP_PROMOTION_VALUE
#define QUICK_LOOKUP_PROMOTION_VALUE(fs, value, res)              \
    do {                                                          \
        rt_local_buf_t *buf = (fs)->local_chained_list;           \
        if (buf == NULL) {                                        \
            res = NULL; /* not found (list is empty) */           \
        }                                                         \
        else if ((quick_lookup_counter -= 13) >= 0) {             \
            for (buf = buf->next; buf != NULL; buf = buf->next) { \
                if (buf->key == (value))                          \
                    return (code_t *)(buf + 1);                   \
            }                                                     \
            /* XXX workaround, see notes above about CPU cache */ \
            if ((fs)->local_chained_list->key == (value))         \
                return (code_t *)((fs)->local_chained_list + 1);  \
            res = NULL; /* not found */                           \
        }                                                         \
        else {                                                    \
            /* approximately once every 23 times, go through */   \
            /* the slower path that will move the item to    */   \
            /* the head of the search list if it is found.   */   \
            quick_lookup_counter += 307;                          \
            res = lookup_old_promotion_values(fs, value);         \
        }                                                         \
    } while (0)
#endif

/* the values compiled at the site; the megamorphic version (marker -1) is
   not one: at a static-type site it is made for the first heap type, and
   counting it would leave room for one static type fewer */
PSY_INLINE int
lookup_count_previous_values(rt_promotion_t *fs, rt_local_buf_t **megabuf)
{
    int result = 0;
    rt_local_buf_t *buf = fs->local_chained_list;
    while (buf) {
        if (buf->key == -1) /* use (PyObject*)(-1) as a marker */
            *megabuf = buf;
        else
            result++;
        buf = buf->next;
    }
    return result;
}

static code_t *
do_promotion_internal_body(rt_promotion_t *fs, long key, source_known_t *sk);
/* 'flags' are the constant's: with SkFlagPyObj, 'key' is an object. Out
   of line, as sigsetjmp would slow its callers (eval_hook.c) */
static __attribute__((noinline)) code_t *
do_promotion_internal(rt_promotion_t *fs, long key, long flags)
{
    code_t *r;
    le_compile_guard_t g;
#if LE_CODESTATS
    psyco_cs_promo_begin(fs->cs_site, key, (flags & SkFlagPyObj) != 0);
#endif
    LE_COMPILE_BEGIN(LE_ENTER_PROMOTION);
    psyco_guard_push(&g);
    if (sigsetjmp(g.env, 0) != 0)
        psyco_guard_retry(&g); /* the parked state is still whole */
    if (flags & SkFlagPyObj)
        Py_INCREF((PyObject *)key);
    r = do_promotion_internal_body(fs, key, sk_new(key, flags));
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_PROMOTION);
#if LE_CODESTATS
    psyco_cs_promo_end();
#endif
    return r;
}

static code_t *
do_promotion_internal_body(rt_promotion_t *fs, long key, source_known_t *sk)
{
    code_t *start, *result;
    vinfo_t *v;
    PsycoObject *po = psyco_unpark(&fs->state, &v);
    mergepoint_t *mp;
    bool owned;

    /* fix the value of 'v' */
    CHKTIME(v->source, RunTime); /* from run-time to compile-time */
    owned = has_rtref(v->source);
#if REG_TOTAL > 0
    if (!RSOURCE_REG_IS_NONE(v->source)) {
        /* remove this value from 'po->regarray' */
        REG_NUMBER(po, RSOURCE_REG(v->source)) = NULL;
        SET_RUNTIME_REG_TO_NONE(v);
    }
#endif
    v->source = CompileTime_NewSk(sk);
    sk_mark_used(po, sk);
    /* compile from this new state, in which 'v' has been promoted to
       compile-time. */
    mp = psyco_exact_merge_point(po->pr.merge_points, po->pr.next_instr);

    start = psyco_code_open(&po->codelimit);
    {
        code_t *codeend;
        rt_local_buf_t *buf = (rt_local_buf_t *)start;
        code_t *code = (code_t *)(buf + 1);
        ALIGN_NO_FILL();
        result = code;
        buf = ((rt_local_buf_t *)code) - 1;

        po->code = insn_code_label(LE_CS_PROMO_COUNTER(result));
        /* a constant holds no run-time reference, so the one the value held
           is dropped on entry; the owner holds the object (sk_new) */
        if (owned)
            psyco_decref_c(po, (PyObject *)key);
        psyco_compatible_keep_facts = true;
        codeend = psyco_compile(po, mp, false);
        psyco_compatible_keep_facts = false;
        CLOSE_CODE("promotion", start, codeend);

        /* listed only once compiled */
        buf->next = fs->local_chained_list;
        buf->key = key;
        fs->local_chained_list = buf;
    }

    return result;
}

static code_t *
detected_megamorphic_body(rt_promotion_t *fs, bool interpret);
static __attribute__((noinline)) code_t *
detected_megamorphic_pyobj_site(rt_promotion_t *fs, bool interpret)
{
    code_t *r;
    le_compile_guard_t g;
#if LE_CODESTATS
    psyco_cs_promo_begin(fs->cs_site, -1, -1);
#endif
    LE_COMPILE_BEGIN(LE_ENTER_PROMOTION);
    psyco_guard_push(&g);
    if (sigsetjmp(g.env, 0) != 0)
        psyco_guard_retry(&g);
    r = detected_megamorphic_body(fs, interpret);
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_PROMOTION);
#if LE_CODESTATS
    psyco_cs_promo_end();
#endif
    return r;
}

/* 'interpret': the site has no generic path, so the version goes on in
   ceval from the promoting instruction (psyco_interp_pyobj_promotion) */
static code_t *
detected_megamorphic_body(rt_promotion_t *fs, bool interpret)
{
    code_t *start, *result;
    vinfo_t *v;
    PsycoObject *po = psyco_unpark(&fs->state, &v);
    mergepoint_t *mp;

    /* mark 'v' as megamorphic */
    CHKTIME(v->source, RunTime);
    v->source |= RunTime_Megamorphic;
    po->pr.go_interpreted = interpret;
    /* compile from this new state, in which 'v' has been flagged */
    mp = psyco_exact_merge_point(po->pr.merge_points, po->pr.next_instr);

    start = psyco_code_open(&po->codelimit);
    {
        code_t *codeend;
        rt_local_buf_t *buf = (rt_local_buf_t *)start;
        code_t *code = (code_t *)(buf + 1);
        ALIGN_NO_FILL();
        result = code;
        buf = ((rt_local_buf_t *)code) - 1;

        po->code = insn_code_label(LE_CS_PROMO_COUNTER(result));
        psyco_compatible_keep_facts = true;
        codeend = psyco_compile(po, mp, false);
        psyco_compatible_keep_facts = false;
        CLOSE_CODE("megamorphic", start, codeend);

        buf->next = fs->local_chained_list;
        buf->key = -1; /* use (PyObject*)(-1) as a marker */
        fs->local_chained_list = buf;
    }

    return result;
}

#if PROMOTION_FAST_COMMON_CASE
/* The polymorphic inline cache (idispatcher.h). */
#define CHAIN_ENTRY(fs, i) ((fs)->chain + (i) * PROMOTION_ENTRY_SIZE)
#define CHAIN_COUNT(fs) ((fs)->ncached > 0 ? (fs)->ncached - 1 : 0)

static void
promotion_chain_end(rt_promotion_t *fs)
{
    code_t *end = CHAIN_ENTRY(fs, CHAIN_COUNT(fs));
    if (fs->fallback != NULL)
        ipromotion_write_jump(end, fs->fallback);
    else
        ipromotion_write_miss(end, fs->reg, ((stub_record_t *)fs) - 1);
}

/* a chain with room for 'n' entries, holding the current ones */
static void
promotion_need_chain(rt_promotion_t *fs, int n)
{
    int i, cap;
    code_t *chain;
    if (fs->chain != NULL && fs->chaincap >= n)
        return;
    cap = fs->chain == NULL && n <= 1 ? 1 : PROMOTION_CHAIN_MAX;
    chain = stub_pool_reserve(PROMOTION_CHAIN_SIZE(cap));
    POOL_OWNER->stub_pool += PROMOTION_CHAIN_SIZE(cap);
    for (i = 0; i < CHAIN_COUNT(fs); i++) {
        long value;
        code_t *target;
        ipromotion_read_entry(CHAIN_ENTRY(fs, i), &value, &target);
        ipromotion_write_entry(chain + i * PROMOTION_ENTRY_SIZE, fs->reg,
                               value, target);
#if ALL_CHECKS
        {
            long v2;
            code_t *t2;
            ipromotion_read_entry(chain + i * PROMOTION_ENTRY_SIZE, &v2, &t2);
            extra_assert(v2 == value && t2 == target);
        }
#endif
    }
    fs->chain = chain;
    fs->chaincap = cap;
    promotion_chain_end(fs);
    /* overwrites the start of the site's own miss path */
    ipromotion_write_jump(fs->jump_if_equal_code + PROMOTION_ENTRY_SIZE,
                          chain);
}

/* the site's megamorphic version, or NULL */
static code_t *
promotion_mega_target(rt_promotion_t *fs)
{
    rt_local_buf_t *buf;
    for (buf = fs->local_chained_list; buf != NULL; buf = buf->next)
        if (buf->key == -1)
            return (code_t *)(buf + 1);
    return NULL;
}

/* the site jumps to 'target' when it sees 'value'. A full cache gives the
   slot of a value sent to the megamorphic version (a heap type at a
   static-type site) to a value with code of its own, and never the
   reverse: the evicted value would otherwise enter C at each run, or, once
   the chain ends in the megamorphic version, run that instead of its code
   (seen in a shapes benchmark). */
static void
promotion_cache_add(rt_promotion_t *fs, long value, code_t *target)
{
    if (fs->ncached == 0) {
        ipromotion_update_entry(fs->jump_if_equal_code, value, target);
        fs->ncached = 1;
    }
    else if (fs->ncached <= PROMOTION_CHAIN_MAX) {
        promotion_need_chain(fs, fs->ncached);
        ipromotion_write_entry(CHAIN_ENTRY(fs, fs->ncached - 1), fs->reg,
                               value, target);
        fs->ncached++;
        promotion_chain_end(fs);
    }
    else {
        /* full */
        code_t *mega = promotion_mega_target(fs);
        code_t *slot = NULL;
        int i;
        for (i = -1; mega != NULL && slot == NULL && i < PROMOTION_CHAIN_MAX;
             i++) {
            code_t *entry =
                i < 0 ? fs->jump_if_equal_code : CHAIN_ENTRY(fs, i);
            long v;
            code_t *t;
            ipromotion_read_entry(entry, &v, &t);
            if (t == mega)
                slot = entry;
        }
        if (slot == NULL) {
            if (target == mega)
                return; /* every slot holds a value with code of its own */
            slot = CHAIN_ENTRY(fs, PROMOTION_CHAIN_MAX - 1);
        }
        ipromotion_update_entry(slot, value, target);
    }
}

/* the site will compile no more values: every value the cache doesn't hold
   goes to 'mega' without entering C. The cache is first refilled with the
   values compiled here (at most MEGAMORPHIC_MAX, which it has room for),
   the most recently used inline, so that none of them runs the megamorphic
   version because a full cache had evicted it. */
static void
promotion_cache_fallback(rt_promotion_t *fs, code_t *mega)
{
    if (fs->fallback == NULL) {
        rt_local_buf_t *bufs[1 + PROMOTION_CHAIN_MAX];
        rt_local_buf_t *buf;
        int i, n = 0;
        fs->fallback = mega;
        for (buf = fs->local_chained_list; buf != NULL; buf = buf->next)
            if (buf->key != -1) {
                extra_assert(n < 1 + PROMOTION_CHAIN_MAX);
                if (n < 1 + PROMOTION_CHAIN_MAX)
                    bufs[n++] = buf;
            }
        fs->ncached = 0;
        for (i = 0; i < n; i++)
            promotion_cache_add(fs, bufs[i]->key, (code_t *)(bufs[i] + 1));
        if (fs->chain == NULL)
            promotion_need_chain(fs, 0);
        else
            promotion_chain_end(fs);
    }
}
#else
#define promotion_cache_add(fs, value, target) \
    do {                                       \
    } while (0)
#define promotion_cache_fallback(fs, mega) \
    do {                                   \
    } while (0)
#endif

/* NOTE: the following two functions must be as fast as possible, because
   they are called from the run-time code even during normal (non-compiling)
   execution. */
static code_t *
do_promotion_long(rt_promotion_t *fs, long value)
{
#if LE_CODESTATS
    psyco_cs_promo_miss(fs->cs_site);
#endif
    /* need a PyObject* key for the local cache dictionary */
    code_t *result;
    long key1 = value;

    /* have we already seen this value? */
    QUICK_LOOKUP_PROMOTION_VALUE(fs, key1, result);
    if (result == NULL) {
        /* no -> we must build new code */
        result = do_promotion_internal(fs, key1, SkFlagFixed);
    }
    /* done -> jump to the codebuf */
    promotion_cache_add(fs, value, result);
    return result;
}

static code_t *
do_promotion_pyobj(rt_promotion_t *fs, PyObject *key)
{
#if LE_CODESTATS
    psyco_cs_promo_miss(fs->cs_site);
#endif
    code_t *result;
    long key1 = (long)key;

    /* have we already seen this value? */
    QUICK_LOOKUP_PROMOTION_VALUE(fs, key1, result);
    if (result == NULL) {
        /* no -> we must build new code */
        result = do_promotion_internal(fs, key1, SkFlagFixed | SkFlagPyObj);
    }
    /* done -> jump to the codebuf */
    promotion_cache_add(fs, key1, result);
    return result;
}

PSY_INLINE code_t *
promotion_pyobj_mega(rt_promotion_t *fs, PyObject *key, bool interpret)
{
#if LE_CODESTATS
    psyco_cs_promo_miss(fs->cs_site);
#endif
    code_t *result;
    long key1 = (long)key;

    /* have we already seen this value? */
    QUICK_LOOKUP_PROMOTION_VALUE(fs, key1, result);
    if (result == NULL) {
        /* no -> did we already promote many objects here? */
        rt_local_buf_t *megabuf = NULL;
        if (lookup_count_previous_values(fs, &megabuf) >= MEGAMORPHIC_MAX) {
            /* yes -> stop specializing */
            if (megabuf == NULL)
                result = detected_megamorphic_pyobj_site(fs, interpret);
            else
                result = (code_t *)(megabuf +
                                    1); /* mega version already compiled */
            promotion_cache_fallback(fs, result);
            return result;
        }
        /* no -> we must build new code */
        result = do_promotion_internal(fs, key1, SkFlagFixed | SkFlagPyObj);
    }
    /* done -> jump to the codebuf */
    promotion_cache_add(fs, key1, result);
    return result;
}

/* psyco_fused_pyobj_promotion: as do_promotion_pyobj, and a value past
   MEGAMORPHIC_MAX stops the generator expression's fusing */
static code_t *
do_promotion_pyobj_fused(rt_promotion_t *fs, PyObject *key)
{
#if LE_CODESTATS
    psyco_cs_promo_miss(fs->cs_site);
#endif
    code_t *result;
    long key1 = (long)key;

    QUICK_LOOKUP_PROMOTION_VALUE(fs, key1, result);
    if (result == NULL) {
        rt_local_buf_t *megabuf = NULL;
        if (lookup_count_previous_values(fs, &megabuf) >= MEGAMORPHIC_MAX) {
            parked_state_t *ps = &fs->state;
            PyObject *mps = ps->live != NULL ? ps->live->pr.merge_points
                                             : ps->merge_points;
            *psyco_fuse_stopped(mps) = 1;
        }
        result = do_promotion_internal(fs, key1, SkFlagFixed | SkFlagPyObj);
    }
    promotion_cache_add(fs, key1, result);
    return result;
}

static code_t *
do_promotion_pyobj_mega(rt_promotion_t *fs, PyObject *key)
{
    return promotion_pyobj_mega(fs, key, false);
}

static code_t *
do_promotion_pyobj_interp(rt_promotion_t *fs, PyObject *key)
{
    return promotion_pyobj_mega(fs, key, true);
}

/* psyco_static_type_promotion: as above, but a value that is not a static
   type goes to the megamorphic version without being made a constant.
   That version is valid for any value, so the inline fast case may point
   to it even if the object dies and its address is reused. */
static code_t *
do_promotion_static_type(rt_promotion_t *fs, PyObject *key)
{
#if LE_CODESTATS
    psyco_cs_promo_miss(fs->cs_site);
#endif
    code_t *result;
    long key1 = (long)key;

    QUICK_LOOKUP_PROMOTION_VALUE(fs, key1, result);
    if (result == NULL) {
        rt_local_buf_t *megabuf = NULL;
        int count = lookup_count_previous_values(fs, &megabuf);
        if (!PyType_Check(key) ||
            (((PyTypeObject *)key)->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
            count >= MEGAMORPHIC_MAX) {
            if (megabuf == NULL)
                result = detected_megamorphic_pyobj_site(fs, false);
            else
                result = (code_t *)(megabuf + 1);
            if (count >= MEGAMORPHIC_MAX) {
                /* from now on every new value goes there */
                promotion_cache_fallback(fs, result);
                return result;
            }
        }
        else {
            result =
                do_promotion_internal(fs, key1, SkFlagFixed | SkFlagPyObj);
        }
    }
    promotion_cache_add(fs, key1, result);
    return result;
}

/* the site's state, when its owner dies */
static void
promotion_release(void *closure)
{
    psyco_park_release(&((rt_promotion_t *)closure)->state);
}

DEFINEFN
code_t *
psyco_finish_promotion(PsycoObject *po, vinfo_t *fix, int pflags)
{
    rt_promotion_t *fs;
    void *do_promotion;
    code_t *code;

    /* a promotion ends the block, so a replay stops before one; were it not
       so, its record would outlive the replay */
    extra_assert(!is_respawning(po));
    /* we remove the non-compile-time array values from 'fix' */
    if (fix->array != NullArray)
        remove_non_compiletime(fix, po);
    /* write the code that calls the proxy 'do_promotion' */
    switch (pflags) {
        case 0:
            do_promotion = &do_promotion_long;
            break;
        case PFlagPyObj:
            do_promotion = &do_promotion_pyobj;
            break;
        /*case PFlagMegamorphic: ... */
        case PFlagPyObj | PFlagMegamorphic:
            do_promotion = &do_promotion_pyobj_mega;
            break;
        case PFlagPyObj | PFlagMegamorphic | PFlagStaticType:
            do_promotion = &do_promotion_static_type;
            break;
        case PFlagPyObj | PFlagMegamorphic | PFlagInterpret:
            do_promotion = &do_promotion_pyobj_interp;
            break;
        case PFlagPyObj | PFlagFused:
            do_promotion = &do_promotion_pyobj_fused;
            break;
        default:
            psyco_fatal_msg("bad pflags");
            return NULL;
    }
#if LE_CODESTATS
    code_t *cs_start = po->code;
#endif
    fs = (rt_promotion_t *)ipromotion_finish(po, fix, do_promotion,
                                             sizeof(rt_promotion_t));
#if LE_CODESTATS
    fs->cs_site = psyco_cs_promo_site(po, pflags, po->code - cs_start);
#endif

    /* fill in the constant structure that 'do_promotion' will get as parameter
     */
    clear_tmp_marks(&po->vlocals);
    psyco_assert_coherent(po);
    fs->local_chained_list = NULL;
    psyco_stub_set_release(fs, &promotion_release);
    code = po->code;
    psyco_park(&fs->state, po, fix);
    return code;
}

/*****************************************************************/
/***   Un-Promotion from non-fixed compile-time into run-time  ***/

static void
array_remove_vinfo(vinfo_array_t *array, vinfo_t *vi)
{
    int i = array->count;
    while (i--) {
        vinfo_t *a = array->items[i];
        if (a != NULL) {
            if (a == vi) {
                extra_assert(a->refcount > 1);
                vinfo_decref(a, NULL); /* other references expected */
                array->items[i] = NULL;
            }
            else if (a->array != NullArray)
                array_remove_vinfo(a->array, vi);
        }
    }
}

static void
array_remove_inside_ct(vinfo_array_t *array, vinfo_t *vi)
{
    int i = array->count;
    while (i--) {
        vinfo_t *a = array->items[i];
        if (a != NULL && a->array != NullArray) {
            if (is_compiletime(a->source))
                array_remove_vinfo(a->array, vi);
            else
                array_remove_inside_ct(a->array, vi);
        }
    }
}

DEFINEFN
void
psyco_unfix(PsycoObject *po, vinfo_t *vi)
{
    /* Convert 'vi' from compile-time to run-time variable. */
    vinfo_t *newvi;
    source_known_t *sk;
    CHKTIME(vi->source, CompileTime);

    /*printf("psyco_unfix(%p, %p, %p)\n", po, po->code, vi);*/

    /* forget all sub-items of the compile-time 'vi',
       which should all be compile-time */
    if (vi->array != NullArray) {
        array_delete(vi->array, po);
        vi->array = NullArray;
    }

    /* A particular case that *does* occur:
       'vi' was always selected as a compile-time vinfo_t not within
       another compile-time vinfo_t's subarray; but it could
       actually appear more than once in po->vlocals, and another
       occurrence could be inside another compile-time vinfo_t.
       As compile-time vinfo_ts are not allowed to contain anything
       but compile-time vinfo_ts, unfixing 'vi' would break it.
       In this situation we remove the faulty occurrences. */
    if (vi->refcount > 1)
        array_remove_inside_ct(&po->vlocals, vi);
    assert_array_contains_nonct(&po->vlocals, vi);

    newvi = make_runtime_copy(po, vi);
    /* make_runtime_copy() never fails for compile-time sources */
    extra_assert(newvi != NULL);

    /* release 'vi->source' and move 'newvi->source' into it. SkFlagPyObj
       stays: the object is never released (code holds its address and
       the owner holds the object while the code lives), and the
       source_known_t may be shared with a snapshot or a callee's frame,
       whose constant it still is */
    sk = CompileTime_Get(vi->source);
    sk_decref(sk);
    vinfo_move(po, vi, newvi);
}
