/* Including this file results in all headers Objects/xxx.h
   being included, so that it has roughly the same result
   for Psyco as a "#include <Python.h>" has for Python:
   including all headers extension modules generally need.

   This file is moreover used internally by psyco.c. */

#ifndef PSYCO_INITIALIZATION

#include "Objects/pabstract.h"
#include "Objects/pboolobject.h"
#include "Objects/pclassobject.h"
#include "Objects/pdescrobject.h"
#include "Objects/pdictobject.h"
#include "Objects/pfloatobject.h"
#include "Objects/pfuncobject.h"
#include "Objects/piterobject.h"
#include "Objects/plistobject.h"
#include "Objects/plongobject.h"
#include "Objects/pmethodobject.h"
#include "Objects/pobject.h"
#include "Objects/pstructmember.h"
#include "Objects/ptupleobject.h"

#else /* if PSYCO_INITIALIZATION */
#undef PSYCO_INITIALIZATION

#include <iinitialize.h> /* processor-specific initialization */

/* internal part for psyco.c */
#if ALL_STATIC
#include "Modules/pmath.c"
#include "Objects/pabstract.c"
#include "Objects/pclassobject.c"
#include "Objects/pdescrobject.c"
#include "Objects/pdictobject.c"
#include "Objects/pfloatobject.c"
#include "Objects/pfuncobject.c"
#include "Objects/piterobject.c"
#include "Objects/plistobject.c"
#include "Objects/plongobject.c"
#include "Objects/pmethodobject.c"
#include "Objects/pobject.c"
#include "Objects/pstructmember.c"
#include "Objects/ptupleobject.c"
#include "Objects/punicodeobject.c"
#include "Python/frames.c"
#include "Python/pbltinmodule.c"
#include "Python/pycompiler.c"
#include "codegen.c"
#include "codemanager.c"
#include "dispatcher.c"
#include "eval_hook.c"
#include "mergepoints.c"
#include "psyfunc.c"
#include "vcompiler.c"
#else  /* if !ALL_STATIC */
EXTERNFN void psyco_compiler_init(void);    /* vcompiler.c */
EXTERNFN void psyco_codegen_init(void);     /* codegen.c */
EXTERNFN void psyco_pycompiler_init(void);  /* Python/pycompiler.c */
EXTERNFN void psyco_frames_init(void);      /* Python/frames.c */
EXTERNFN void psyco_bltinmodule_init(void); /* Python/pbltinmodule.c */
EXTERNFN void psy_object_init(void);        /* Objects/pobject.c */
EXTERNFN void psy_classobject_init(void);   /* Objects/pclassobject.c */
EXTERNFN void psy_descrobject_init(void);   /* Objects/pdescrobject.c */
EXTERNFN void psy_dictobject_init(void);    /* Objects/pdictobject.c */
EXTERNFN void psy_floatobject_init(void);   /* Objects/pfloatobject.c */
EXTERNFN void psy_funcobject_init(void);    /* Objects/pfuncobject.c */
EXTERNFN void psy_iterobject_init(void);    /* Objects/piterobject.c */
EXTERNFN void psy_listobject_init(void);    /* Objects/plistobject.c */
EXTERNFN void psy_longobject_init(void);    /* Objects/plongobject.c */
EXTERNFN void psy_methodobject_init(void);  /* Objects/pmethodobject.c */
EXTERNFN void psy_rangeobject_init(void);   /* Objects/prangeobject.c */
EXTERNFN void psy_tupleobject_init(void);   /* Objects/ptupleobject.c */
EXTERNFN void psy_unicodeobject_init(void); /* Objects/punicodeobject.c */
EXTERNFN void psyco_initmath(void);         /* Modules/pmath.c */
EXTERNFN int ni_eval_hook_init(void);       /* eval_hook.c */
EXTERNFN void codemanager_init();           /* codemanager.c */
EXTERNFN void psycofunction_init();         /* psyfunc.c */
#endif /* !ALL_STATIC */

PSY_INLINE void initialize_all_files(void) {
  initialize_processor_files();
  psyco_compiler_init();    /* vcompiler.c */
  psyco_codegen_init();     /* codegen.c */
  psyco_pycompiler_init();  /* Python/pycompiler.c */
  psyco_frames_init();      /* Python/frames.c */
  psyco_bltinmodule_init(); /* Python/pbltinmodule.c */
  psy_object_init();        /* Objects/pobject.c */
  psy_classobject_init();   /* Objects/pclassobject.c */
  psy_descrobject_init();   /* Objects/pdescrobject.c */
  psy_dictobject_init();    /* Objects/pdictobject.c */
  psy_floatobject_init();   /* Objects/pfloatobject.c */
  psy_funcobject_init();    /* Objects/pfuncobject.c */
  psy_iterobject_init();    /* Objects/piterobject.c */
  psy_listobject_init();    /* Objects/plistobject.c */
  psy_longobject_init();    /* Objects/plongobject.c */
  psy_methodobject_init();  /* Objects/pmethodobject.c */
  psy_rangeobject_init();   /* Objects/prangeobject.c */
  psy_tupleobject_init();   /* Objects/ptupleobject.c */
  psy_unicodeobject_init(); /* Objects/punicodeobject.c */
  psyco_initmath();         /* Modules/pmath.c */
  ni_eval_hook_init();      /* eval_hook.c */
  codemanager_init();       /* codemanager.c */
  psycofunction_init();     /* psyfunc.c */
}

#endif /* PSYCO_INITIALIZATION */
