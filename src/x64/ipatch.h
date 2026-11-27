/*
 * Replacing an exported C function of another library at its entry, for
 * every caller: its first bytes become a jump to the replacement, and the
 * instructions they held are copied, moved to run elsewhere, in front of a
 * jump back past them, so that the original can still be called.
 */
#ifndef _IPATCH_H
#define _IPATCH_H

#include "../psyco.h"

/* Every call of the function at 'fn' runs 'by' from now on. Returns what
   to call for the original, or NULL with nothing changed where it can't
   be done: 'fn' (a function's entry) returns within the jump's bytes,
   its first instructions aren't ones this file knows how to move, no page
   can be mapped near it, or its page can't be made writable. Called with
   the GIL held, before any other thread could be running 'fn's first
   bytes. */
EXTERNFN void *
le_patch_entry(void *fn, void *by);

/* As le_patch_entry, but each call jumps to 'by', with the registers and
   stack as the caller left them for the original: 'by' must change no
   register that a caller could count on the original leaving alone
   (ipatch.c:le_patch_call says why a caller might). */
EXTERNFN void *
le_patch_entry_jump(void *fn, void *by);

#endif /* _IPATCH_H */
