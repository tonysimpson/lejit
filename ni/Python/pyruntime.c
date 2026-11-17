/* The only file built with Py_BUILD_CORE, for what 3.7 keeps in _PyRuntime
   (Include/internal/pystate.h), which the extension headers don't
   describe. Not #included into ni.c under ALL_STATIC: setup.py always
   builds it on its own. */
#define Py_BUILD_CORE
#include <Python.h>
#include "internal/pystate.h"

/* where PyThreadState_GET() reads the current thread state: an
   _Py_atomic_address, which a plain load reads */
void *ni_tstate_current_address(void) {
  return &_PyRuntime.gilstate.tstate_current;
}
