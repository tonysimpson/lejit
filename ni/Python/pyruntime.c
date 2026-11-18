/* The only file built with Py_BUILD_CORE, for what the runtime state
   (_PyRuntime, in the internal headers) holds, which the extension headers
   don't describe. Where the current thread state and eval_breaker are,
   and the copy of what ceval does when eval_breaker is set, are
   mechanisms chosen from the target's source: this file is only
   their home. Not #included into ni.c under ALL_STATIC: setup.py always
   builds it on its own. */
#define Py_BUILD_CORE
#include <Python.h>
#include "internal/pystate.h"
#include "frameobject.h"
#include "opcode.h"
#include "runtime_layouts_gen.h"

/* ni_tstate_current_address */
#include "mechanism_tstate_runtime_gen.h"
/* ni_eval_breaker_address, ni_eval_breaker_handler,
   ni_eval_breaker_at_entry */
#include "mechanism_eval_breaker_runtime_gen.h"
