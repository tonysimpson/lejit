#if ALL_STATIC
#include "iprocessor.c"
#include "idispatcher.c"
#include "iencoding.c"
#include "ipyencoding.c"
#else
EXTERNFN void psyco_processor_init(void); /* iprocessor.c */
#endif

PSY_INLINE void initialize_processor_files(void) {
  psyco_processor_init();	/* iprocessor.c */
}
